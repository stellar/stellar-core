# Versioning stellar-core and soroban

Versioning stellar-core and soroban involves codifying and managing the
relationships between 3 sets of independently evolving code:

  1. Versions of stellar-core
  2. Versions of the soroban host
  3. Versions of individual contracts

The goal of codifying and managing relationships among versions is to ensure two
types of compatibility hold:

  1. Forward compatibility: old code handling new contexts.
  2. Backward compatibility: new code handling old contexts.

Each such type of compatibility can be further refined as either weak or strong.
Weak compatibility means that the code in question "handles" the context in
question by merely recognizing it and failing clearly, in an informative
fashion. Strong compatibility means that the code "handles" the context by
continuing to function correctly.

Version relationships will be managed by 4 separate mechanisms:

  1. The "protocol" version number.
  2. The "environment interface" version number.
  3. The "host logic" version number.
  4. The contract spec subtyping relationship.

## Stellar-core

Stellar-core interacts with "different versions of itself" in two dimensions:

  1. Over space: multiple replicas of stellar-core coordinate in consensus to
     process transaction sets, with identical results at each replica.

  2. Over time: old versions of stellar-core record transactions into a
     historical record (stored in archives) that new versions must be able to
     replay precisely.

### The protocol version number

The relationship between these different versions of stellar-core is managed
primarily by the protocol version number. This is worth understanding in detail:

  - Each ledger header has a protocol version number, meaning:

    - All peers in consensus agree on the protocol version number.

    - The protocol version number only changes when there is consensus to
       perform an upgrade (using a field in consensus messages).

    - During history replay, stellar-core knows the protocol version number that
       was in effect when each ledger was closed.

### Restrictions on XDR evolution

  - Every stellar-core process communicates with other stellar-core processes
    through XDR, and while XDR does not have built-in support for evolution of
    its schema, we make two strong restrictions on how we evolve XDR, to achieve
    compatibility:

    - We only ever evolve our XDR in a way that supports _fairly_ strong (at
      least IO-level) backward compatibility, by only adding cases to
      enumerations and unions (including extension points we put in for this
      purpose), such that the XDR-reading code in any new stellar-core version
      can always parse output of the XDR-writing code in an old stellar-core
      version.

    - We only store XDR in a context that carries a protocol version number,
      which supports weak forward-compatibility: old versions of stellar-core
      recognize when they're interacting with new versions and fail safe,
      refusing to communicate, typically disconnecting or rejecting their input.

In the stellar classic protocol, the protocol version number and the
restrictions on how we use XDR have been sufficient to manage the relationships
among versions of stellar-core, mainly because of two special characteristics of
the above arrangement:

  - Every stellar-core knowing (from the ledger header) which protocol version
    number to replay old transactions under means that if we fix a bug in
    stellar-core we can keep an old (buggy) version of a _specific code path_
    around in new (fixed) versions and replay old-protocol transactions along
    the old code path, thereby strengthening the IO-level strong backward
    compatibility of the system to strong _semantic_ backward compatibility. We
    replay old transactions under the exact logic they were recorded under,
    getting bit-for-bit replay.

  - Every stellar-core knowing (from the ledger header) when it's receiving
    ledgers from a network that has voted to upgrade past its supported protocol
    version means that it can fail safely in a relatively benign failure mode,
    that we require operators to be willing to adapt to by downloading and
    deploying a new version of stellar-core. In other words: the weak form of
    forward-compatibility among stellar-core versions is acceptable, we do not
    _have_ to support old versions of stellar-core speaking to new versions,
    over time, just detect and emit an error telling users to upgrade.

As we will see, neither of these characteristics hold in the world of soroban,
so we will need some new mechsnisms.

## The soroban host

The soroban host necessarily participates in _two_ versioning relationships:

  - Like stellar-core (since it's now a _component_ of stellar-core), the
    soroban host "interacts with different versions of itself" in two
    dimensions: over space (consensus) and over time (replay).

  - The soroban host also interacts with different versions of _individual
    contracts_, which are uploaded to the ledger and remain there over time. A
    new soroban host can process a new transaction in a ledger with a new ledger
    protocol version, but that transaction may still invoke an old contract.

In addition to this first complication (the host-contract version relationship)
there is also a potentially worse complication:

  - The soroban host is a much larger piece of software than the classic
    stellar-core transaction subsystem, and much of it is written by third
    parties, so it is much harder for us (SDF) to maintain precise control over
    its semantics from one version to the next.

To handle for these two complications, we have two new mechanisms:

  1. The environment interface version number. This is a number similar to the
     stellar-core protocol number -- in that it will only be increased through
     consensus and will be recorded in the ledger -- but it can vary more often
     and can be used to capture additional nuances in the relationship between
     host and contract, that go beyond changes captured by the protocol version
     number.

  2. The host logic version number. This is a number that indicates the _exact_
     (bit-for-bit) implementation of the host source code (and its entire
     dependency tree) that we execute a given ledger against. This number is
     also stored in the ledger and can only be upgraded by consensus.

### The environment interface version number

The environment interface version number increases any time the environment
interface changes, which includes any protocol version number bump.  In theory
the environment interface version number could just be the protocol number, but
during development of soroban we wanted the freedom to expire experimental
versions of the environment interface, so it got its own number.

Every contract has a minimum environment interface version number written into
it.

Every host has both a minimum and maximum supported environment interface
version number. The minimum will freeze when we finalize the interface. The
maximum will continue to advance as the interface is evolved.

### Host compatibility with contracts

If a contract has a higher minimum environment interface version number than a
host's maximum, it means the contract is too new for the host, and there is no
way for the host to run the contract. This case is rejected with an erro: the
old host is only weakly forward-compatible with new contracts.

If a contract has minimum environment interface version number within a host's
supported range, then the host can run the contract; the only question is how to
do so compatibly.

From the perspective of the host, this is a backward-compatibility question: the
new host must parse and run data (a contract) from an old context. From the
perspective of the contract -- which is also code! -- this is a
forward-compatibility question: the old contract must adapt to running in a new
context.

The soroban host can only make promises from the host's perspective: the
backward-compatibility promises. Specifically it can and will promise:

  - The environment interface will only evolve in ways that contain, as a
    backward-compatible subset, all the semantic entities from all previous
    versions. In other words the environment interface can grow new host object
    types and host functions over time, but never remove any.

  - While XDR IO in contracts is rare, the host can at least promise that the
    XDR spoken by a new host will be an evolution of the old XDR spoken by the
    contract, subject the the standard restrictions on XDR evolution: strong
    backward compatibility and weak forward compatibility, via enum and union
    extensions and an observable protocol version number. This ensures that the
    new host can read old XDR written out by the old contract, and the old
    contract can at least detect (and either fail or attempt partial success)
    when it is given XDR from a version newer than it was compiled with.

What the host cannot promise, but must rely on a contract to promise, are the
symmetric cases:

  - Old contract code should be (as much as the contract author desires and as
    much as is semantically reasonable) forward-compatible with new environment
    interface versions and even new protocol versions. Specifically contracts
    should as much as possible tolerate additions to the environment interface
    in the ways allowed: new host object types, new host functions, and even new
    XDR enum cases or unions (if it wishes to try). The exact degree of
    forward-compatibility a contract supports is beyond SDF's control, as a
    contract can choose to fail for any reason it likes. If a contract wishes it
    can assert "unreasonable" compatibility conditions, such as failing if the
    contract observes an unknown interface version number, enum case, host
    object type, or even failing if it observes too high a ledger number. In
    terms of guarantees, we can only "meet contracts half way" by providing a
    backward compatible host and asking them to write "as forward-compatibly as
    they can" (or want to be).

### The host logic version number

A host logic version number designates an _exact_ version of the
soroban-env-host crate, including the exact versions of all of its
transitive dependencies.

The ledger stores a `CONFIG_SETING_CONTRACT_HOST_LOGIC_VERSION` entry
that designates the exact logic version that each ledger was initially
recorded under. Because this is part of the ledger, it is a consensus
value: every peer in consensus has the _exact same_ version of the
soroban host. This is different from stellar network protocol
versions, where it's possible to have peers in consensus but on
slightly different minor versions of stellar-core (eg. 19.2 and 19.3
might be in consensus). The host logic versions are more exact.

Host logic version numbers are assigned _by the embedder_ (that is:
stellar-core) rather than soroban-env-host. There are two reasons
for this:

  - Every increase to a host logic version number is a "heavy"
    operation that requires a network-consensus-based "upgrade" to the
    config setting. The host might want to make multiple small releases of
    before the embedder commits to upgrading the network from host
    logic version N to N+1, so consecutive host logic version numbers don't
    necessarily reflect consecutive host releases.

  - Cargo happens to resolve dependency version specifications at the
    "final" site of compilation, which is the embedder. In other
    words, the soroban-env-host crate literally _can't tell_ which
    precise versions of some of its transitive dependencies will be
    resolved to in a given embedding. Even if it specifies exact
    versions of its direct dependencies, indirect dependencies will
    have looser version specifications that resolve differently at
    different times. Only the top-level Cargo.lock file in
    stellar-core specifies an exact configuration.

There are two unusual consequences of this arrangement:

  - In order to do an "upgrade" on the network at all -- that atomically
    switches from host logic version N to N+1 at a single ledger boundary -- we
    have to actually keep _two versions_ of the soroban-env-host crate (and all
    of its dependencies) compiled into stellar-core at all times. Rust allows
    this, but it's a bit tricky and we have added some safety mechanisms to make
    it harder to make mistakes.

  - Any backward-compatibility code in soroban-env-host that we add to emulate
    past versions during replay has to be gated on some value _other_ than a
    host logic version number, because soroban-env-host doesn't know what a host
    logic version number even is. As a consequence we gate backward
    compatibility code in soroban-env-host using an enum called `LegacyEpoch`.

We'll discuss these in some detail below, as well as give a worked
example of upgrading.

## Lo and hi versions

As mentioned above, at any given time stellar-core has two versions
of soroban-env-host compiled in. These are symbolically called `lo`
and `hi`, and the host logic version of `hi` is always defined to be
one greater than the host logic version of `lo`.

### Invocation

When invoking a contract host function (which is technically what happens when
we "run a smart-contract transaction"), stellar-core loads the intended host
logic version `N` from the ledger and decides which of `hi` and `lo` to build a
host and invoke a function on, considering 3 possible cases:

  - If `N > hi::VERSION`, then `N` is "from the future" and no invocation can
    happen on on this version of stellar-core. It needs to upgrade. The
    invocation fails. This is weak forward compatibility.

  - If `N == hi::VERSION` or `N = lo::VERSION`, then the invocation is made done
    on a `Host` from `hi` or `lo` as appropriate.

  - If `N < lo::VERSION`, then `N` is translated to some `LegacyEpoch` that
    soroban-env-host can understand, and the invocation is made on a `Host` from
    `hi`, configured to emulate that `LegacyEpoch`. This is strong backward
    compatibility.

It may be counterintuitive that in the third case the call is run on `hi` rather
than `lo`, but as we will see in the worked example, it's not possible to add
new backward-compatibility code to `lo` to emulate a given `LegacyEpoch` by the
time we need to add it, so to allow adding such as we discover the need for it
during upgrades, we always replay legacy ledgers on `hi`.

### Upgrading

Upgrading stellar-core to a new version of soroban-env-host requires care. Two
aspects of the process are important to understand:

  - The upgrade will eventually be "activated" in the network, meaning that the
    network will vote to upgrade from some host logic version `N` to `N+1`, and
    this transition will occur synchronously across all nodes in consensus. This
    in turn means that when the upgrade occurs, all nodes must have `lo=N` and
    `hi=N+1`. In other words it's only _possible_ for a network upgrade to occur
    as a transition from running on `lo` to running on `hi`.
  
  - Once the network upgrade has occurred, there is a _second phase_ in which
    the network continues to run on logic version `N+1`, but it shifts from `hi`
    to `lo` to _prepare_ for the next upgrade. In other words, a new version of
    stellar-core is prepared and deployed that can still run host logic version
    `N+1`, but runs it as `lo` rather than `hi`. Instead, `hi` is advanced to
    host logic version `N+2`, and host logic version `N` is _expired_ from the
    new version of stellar-core. In the process of expiring direct support for
    host logic version `N`, some backward compatibility code may be added to
    host logic version `N+2`, in the form of a new `LegacyEpoch` in
    soroban-env-host.

### LegacyEpochs

A `LegacyEpoch` is just an ordered symbolic value -- a Rust `enum` -- that
denotes some time period in the past when the logic of soroban-env-host was
different than it is by default now. The special value `LegacyEpoch::Current` is
always the highest `LegacyEpoch` and always denotes the default, current
behaviour.

We separate `LegacyEpoch` values from host logic version numbers because, as
mentioned before, host logic version numbers "don't mean anything" to the host,
they're assigned by the embedder (stellar-core), and it would be confusing to
have logic version numbers chosen by stellar-core appear inside the host.

Further, the host logic that stellar-core numbers as version `N` may or may not
be "exactly compatible" with the logic stellar-core numbers as `N+1`, so we may
or may not need to add `LegacyEpoch` values when assigning new logic version
numbers. Indeed the only real way we can _judge_ compatibility at all is by
taking the ledgers that were recorded on the network with logic version `N` and
replaying them on logic `N+1` and seeing if their outcomes differ in any
observable way.

If, when we do such a replay, we see a difference, we add a new `LegacyEpoch` to
soroban-env-host describing the _way `N` differed_ that _was actually observed_
during the replayed history segment, and then add an entry to stellar-core's
invocation path mapping its logic version `N` to the host's new `LegacyEpoch`.
At this point stellar-core can expire support for its logic version `N`, because
it's able to emulate `N` on future version (it can actually only emulate version
`N` on version `N+2`, due to the way upgrades are sequenced; see the example
below).

## Worked example

This is a general example that covers all the main points above.

Suppose the network has a consensus `CONFIG_SETING_CONTRACT_HOST_LOGIC_VERSION`
set to `75`. Suppose that there is a mix of versions of stellar-core in the
field, some with `lo=75` and some with `hi=75`. Suppose also that there is a bug
in `75` such that it does binary search slightly wrong (not so wrong as to never
work, but wrong enough that it's a bug). We will proceed through describing a
full upgrade cycle, including fixing the bug and preserving it under a
`LegacyEpoch` for backward compatibility.

  1. Since _some_ nodes exist with `lo=75`, they must have `hi=76`, which means
     that stellar-core development has already assigned the host logic version
     `76` to some version of soroban-env-host. This in turn means that before
     anything else can be done, we must upgrade _all_ nodes to that release,
     with `lo=75` and `hi=76`.

  2. Once all nodes are running with `lo=75` and `hi=76`, they must agree to
     arm an upgrade to set `CONFIG_SETING_CONTRACT_HOST_LOGIC_VERSION=76`.
     Eventually this upgrade occurs (nothing more can happen until it does).

  3. Now we proceed to phase 2 of the upgrade process. We're going to work on
     a new version of stelar-core defining logic version `77`, as well as expiring
     logic version `75`. This new version, in other words, will have `lo=76` and
     `hi=77`.

  4. To do this, we update `lo::VERSION` to `76` in stellar-core; `hi::VERSION`
     automatically becomes `77` since it is defined as `lo::VERSION+1`. We then
     adjust git references and release version numbers in stellar-core's
     `Cargo.toml`, setting `soroban-env-host-lo` to what `soroban-env-host-hi`
     previously pointed to, and setting `soroban-env-host-hi` to some new
     release of the host.

  5. Next we evaluate whether we can emulate the segment of history that was
     recorded under logic version `75` on later versions. We won't be able to
     add any backward-compatibility changes to `76` if we need to, since `76` is
     already deployed, but we can adjust the still-in-development logic version
     `77`, so we do a replay of the `75` ledgers on `77`. Since the network is
     on `76` there are no new `75` transactions being recorded, and we can
     safely do this replay offline on a development workstation.

  6. If the `75` ledgers replay without any divergence on `77`, the upgrade is
     clean and we need no `LedgerEpoch`, we can just drop `75`. But suppose the
     binary search bug in `75` was fixed in the host in `76` (or `77`), and
     suppose some transaction recorded under `75` exercised it, so diverges on
     replay under `77`. Before we can proceed we must modify the host further
     (and redefine `77` to include the modification). We add a new `LedgerEpoch`
     covering the _old_ behaviour, call it `LedgerEpoch::BadBinSearch`, and add
     a branch like `self.is_in_legacy_epoch(LegacyEpoch::BadBinSearch)` to the
     host that activates backward-compatibility code.

  7. Finally we have a version of the host that can emulate `75` for replay,
     i.e. a version we're comfortable calling `77`. We update the `Cargo.toml`
     in stellar-core to point its `soroban-env-host-hi` to the chosen host
     version (containing the new backward-compatibility code), then add a case
     to stellar-core's impl of `SetHostLogicVersion` for
     `hi::soroban_env_host::Host` that maps logic version `75` to
     `hi::LedgerEpoch::BadBinSearch`, which is present in `hi`.

## Contract spec subtyping

TBD.