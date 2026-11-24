# Security Policy and Reporting a Vulnerability

stellar-core falls under the Stellar Development Foundation's (SDF) bug bounty
program, hosted on HackerOne (**https://hackerone.com/stellar**), so reports 
should be submitted through HackerOne.

This SECURITY.md page is the authoritative source so if anything here differs from HackerOne, this page applies.

## Reporting a vulnerability

- Submit your report at https://hackerone.com/stellar/reports/new.
- Include as much detail as possible, including a description of the issue,
  its potential impact, and clear reproduction steps or a Proof of Concept.
- Reports with extremely long description and/or incoherently describing the vulnerability might get rejected.
- Do **not** open public GitHub issues or pull requests for security
  vulnerabilities.


### In scope

- Only code that has been released (present in the
[release tags](https://github.com/stellar/stellar-core/releases)).
- Vulnerability with current mainnet network settings. See running
[Quickstart with mainnet settings](https://github.com/stellar/quickstart/blob/main/local/core/etc/config-settings/README)

### Out of scope

- Vulnerabilities in previous protocol versions.
- Code residing on development or feature branches, and any code tagged or documented as unreleased, experimental, or behind a
feature flag not yet enabled on the Stellar public network.
- Any security issue arising from collusion of a node blocking set.
- Any exploit that requires a quorum of validators.
- For MEV attacks, probabilistic/brute-force transaction ordering approaches
  (i.e. the attacker sends many semantically identical transactions in order
  for one to execute first).
- Any feature behind a `BUILD_TESTS` flag.
- All test files.
- Offline commands.
- Issues that depend on malicious or incorrect data from external data
  providers used by tools such as Lab, CLI, or similar interfaces.
- stellar-core's built-in HTTP command and query endpoints are intended for
  private operator/local infrastructure use only. They are not public network
  APIs and are not meant to be exposed to untrusted/public internet traffic.
  Reports requiring direct public exposure of these endpoints are out of scope
  unless they demonstrate impact under the supported private/local deployment
  model.
- DoS vectors requiring brute-force paid transaction flooding (economically
  expensive).
- Network settings causing out-of-bounds issues, since settings are manually
  proposed and evaluated by validators.
- Archived GitHub repositories or projects, and GitHub repository forks.
- Bugs already known to SDF or already reported by another researcher
  (including known issues the project has consciously decided not to fix).
  Check the relevant GitHub repositories' issues marked with a security label
  before reporting.


## Severity classification

Severity is determined using the
[OWASP risk rating methodology](https://owasp.org/www-community/OWASP_Risk_Rating_Methodology)
rather than CVSS:

1. Determine the severity by finding the *Impact Category* that best matches
   the vulnerability in the severity lookup matrix below.
2. Evaluate how easy it is to exploit the vulnerability. Typically, if the
   exploit needs a malicious tier-1\* to pull off the attack, then severity
   goes down at least one level (for example, High to Medium).

\* Tier-1 here refers to the transitive quorum, given the state of the network
today. If in the future the quorum changes, this language would need to be
re-evaluated.

### Severity lookup matrix

| Impact Category | Trivial to exploit | Requires tier-1 access |
|-----------------|--------------------|------------------------|
| Direct theft / loss of funds | Critical | Critical |
| Total network halt | Critical | High - Medium (mitigation dependent) |
| Affects XLM supply integrity (minting, burning) | Critical | High |
| Fund freeze | Critical | High |
| Observable non-determinism causing divergence | Critical | High |
| Fee pool exploits | High | Medium |
| Soroban runtime bug putting realistic smart contracts at risk of plausible malfunction | High | - |
| No combination of the Tier-1 validator organizations in a new node's stellar-core quorum set config allows them to join the network | High | Medium |
| Increase resource consumption that can cause slowdown of 5 seconds or more | Medium | Low |
| Increase memory consumption more than 20% of the recommended hardware specification | Medium | Low |
| Soroban simulation library causing Stellar RPC crash | Medium | - |
| MEV / transaction ordering manipulation | Medium | - |
| Bypass Soroban rent fee mechanism paying less than 20% of actual | Medium | Low |
| Shut down of greater than 30% of watcher nodes (network unaffected) | Medium | Low |
| Bypass Soroban rent fee mechanism paying between 21% to 99% | Low | Informational |
| DoS by less than or equal to 30% consumption of CPU/memory resources | Low | Informational |
| Increase resource consumption that can cause slowdown of more than 1 second but less than 5 seconds | Low | Informational |
| Increase memory consumption more than 5% but less than 20% of the recommended hardware specification | Low | Informational |
| Shut down of 10% to 30% of watcher nodes (network unaffected) | Low | Informational |
| Minor metering discrepancy that can cause slowdown between 2 and 5 seconds | Low | Informational |
| Trivial metering discrepancy that can cause slowdown less than 2 seconds | Informational | Informational |

### Severity, impact and examples

The examples below should be evaluated in the context of the hardware
specifications described at
https://developers.stellar.org/docs/validators/admin-guide/prerequisites#hardware-requirements.

| Severity | Impact Description | Concrete Example |
|----------|--------------------|------------------|
| Critical | Direct theft or irreversible loss of funds excluding fees burned | \*Payout would be a % of loss of funds |
| Critical | Total network halt — network cannot confirm new transactions | Example 1: Auth stack exhaustion causing all validator nodes to crash. Example 2: XDR recursion depth limits causing stack overflow. Example 3: Oversized duplicate SCP message crashing a node via flow control invariant abort. \*Mitigation mechanism determines the severity. If it is easy to mitigate by removing a Tier-1 from another's config then it is medium severity. If a new software package is needed then it is high severity. |
| Critical | Minting of native asset (XLM) outside of protocol rules | Integer overflow in fee validation enabling creation of XLM from nothing |
| Critical | Permanent freezing of funds | Bug in Soroban contract storage that makes persistent entries permanently inaccessible |
| Critical | Observable non-determinism causing divergence | Floating-point or platform-dependent behavior in Soroban host functions producing different ledger hashes across validator architectures (e.g., ARM vs x86) |
| High | Fee pool exploits | A user submits a transaction that drains the Fee Pool thus stealing XLM |
| High | Soroban runtime bug putting realistic smart contracts at risk of plausible malfunction | Examples: unauthorized call gets authorized, SCVal stored in storage gets corrupted, error code gets lost, cryptographic function "validates" invalid input, wrong contract gets invoked, wrong event emitted. |
| High | New nodes unable to join the network due to corrupt history archive of all Tier-1 validator organizations | A bug that corrupts the history archive of every Tier-1 validator organization thus preventing new nodes from joining the network. No combination of Tier-1 validator organizations in the `[[VALIDATORS]]` array in the new node's stellar-core config allows them to join the network. However, if this exploit requires a malicious Tier-1 validator organization to corrupt every other Tier-1 validator's history archive then the severity would drop to a Low. |
| Medium | A malicious Tier-1 validator able to halt the entire network | A tier-1 validator crafting malformed SCP messages that cause all other tier-1 nodes to crash |
| Medium | Metering discrepancy in Soroban that can lead to resource exhaustion | Soroban host functions undercharging CPU or memory resources by 50 times such that an attacker can produce measurable slowdown |
| Medium | Increase resource consumption that can cause slowdown of 5 seconds or more | Specially crafted Soroban contract invocation that triggers excessive disk I/O during ledger close, slowing all validators |
| Medium | Increase memory consumption more than 20% of the recommended hardware specification | |
| Medium | Soroban simulation library bug causing Stellar RPC to crash | Bug in Soroban simulation library causing Stellar RPC nodes to crash when running simulation |
| Medium | MEV attack or any transaction ordering manipulation | Bug where attacker can deterministically cause a transaction to execute before a victim transaction |
| Low | Shut down of greater than 30% of watcher nodes – network is unaffected and continues to close ledgers | Peer-to-peer flood message causing non-tier-1 watchers to disconnect |
| Low | DoS by causing less than 30% of CPU/memory resource consumption on nodes outside of the declared limits and without brute force | Specially crafted Soroban contract invocation that triggers moderate disk I/O during ledger close, slowing all validators |
| Low | Bypass of transaction fee mechanism enabling free or severely underpriced transactions | TxSet baseFee integer overflow bypassing fee validation entirely |
| Low | Exploit Soroban rent fee mechanism | Any exploit that can get access to free rent for persistent Soroban entries. |
| Informational | Minor metering discrepancy causing undercounting of Soroban transactions | Undercharging in Soroban host function that cannot be exploited to DoS the network or cause any significant slowdown. |
| Informational | DoS vector requiring brute-force (paid transaction flooding) | Submitting thousands of max-fee transactions to slow ledger close — economically expensive and out of scope |
| Informational | Network settings causing out of bounds issues | Settings are manually proposed and thoroughly evaluated by validators so there is no risk of going out of bounds |
| Informational | New nodes unable to join the network | Corrupt history causing a new node to not be able to join |

## Submission requirements

A Proof of Concept (PoC) demonstrating the bug's impact is required for all
severities. All reports, regardless of severity, must include:

- A description of the vulnerability and the affected asset.
- Step-by-step reproduction (Proof of Concept), including actual requests,
  responses, or an exploit script.
- Evidence that the issue is reproducible with a minimal working script.

Reports submitted without a sufficient PoC will not be eligible for payout.

### Best practices

- Use a local instance and a separate network, not testnet or pubnet, when
  researching security bugs. Blockchains are public, and someone may observe
  your findings and report a bug before you do.
- For issues that depend on a specific runtime or environment, a containerized
  Proof of Concept (such as a Dockerfile) is strongly encouraged when it
  materially improves reproducibility.
