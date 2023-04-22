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

Specifically, we follow this process for protocol upgrades:

  1. Before the upgrade, the network is on protocol `N`. To keep things simple,
     we require that both stellar-core and soroban give their releases version
     numbers like `N.x.y` when `N` is their maximum supported protocol. The
     release major version number is the max supported protocol version.

  2. To upgrade to `N+1` we'll be producing a new soroban version `(N+1).0.0`
     and a new stellar-core `(N+1).0.0`, and the stellar-core will set its
     `Config::CURRENT_LEDGER_PROTOCOL_VERSION` to `N+1`.

  3. In the `Cargo.toml` of stellar-core, we copy the `N.x.y` version number and
     git hash from the existing dependency named `soroban-env-host-curr` to the
     optional dependency named `soroban-env-host-prev`, then set
     `soroban-env-host-curr` to `(N+1).0.0` and the appropriate git hash, and
     build a with `--feature=soroban-env-host-prev`.

  4. We deploy this dual-version "transitionary build" to the network. This must
     be the first version with support for `N+1`, and since it was built with
     support for `soroban-env-host-prev` it will continue to run ledgers marked
     as protocol `N` on the previous soroban version `N.x.y`, but run all _other_
     ledgers -- both older and newer than protocol `N` -- on the new soroban
     version `(N+1).0.0`. This allows early testing of `(N+1).0.0` by replaying
     it against all of history _before_ protocol `N`.

  5. We do the consensus protocol upgrade on this "transitionary build"
     version. We define the validity condition for a protocol upgrade such that
     stellar-core is only _willing_ to vote for a protocol upgrade when
     configured with two versions of soroban built-in, and such a configuration
     will cut over instantaneously from old soroban `N.x.y` to new soroban
     `(N+1).0.0` on upgrade.

  6. Once the upgrade is complete, we prepare the _next_ version of stellar-core
     _without_ the transitionary version. That is, we build without
     `--feature=soroban-env-host-prev`. In order to be sure that it is safe to
     expire that version, we first replay all ledgers from `N` and before on
     the current soroban `(N+1).0.0` and see if they replay correctly. If they
     do not, we add backward compatibility code to soroban and do a point
     release like `(N+1).0.1` that can replay correctly, and bump the version
     in stellar-core's `Cargo.toml` to `(N+1).0.1`.

# Protocol versions and pre-release flavors

We configure soroban and stellar-core to check certain consistency requirements
around protocol versions:

  - Stellar-core's max protocol number and its major version number are the same.

  - Stellar-core's and soroban's major version numbers are the same.

Additionally, every contract's WASM has an embedded minimum protocol version
number, so that we do not run contracts on incompatible hosts.

However, while we're developing soroban (before release to a public network and
commitment to backward-compatible protocol releases) we also want to be able to
_break_ compatibility (forcing recompiles of contracts) when we change the
soroban env interface incompatibly, by incrementing some number in the env.

As a result, we adopt the convention (while soroban is being developed) that
only the low 16 bits of the protocol version number identify the protocol, and
the high 16 bits identify a pre-release flavor that must match _exactly_ between
contract and environment. When reasoning about protocol numbers we only look at
the low 16 bits. When deciding whether a contract is compatible, we also examine
the pre-release flavor and check that it is identical.

Once soroban is released, we'll remove all the code associated with these
pre-release flavors.
