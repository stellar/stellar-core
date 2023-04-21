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

Version relationships will be managed by 2 separate mechanisms:

  1. The "protocol" version number.
  2. The contract spec-compatibility relationship.

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
so we will need some new mechanisms.

## The soroban host

The soroban host necessarily participates in _two_ versioning relationships:

  - Like stellar-core (since it's now a _component_ of stellar-core), the
    soroban host "interacts with different versions of itself" in two
    dimensions: over space (consensus) and over time (replay).

  - The soroban host also interacts with different versions of _individual
    contracts_, which are uploaded to the ledger and remain there over time. A
    new soroban host can process a new transaction in a ledger with a new ledger
    protocol version, but that transaction may still invoke an old contract.

As with stellar-core itself, both of these relationships will mediated
through the protocol version, but there is a new complication:

  - The soroban host is a much larger piece of software than the classic
    stellar-core transaction subsystem, and much of it is written by third
    parties, so it is much harder for us (SDF) to maintain precise control over
    its semantics from one version to the next.

As a result of this complication, the network has a much higher risk of
experiencing a loss of consensus (thus divergence, a halt or split) due to minor
differences between the the _versions of the soroban host_ embedded each peer,
even if all peers are in consensus about the current declared protocol number.

To mitigate this risk, we introduce two new concepts -- canonical protocol
semantics and pinned soroban version trees -- as well as a new, more cautious
two-phase structure for performing upgrades to the soroban host.

### Canonical protocol semantics

Assume we can (socio-politically) identify a transitive quorum of peers as "the
canonical quorum" of a network, for the sake of versioning (eg. the "Tier 1"
peers), so we can speak of the network being "on" a given protocol version.

For any given protocol `N`, after the network has transitioned to protocol
`N+1`, we define "the canonical semantics of protocol `N`" as the sequence of
ledgers (transaction inputs, outputs, and exact ledger header, identified by
hash) that were recorded by the network while it was on protocol `N`.

Note that "the canonical semantics" of protocol `N` only has a fixed meaning
once the network has transitioned _off_ `N`. It is not useful or meaningful to
speak of the canonical semantics of protocol `N` while the network is still on
`N`, as the set grows moment-to-moment as new ledgers are closed.

### Pinned soroban version trees

Soroban is a Rust program and therefore follows Rust's particular approach to
versioning: when a release of (say) the soroban-env-host package is made, it
specifies its dependencies symbolically, with some wiggle room for "compatible
upgrades" (and even more wiggle room in transitive dependencies we don't
control). All of the wiggle room is "resolved" to _specific_ versions of each
transitive dependency by the package manager cargo only when the package is
embedded into a final application, like stellar-core.

It is quite easy to accidentally cause cargo to re-resolve dependencies, and
thus alter the version of a dependency (or transitive dependency) embedded in
stellar-core this way. And as we'll see below, we're (at times) going to wind up
with _two_ entire sets of soroban packages embedded in stellar-core, which makes
things even more confusing and fragile. So to make this process less error-prone
we snapshot and version (in git) and embed (in stellar-core) a secondary textual
manifest of the exact dependency tree (with hash identities) of each intended
soroban version, that is cross-checked against the _actual_ soroban versions
compiled-in to stellar-core, on startup.

### Two-phase upgrades

To define the upgrade process, we must assume as a precondition that the network
is already _running_ a set of non-diverging versions of stellar-core. The peers
of the network may have different minor versions, but we assume they are
sufficiently similar that they continue to close ledgers in strict
consensus. Our job is to define how to _maintain_ this assumption, inductively,
on each upgrade.

If we are upgrading parts of stellar-core _outside_ of soroban, we follow the
usual rules: protocol-level changes must be versioned via on-chain protocol
version upgrades, non-protocol changes can ship in minor versions. The soroban
versions embedded in stellar-core need not change; as we'll see below the new
protocol will just be considered an extension of the protocols to run on the
`curr` version of soroban and so will automatically run on it.

If we are upgrading soroban, we follow the same _general_ strategy we we do for
stellar-core:

  - Changes to soroban we believe (with very high confidence) are
    non-semantics-changing, we can ship as a "non-protocol" change, just
    modifying a pinned version of soroban embedded in stellar-core.

  - Changes to soroban we know to be introducing new functionality are "soroban
    protocol changes", as in normal stellar-core, and will be gated on the new
    protocol number.

However, there is also a new category of change with soroban: those where we are
not _sure_ whether the change in question will or won't change semantics. These
introduce complications:

  - Since they _might_ change semantics, we have to treat such changes as
    "soroban protocol changes" as well (and cut over to running the new code
    using a protocol-number gate) even though the change might do nothing. Only
    way to be safe.

  - Since we don't know _where_ the new code might change semantics, we can't
    put a protocol-number gate on specific lines of code; we put the gate on the
    _entire_ tree of dependencies containing the soroban host, and compile
    _multiple copies_ of the soroban host into stellar-core, simultaneously. At
    least during the upgrade.

  - Because we don't want to keep an ever-growing set of soroban versions
    compiled-in to the same stellar-core binary, we arrange to keep up to
    _two_ versions at any given time, and we do migrations in two phases,
    first _adding_ support for a new version and then later _expiring_
    support for the previous version.

More formally:

  - All versions of soroban can observe the current ledger protocol number; it
    is installed while setting up the soroban host. This can be used to add
    backward-compatibility code to soroban that is gated on specific protocol
    versions.

  - At any given time we have a version of `soroban-env-host` compiled into
    stellar-core under the alias `soroban-env-host-curr`.

  - We define a second _optional_ version of soroban which may be compiled in to
    stellar-core under the alias `soroban-env-host-next` (which also defines the
    Rust "feature" `soroban-env-host-next` on which `cfg()` conditions can be
    applied).

  - When `#[cfg(feature="soroban-env-host-next")]` a conditional branch is added
    to the Rust call path that invokes a host function to test the protocol
    number of the ledger being closed and, if it's equal to
    `Config::CURRENT_LEDGER_PROTOCOL_VERSION`, invoke the host function on
    `soroban-env-host-next`. Ledgers with earlier protocol versions invoke host
    functions on the `soroban-env-host-curr` copy, as do any ledgers when the
    feature is compiled-out.

  - Due to the way Rust's module system works, the `next` version can only be
    compiled-in if it represents a different package version number of soroban
    than the `curr` version.

  - When doing a "soroban protocol change", we follow a "two phase" process:

    - Phase 1 is "adding support for a new protocol and upgrading to it":

	- Assume the current network is running protocol `N` and the C++
	  constant `Config::CURRENT_LEDGER_PROTOCOL_VERSION` (which is the
	  _maximum_ protocol supported by stellar-core) is equal to `N`. If this
	  assumption doesn't hold it means you have already released a version
	  `V` of stellar-core with `Config::CURRENT_LEDGER_PROTOCOL_VERSION`
	  ahead of the network version `N`, and you need to upgrade the
	  network's version `N` before proceeding, because stellar-core version
	  `V` is not going to treat the upgrade to `N` as a "soroban protocol
	  change".

	- Increment (in C++) the `Config::CURRENT_LEDGER_PROTOCOL_VERSION`,
	  i.e. set it to `N+1` which is the version you are now _defining_
	  as a "soroban protocol change".

	- Let `R` be the current version of soroban.

	- Let `S` be the new version of soroban with the change you want.

	- Set the `soroban-env-host-next` package specification in `Cargo.toml`
	  to refer to version `S`.

	- Compile the Rust code with `--feature=soroban-env-host-next`, which
	  will compile-in the second copy of the `soroban-env-host`, but
	  continue to run the network's current version `N` and earlier
	  protocols `P < Config::CURRENT_LEDGER_PROTOCOL_VERSION` on the
	  existing `soroban-env-host-curr` version.

	- Release a version of stellar-core built this way. This will be the
          first version of stellar-core in the field with any support for
          protocol `N+1`, and it will be built with two versions of soroban, and
          configured to switch from `soroban-env-host-curr` to
          `soroban-end-host-next` exactly when it transitions from protocol `N`
          to `N+1`.

	- Run the protocol upgrade by consensus.

    - Phase 2 is "expiring support for an old protocol":

        - Set the `soroban-env-host-curr` package specification in `Cargo.toml`
          to the now-active version `S` of soroban. Doing this expires the old
          version `R` that was originally used to record protocol version `N`,
          leaving only the new version `S` (which may or may not replay `N`).

        - Recompile _without_ `--feature=soroban-env-host-next` (at this point
          `curr` and `next` are both `S` anyways).

        - Now that protocol `N` is inactive on the network, the canonical
          semantics for `N` are well defined: whatever was recorded. All that
          remains is to ensure `N` (and earlier) can be replayed faithfully on
          `S`. Replay all ledgers that were recorded under protocol `N`, and
          then further all ledgers before `N`, on this build of stellar-core. If
          it works, you're done. If it does not work, you've discovered what you
          might or might not have been expecting: semantic differences between
          `R` and `S`. Add backward-compatibility code to a point-release update
          `S.x` of soroban immediately after `S` (making sure you do not pull in
          any _other_ semantics-altering changes) and update the `Cargo.toml`
          spec to have `soroban-env-host-curr` as `S.x`.

        - Release this new version of stellar-core. Versions in the field are
          now a mixture of some with both `R` and `S`, and some with just `S.x`,
          but (in theory) both can replay all the same history, and behave the
          same way on all new ledgers.


### Host compatibility with contracts

If a contract has a higher minimum environment interface version number than a
host's maximum, it means the contract is too new for the host, and there is no
way for the host to run the contract. This case is rejected with an error: the
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

## Contract spec-compatibility relation

TBD.