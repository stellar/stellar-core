# Protocol version upgrades

The stellar "protocol version" (a.k.a. "ledger version number") is extended to
include all _observable_ changes to soroban:

  - Changes to the set of host functions and the data model conveyed over the
    soroban environment interface.

  - Intentional and observable changes to the semantics of soroban functions,
    eg. to fix bugs.

  - Unintentional but still observable changes to the semantics of soroban
    functions.

The third category is new: we might receive updates to soroban (or its
transitive dependencies) that we don't expect. These are harder to deal with
than normal stellar-core changes:

  - We don't know _when_ an unintentional but observable change is happening, so
    we have to over-approximate by doing more frequent "protocol upgrades",
    basically any time we're not 100% certain a change is non-observable.

  - We don't know _where_ an unintentional but observable change happens in the
    set of transitive dependencies of soroban, so we have to gate the _entire_
    tree of dependencies on a protocol version, at least during an upgrade,
    keeping two copies of soroban-and-its-dependencies compiled-in at once.

There is one saving grace, however: once a given protocol version `N` is _past_
and the network is no longer recording ledgers marked with `N`, we have a fixed
set of ledgers labeled with `N` that we need to maintain accurate replay of; any
differences in soroban versions not-observed by transactions in that range of
ledgers can be ignored. So while we need to maintain two separate copies of
soroban compiled-in to stellar-core _during_ an upgrade, we can expire one of
them _after_ the upgrade by ensuring that the new code can replay the old (now
complete) range of ledgers faithfully.

## Protocol upgrade process

We follow this process for protocol upgrades:

  1. Before the upgrade, the network is on protocol `N`. To keep things simple,
     we require that both stellar-core and soroban give their releases version
     numbers like `N.x.y` when `N` is their maximum supported protocol. The
     release major version number is the max supported protocol version.

  2. To upgrade to `N+1` we'll be producing a new soroban version `(N+1).0.0`
     and a new stellar-core `(N+1).0.0`, and the stellar-core will set its
     `Config::CURRENT_LEDGER_PROTOCOL_VERSION` to `N+1`.

  3. We configure the build of stellar-core v(N+1).0.0 as a "transitionary
     build" that has _both_ versions of soroban in it. In the `Cargo.toml` of
     stellar-core, we copy the `N.x.y` version number and git hash from the
     existing dependency named `soroban-env-host-curr` to the optional
     dependency named `soroban-env-host-prev`, then set `soroban-env-host-curr`
     to `(N+1).0.0` and the appropriate git hash, and configure stellar-core
     with `--enable-protocol-upgrade-via-soroban-env-host-prev`, which will
     cause the rust build phase to use `--feature=soroban-env-host-prev`.
     We will also have to update both pinned dependency tree files as
     described below in "Pinned soroban dependency trees".

  4. We deploy this dual-version "transitionary build" to the network. This must
     be the first version with support for `N+1`, and since it was built with
     support for `soroban-env-host-prev` it will continue to run ledgers marked
     as protocol `N` on the previous soroban version `N.x.y`, but run all _other_
     ledgers -- both older and newer than protocol `N` -- on the new soroban
     version `(N+1).0.0`. This allows early testing of `(N+1).0.0` by replaying
     it against all of history _before_ protocol `N`.

  5. We do the consensus protocol upgrade on this "transitionary build"
     version. We define the validity condition for a protocol upgrade such that
     stellar-core is only _willing_ to vote for a soroban-era protocol upgrade
     when configured with two versions of soroban built-in, and such a
     configuration will cut over instantaneously from old soroban `N.x.y` to new
     soroban `(N+1).0.0` on upgrade.

  6. Once the upgrade is complete, we prepare the _next_ version of stellar-core
     _without_ the transitionary version. That is, we build without
     `--feature=soroban-env-host-prev`. In order to be sure that it is safe to
     expire that version, we first replay all ledgers from `N` and before on
     the current soroban `(N+1).0.0` and see if they replay correctly. If they
     do not, we add backward compatibility code to soroban and do a point
     release like `(N+1).0.1` that can replay correctly, and bump the version
     in stellar-core's `Cargo.toml` to `(N+1).0.1`.

## Pinned soroban dependency trees

At any point we may have one _or two_ versions of soroban built into
stellar-core (see below). This is confusing and error-prone. It is easy to
update one version and accidentally cause cargo to "re-resolve" version specs in
dependencies or transitive dependencies to new versions, and accidentally cause
changes.

To help cross-check versions and reduce accidental changes, we embed (in all
builds) a copy of both `Cargo.lock` and one or more files showing the full,
expected dependency tree of either or both of the embedded copies of
soroban. These files are stored in git adjacent to `Cargo.toml` and are called
`host-dep-tree-curr.txt` and `host-dep-tree-prev.txt`. Any time we change the
version(s) specified in `Cargo.toml` we will likely need to update one or
another of these files.

The easiest way to update the files is just to run `stellar-core version`
without updating the files: it will exit with an error that shows the file
content that was expected and the content that was found. Copy the "found"
content into the expected file and rebuild, and it should work. The point of
this step is to catch accidental errors, so we carefully review the changed
dependency tree before we commit it, and ensure it contains exactly the changes
we expect!

If we want to manually regenerate the files, we can also use the cli tool
`cargo-lock` by hand (this is the crate stellar-core uses), for example:

    $ cargo install --git https://github.com/rustsec/rustsec \
        --rev dcc72b697e10a2d1e3d1ce70d7d7b0d5cbc41dc4 \
	--locked --features cli cargo-lock
    $ cargo lock tree --exact soroban-env-host@0.0.15


## Protocol version, release version and contract cross-checks

We configure soroban, stellar-core and contracts built for soroban to check
certain consistency requirements around protocol and release versions:

  1. Stellar-core's max protocol (`Config:::CURRENT_LEDGER_PROTOCOL_VERSION`)
     and its major version number must be the same.

  2. Stellar-core's and soroban's major version numbers are the same.

  3. Every contract's WASM has an embedded minimum protocol version number,
     and only run contracts with current or older protocols than the max
     protocol supported by the current host.

There are three caveats to the above:

  1. When stellar-core is not on a _release_ version (that is when its version
     number does not look exactly like `vNN.x.y` or `vNN.x.y-rcN`) we assume
     it's a development build, and relax the first criterion, only warning.

  2. When soroban is in a pre-1.0 state (when its major version is 0) we
     relax the second criterion, only warning.

  3. When soroban is in a pre-1.0 state we also _tighten_ the third criterion,
     specifying both a 32-bit protocol version and a secondary 32-bit
     "pre-release version" into a composite 64-bit "interface version number"
     stored in the WASM (the low 32 bits are the pre-release number, the high 32
     bits are the protocol number). While the protocol version comparison allows
     contracts compiled with old protocols to run on new hosts, the pre-release
     number has to match _exactly_ for a contract to run. This allows forcing
     recompilation of pre-release contracts by bumping the pre-release version.
     After soroban 1.0 is released, in any release with a nonzero major version
     number, the pre-release version component of the interface version must
     always be zero.

