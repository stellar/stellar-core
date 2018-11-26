---
title: Security and Protocol release notes
---

# Purpose of this document

This document describes changes to the Stellar protocol as well as other changes made to address security issues.

It is organized as a reverse chronological timeline of releases.

## Protocol updates

* Changes to SCP.
* Changes to the Stellar Protocol (anything that modifies how the distributed ledger functions, including historical data).

## Security issues

* DDoS.
* Crashes (that could lead to remote code execution).
* Other attacks that can be exploited (inside or outside of the Stellar protocol).

## Goals for this document

* have a summary view of changes that affect the code base (stellar-core has to be able to replay all ledgers generated since genesis on the Stellar public network).
* drive transparency on issues that affected the network in the past as well as their impact.

## Security issues disclosure policy

(does not apply to protocol changes that are not security related)

The goal is work with the larger security community on a responsible disclosure model.

It then follows that:
* this document is not where security disclosures are made, instead follow the process outlined in [Stellar's bug bounty program](https://www.stellar.org/bug-bounty-program/) as a way to triage and respond to issues.
* issues are reflected in this document 30 days after release of the version of Stellar core containing fixes for the issues.

# Format of each report

* `tag-name` - security - description of the problem and impact
    * exploited - yes/no/unknown
        * if yes: description of the attacks that took place (with timeline).
    * mitigation: code fix, etc

* `tag-name` - protocol - description of the protocol change

## Tags used in this document

* `Overlay` - subsystem used by peers to communicate to each other
* `Herder` - subsystem coordinating all other subsystems
* `SCP` - subsystem implementing SCP
* `Ledger` - Ledger management including transaction subsystem
* `History` - History subsystem

# List of releases

## v10.0.0 (2018-09-05)

* `Ledger` - protocol - new `bumpSeqOp`, implementing CAP0001

* `Ledger` - protocol - updated signature verification, to be done at transaction
apply step. implements CAP0002

* `Ledger` - protocol - add liabilities to offers, implements CAP0003

* `Ledger` - security - rounding error could allow dust trades to make large error.
    * exploited: yes
        * over the course of a few weeks preceding the upgrade to 10, some
         bots performed dust trades.
        * Impact is determined by the ratio between assets. For example with
        P=1/20,000 a rounding error of 10E-7 (1 in absolute term), is equivalent
        to a 20,000:1 error (effective P is 1.0 instead of 1/20,000)
    * mitigation: CAP0004 implemented in protocol 10

* `Ledger` - protocol - fast fail attempts to `changeTrustOp` on native.

* `Ledger` - protocol - fast fail `setOptionsOp` when attempting to set weight
of a signer to more than 255.

## v9.2.0 (2018-03-20)

* `Herder` - protocol - properly compute next ledger start time (could lead to rounds starting too early).

* `SCP` - protocol - make timing out of the ballot counters less aggressive (reduce overall SCP time to close a ledger).

## v9.1.0 (2018-01-18)

* `Overlay` - security - stack overflow when processing bad xdr (DDoS)
    * exploited: no
    * mitigation: code fix

* `History` - protocol - some snapshot files could be corrupt when generated (rendering them unusable)

## v9.0.1 (2017-12-20)

* `SCP` - protocol - allow values to be validated differently during nomination and ballot protocol (used to be potentially more strict for the values generated during nomination)

* `Herder` - protocol - change the way upgrades are managed to be "one time triggers" instead of being on all the time


## v9.0.0 (2017-12-08)

* `Overlay` - peer could perform multiple handshakes in parallel denying other nodes from connecting (DDoS)
    * exploited: no
    * mitigation: code fix

* `Overlay` - all peers got dropped when node was getting overloaded by a single peer (DDoS)
    * exploited: no
    * mitigation: code fix

* `Ledger` - security - overflow in base reserve computation would allow certain operation to reduce the balance below reserve.
    * exploited: unknown
        * while it was possible to take the balance below reserve, this would simply make accounts unusable until more Lumens were sent to the account.
    * mitigation: code fix

 * `Ledger` - protocol - `manageOffer` now computes the amount of Lumens that can be sold as if the offer was created

* `Ledger` - protocol - make `BASE_RESERVE` configurable

* `Ledger` - protocol - update fee processing check to not double count fee (allows to spend the last `minfee` amount from an account)

* `Ledger` - protocol - updated protocol version to 9

## v0.6.2 (2017-04-30)

* `Ledger` - security - invalid use of cached data could lead to lumen creation (double spend) or destruction
    * exploited: yes
        * rogue transactions caused new Lumens to be created, not accounted for in total coins
    * mitigation:
        * code fix
        * in order to restore the ledger to its expected number of coins, the foundation burned Lumens using one of the bugs fixed in this release (`pathPaymentOp`), practically speaking this ended up being equivalent to a forced distribution of Lumens by the foundation.
        * invariant for total coins implemented

* `Ledger` - protocol - updated protocol version to 8 (2017-04-26)

* `Ledger` - protocol - inflation fix: properly update `totalCoins` that are re-injected in `feePool` (due to rounding or deleted winners), was causing `totalCoins` to not match the actual sum of all coins in existence

* `Ledger` - protocol - don't use cached data when sending to self using `pathPaymentOp`

* `Ledger` - protocol - never cache account data between operations

## v0.6.1d (not widely released - 2017-04-26)

* `Ledger` - protocol - updated protocol version to 7

* `Ledger` - protocol - temporary disable signature verification in preparation for version 8 that contains fixes for caching problems

## v0.6.1c (not widely released - 2017-04-08)

* `Ledger` - security - merge account could be called on an account already merged in the same ledger, causing the Lumens balance of the doubly merged account to be credited multiple times into the destination account
    * exploited: yes
        * rogue transactions caused new Lumens to be created, not accounted for in total coins
    * mitigation:
        * another minimal code fix scoped to merge account was implemented to stop the updated pattern of transactions exploiting the bug while working on complete fix
        * additional monitoring of network activity

* `Ledger` - protocol - updated protocol version to 6

* `Ledger` - protocol - reload balance of source account when merging accounts (bad fix)

## v0.6.1b (not widely released - 2017-04-06)

* `Ledger` - security - merge account could be called on an account already merged in the same ledger, causing the Lumens balance of the doubly merged account to be credited multiple times into the destination account
    * exploited: yes
        * rogue transactions caused new Lumens to be created, not accounted for in total coins
    * mitigation:
        * minimal code fix implemented to stop known pattern of transactions exploiting the bug
        * additional monitoring of network activity

* `Ledger` - protocol - updated protocol version to 5

* `Ledger` - protocol - don't allow merging accounts from non-existant accounts (bad fix)

## v0.6.1 (2017-03-07)

* `Ledger` - protocol - updated protocol version to 4

* `Ledger` - protocol - ensure that `ManageData` cannot be used on unsupported on protocol version smaller than (and including) 3

## v0.6.0 (2017-02-07)

* `Ledger` - protocol - updated protocol version to 3

* `Ledger` - protocol - perform additional checks when sending to self (make failures consistent with non self payments of non-native assets)

* `Ledger` - protocol - updated order book's rounding (avoid double round down when dealing with dust trades)

* `Ledger` - protocol - added `hash(tx)` and `hash(X)` as signing methods

* `Ledger` - protocol - do not allow to call `AllowTrustOp` and `ChangeTrustOp` on self

* `Ledger` - protocol - do not allow to create an offer with an amount of 0 (would fail as if it deleted an offer)

* `Ledger` - protocol - properly set `lastModifiedLedgerSeq` for `DataEntry`

* `SCP` - protocol - limit number of validators in a quorum set to a smaller value

* `Overlay` - security - node would cache data that it didn't request, potentially purging data that it would need from cache (DDoS)
    * exploited: no
    * mitigation: code fix

## v0.5.0 (2016-04-11)

* `Ledger` - protocol - added support for `DataEntry` and `ManageDataOp`

## v0.4.0 (2015-12-21)

* `Herder` - security - arbitrary validators on the network could send messages, causing validators to use up all their memory (DDoS)
    * exploited: no
    * mitigation: code fix

## v0.3.2 (2015-11-23)

* `SCP` - security - bad sequence of messages could lead to node crashing (DDoS)
    * exploited: no
    * mitigation: code fix

## v0.3.1 (2015-11-18)

* `SCP` - protocol - adjustments based on the whitepaper updates as of November 17 2015

## v0.3.0 (2015-11-16)

* `Overlay` - security - busy loop when peers were claiming to have data they don't have (DDoS)
    * exploited: no
    * mitigation: code fix

* `SCP` - protocol - adjustments based on the whitepaper updates as of November 2015


## v0.2.5 (2015-11-06)
* `Overlay` - security - bad peers would be retried, bypassing the PREFERRED_PEERS setting (DDoS)
    * exploited: no
    * mitigation: code fix

## v0.2 (2015-10-12)

* `Overlay` - security - partial messages not handled properly could lead to busy peers (DDoS)
    * exploited: no
    * mitigation: code fix

* `Herder` - security - creation of unknown slots would allow malicious peers to allocate memory (DDoS)
    * exploited: no
    * mitigation: code fix

* `SCP` - protocol - additional validation of quorum sets
