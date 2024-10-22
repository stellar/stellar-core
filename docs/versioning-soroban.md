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
    tree of dependencies on a protocol version. We do this by compiling multiple
    _full copies_ of the soroban host into stellar-core.

In the worst case, we will wind up compiling-in as many copies of soroban as
there are protocols. This is actually not _so_ bad since each copy of soroban
is only a few hundred KiB of object code.

But: we can do better. The key observation is that once a given protocol `N` is
_past_ and the network is no longer recording ledgers marked with `N`, we have a
fixed set of ledgers labeled with `N` that we need to maintain accurate replay
of; any _observable_ differences in soroban versions that was, in practice, not
_actually observed_ by any transactions in that range of ledgers can be ignored.

And "ignored" here means that we can retire some extra copies of soroban,
post-upgrade. Remove them from stellar-core. Specifically if the historical
recording of "all protocol `N` ledgers" replays correctly on soroban `N+1` then
there is no need to keep soroban `N` compiled-in to stellar-core anymore,
soroban `N+1` will suffice.

## Protocol upgrade process

**Note:** We used to follow a more rigid and operationally subtle plan with only
1 or at most 2 versions of soroban -- symbolically called `curr` and `prev` --
compiled-in at any moment, with a complex sequence of upgrade steps to enable,
upgrade, rename, and disable the symbolic names. As of protocol 22 we shifted to
the simpler and more general plan of just keeping around as many sorobans as
needed, identified by protocol number rather than symbolic name. If you see the
terms `curr` and `prev` kicking around, those are mostly no longer relevant
terms (with the exception of a module alias inside `lib.rs`, see below.)

We follow this process for protocol upgrades, taking as an illustrative example
the upgrade from protocol 22 to 23:

  1. Recall that when the network is on protocol `22`, it means the versions of
    stellar-core and soroban have versions like `22.x.y` and tags like
    `v22.x.y`, i.e. with `22` as their major version number.

  2. We therefore start by tagging `soroban-env-host` at `v23.0.0` and releasing
     the crate at `23.0.0`.

  3. We then update stellar-core's `Config::CURRENT_LEDGER_PROTOCOL_VERSION` to
     `23` and tag the version as `v23.0.0`. This will cause
     `src/main/StellarCoreVersion.cpp` to be regenerated and stellar-core should
     think of itself as speaking protocol 23.

  4. We then add a new submodule to stellar-core under `src/rust/soroban/p23`.
     This submodule has _another copy_ of the `soroban-env-host` repository,
     but checked out at the `v23.0.0` tag. We do this with something like

       - `mkdir src/rust/soroban/p23`
       - `git submodule add https://github.com/stellar/rs-soroban-env src/rust/soroban/p23`
       - `cd src/rust/soroban/p23`
       - `git checkout v23.0.0`

  4. We wire that new protocol into `src/rust/src/lib.rs` by copying the
     existing highest-numbered protocol-pecific module, say `mod p22 { ... }`
     that exists inline in that file, to a new copy say `mod p23 { ... }`.

   5. We also update the module alias `soroban_curr`, by changing a line like
      `use p22 as soroban_curr` to say `use p23 as soroban_curr`.

   6. We also copy and paste a line like
      `proto_versioned_functions_for_module!(p22),` into a new line like
      `proto_versioned_functions_for_module!(p23),` which will register the new
      module. The module self-identifies the protocol it's responsible for
      handling.

   7. We then copy the "expected dependency tree" file from protocol 22 to 23:

      - `cp src/rust/src/dep-trees/p22-expect.txt src/rust/src/dep-trees/p23-expect.txt`

   8. We then attempt to rebuild. The rebuild will probably fail because the
      _actual_ dependencies of the p23 soroban are _different_ from those listed
      in the `p23-expect.txt` file. These files are just here to ensure we
      notice unintentional changes to dependencies, and the build system should
      display the differences, requiring manual acceptance. If we are satisfied
      with the displayed diffs, we just copy the actual file to the expected
      file:

      - `cp src/rust/src/dep-trees/p23-actual.txt src/rust/src/dep-trees/p23-expect.txt`

      And then rebuild.

   9. Technically that's it! We should have a copy of stellar-core that will run
      soroban 22.x.y when given a protocol 22 ledger, and soroban 23.0.0 when
      given a protocol 23 ledger. There is one final step to consider later.

   10. _After the protocol 23 upgrade_, we can try _commenting out_ the
      `proto_versioned_functions_for_module!(p22),` line to see if we can still
      replay the recorded history of protocol 22 (which is set in stone now) on
      soroban 23.x.y. If so, we can just delete that line and the corresponding
      git submodule and dep-tree files: p22 is subsumed into p23. But it might
      not work, again for two reasons:

        - The p23 module might explicitly reject replay of protocol 22 (if there
          was a major protocol change and we literally broke or removed support
          for protocol 22 semantics, intentionally, from the p23 module). In
          this case we have to keep the p22 module around forever.

        - The p23 module might accidentally _diverge_ during replay. In this
          case we have to look at the specifics. If there's a _minor_ change we
          can make to p23 in the future to make it capable of faithfully
          replaying p22, we can make that change at our leisure and try again.
          But if the change is really intrusive, it might be easier to just keep
          p22 around forever anyways.

## Rust, Cargo, versions, submodules, rlibs, and dep-tree files

This seciton is optional details about implementation technique for anyone
surprised by the build infrastructure, or the fact that the lib.rs file doesn't
seem to work in their IDE quite right, or surprised by the dep-tree files in the
steps above.

We are leveraging Rust's support for linking together multiple copies of "the
same" library (soroban) with different versions, but we are doing so somewhat
against the grain of how cargo normally wants to do it.

Do do this "the normal way", we would just list the different versions of the
soroban crate in `Cargo.toml`, and then when we built it cargo would attempt to
resolve all the dependencies and transitive-dependencies of all those soroban
versions into a hopefully-minimal set of crates and download, compile and link
them all together.

This has one minor and one major problem:

  1. The minor problem is that when you change anything in any dependency, cargo
     tends to re-resolve stuff. Resolving is when it converts version
     _requirements_ (in `Cargo.toml`) to _specific versions_ (in `Cargo.lock`),
     and all the resolutions of all the transitive dependency requirements of
     all the versions of soroban linked into stellar-core wind up mixed together
     in a single `Cargo.lock` file, which makes it hard to tell when
     dependencies of any subtree change. We dealt with this initially by using a
     tool that could separate-out and print independent subtrees within a
     `Cargo.lock` file, and we stored and compared those in version control,
     which mostly meant we could catch changes. But it did not fix the major
     problem.

  2. The major problem is that when we want to take a possibly-breaking update
     -- say above when we add a new protocol p23 -- if that update depends on
     some 3rd party crate say foo 0.2 and we already have a dependency in our
     p22 module on foo 0.1, cargo will bump _both_ to foo 0.2, which _changes_
     the semantics of the p22 module.

       - We initially though a way out of this is to add redundant exact-version
         dependencies (like `foo = "=0.2"`) to `Cargo.toml` for
         `soroban-env-host` but there turn out to be both a minor and a major
         problem with that too.

       - The minor problem is that it is unpopular with downstream users (it
         limits the set of libraries they can use soroban with).

       - The major problem is that cargo, as a matter of semver-enforcement
         policy (it's not a rust language limitation), only allows you to link
         together multiple versions of a crate if they are in different semver
         "compatibility ranges" (see
         https://doc.rust-lang.org/cargo/reference/resolver.html). This means
         the differing versions need to differ significantly. The must differ by
         at least:
         - A full major version, when their major number is >0
         - A full minor version, when their major number is 0

      - We found ourselves repeatedly dealing with crates that we _wanted_ to
        multiply version, to isolate from unintentional changes, but that were
        inside the same semver compatibility range. So cargo literally wouldn't
        let us.

As a consequence of these issues, we decided to take a more radical approach,
and _not involve cargo_ in the management of the multiple versions.

Instead we:

  - Build each soroban library into an `rlib`, essentially a static library,
    using a separate invocation of `cargo build --locked` against the separate
    `Cargo.lock` file held in each soroban submodule. They each get locked
    separately and build in ignorance of one another.

  - Actually pass an additional `-Cmetadata=p23` or `-Cmetadata=p22` or whatever
    flag to each build, to further ensure non-collision of any transitive deps
    that would otherwise risk colliding due to non-reproducible build issues
    (this is amazing but necessary, see `src/Makefile.am` for details).

  - _Manually_ pass those `rlib` files as separate `--extern` dependency
    definitions to `cargo rustc` when building the `librust_stellar_core.a`
    stellar-core crate that combines the multiple sorobans together.

  - Just to be on the safe side, still render the content of the lockfiles of
    each submodule as a `p22-expect.txt` or `p23-expect.txt` file that we store
    in the outer stellar-core tree, and compare them during the build to make
    sure we only take updates we've manually looked at and signed off on.

That last part is _somewhat_ vestigial from our earlier work with the tool that
printed the subtrees of the combined `Cargo.lock` file, but it doesn't hurt to
manually review all such changes, so we left it in.

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

