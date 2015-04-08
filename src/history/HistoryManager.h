#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "generated/StellarXDR.h"
#include "history/HistoryArchive.h"
#include <functional>
#include <memory>

/**
 * The history module is responsible for storing and retrieving "historical
 * records" in longer-term (public, internet) archives. These records take two
 * forms:
 *
 *   1. Buckets from the BucketList -- checkpoints of "full ledger state".
 *   2. History blocks -- a sequential log of ledger headers and transactions,
 *      stored as separate ledger and transaction files.
 *
 * The module design attempts to satisfy several key constraints:
 *
 *   - Simplicity. Historical records must be simple enough that multiple
 *     implementations can digest them. The formats and protocols employed
 *     should be simple enough that they will have longevity and resistance
 *     to implementation idiosyncrasies and bugs. The interface to the
 *     history system should be simple enough that multiple backends can
 *     implement it.
 *
 *   - Public transparency. History should be stored in ways that permit
 *     3rd parties to access/mirror them completely, efficiently and easily,
 *     preferably without asking permission or consuming server resources.
 *
 *   - Resource efficiency. History should not consume unreasonable amounts of
 *     space. It should be practical to store truncated partial histories. It
 *     should employ large-chunk-size bulk transport over commodity bulk file
 *     service protocols whenever possible. It should be write-once and permit
 *     old / cold files to be moved to increasingly cheap (eg. glacier) storage
 *     or deleted using server-side expiration/archival policies.
 *
 *   - Tolerable latency for two main scenarios: "new peer joins network"
 *     and "peer temporarily de-synchronized, needs to catch up".
 *
 * The history store is served to clients over HTTP. In the root of the history
 * store is a history archive state file (class HistoryArchiveState) which
 * stores the most recent checkpoint (including version info, most recent ledger
 * number and most recent bucket hashes). As per RFC 5785, this checkpoint is
 * stored at .well-known/stellar-history.json as a JSON file.
 *
 * Checkpoints are made every 64 ledgers, which (at 5s ledger close time) is
 * 320s or about 5m20s. There will be 11 checkpoints per hour, 270 per day, and
 * 98,550 per year. Counting checkpoints within a 32bit value gives 43,581 years
 * of service for the system.
 *
 * While the _most recent_ checkpoint is in .well-known/stellar-history.json,
 * each checkpoint is also stored permanently at a path whose name includes the
 * checkpoint number (as a 32-bit hex string) and stored in a 3-level deep
 * directory tree of hex digit prefixes. For example, checkpoint number
 * 0x12345678 will be described by file history/12/34/56/history-0x12345678.json
 * and the associated history block will be written to the two files
 * ledger/12/34/56/ledger-0x12345678.xdr.gz and
 * transaction/12/34/56/transaction-0x12345678.xdr.gz
 *
 * Bucket files accompanying each checkpoint are stored by hash name, again
 * separated by 3-level-deep hex prefixing, though as the hash is unpredictable,
 * the directories will fill up in unpredictable order, not sequentially: if the
 * bucket's hex hash is <AABBCCDEFG...> then the bucket is stored as
 * bucket/AA/BB/CC/bucket-<AABBCCDEFG...>.xdr.gz
 *
 * The first ledger and transaction files (containing the genesis ledger) can
 * therefore always be found in ledger/00/00/00/ledger-0x00000000.xdr.gz and
 * transaction/00/00/00/transaction-0x00000000.xdr.gz and described by
 * history/00/00/00/history-0x00000000.json. The buckets will all be empty in
 * that state.
 *
 *
 * Boundary conditions and counts:
 * -------------------------------
 *
 * There is no ledger 0 -- that's the sequence number of a _fictional_ ledger
 * with no content, before "ledger 1, the genesis ledger" -- so the initial
 * ledger block (block 0x00000000) has 63 "real" ledger objects in it, not 64 as
 * in all subsequent blocks. We could, instead, shift all arithmetic in the
 * system to "count from 1" and have ledger blocks run from [1,64] and [65,128]
 * and so forth; but the disadvantages of propagating counts-from-1 arithmetic
 * all through the system seem worse than the disadvantage of having to
 * special-case the first history block, so we stick with "counting from 0" for
 * now. So all ledger blocks _start on_ a multiple of 64 and run until
 * one-less-than the next multiple of 64, inclusive: [0,63], [64,127],
 * [128,191], etc.
 *
 *
 * The catchup algorithm:
 * ----------------------
 *
 * When catching up, it's useful to denote the ledgers involved symbolically.
 * Consider the following timeline:
 *
 *
 *       [==========|.......|========|=======|......]
 *    GENESIS     LAST    RESUME   INIT    NEXT    TIP
 *                                   |              |
 *                                   [-- buffered --]
 *                                       (in mem)
 *
 * The network's view of time begins at ledger GENESIS and, sometime thereafter,
 * we assume this peer lost synchronization with its neighbour peers at ledger
 * LAST. Catchup is then concerned with the points that happen after then:
 * RESUME, INIT, NEXT and TIP. The following explains the logic involving these
 * 4 points:
 *
 * The desynchronized peer commences catchup at some ledger INIT, when it
 * realizes INIT > LAST+1 and that it is "out of sync". This begins "catchup
 * mode", but we expect that during catchup TIP -- the consensus view of the
 * other peers -- will continue advancing beyond INIT. The peer therefore
 * buffers new ledger-close events during catchup, as TIP advances. This set of
 * ledgers -- the segment [INIT, TIP] -- is stored in memory, in the
 * LedgerManager (::mSyncingLedgers) and extended as SCP hears of new closes,
 * until catchup is complete.
 *
 * The catchup system then rounds up from INIT to NEXT, which is the next
 * checkpoint after INIT that it can find on a history archive. It will pause
 * until it can find one; this accounts for a delay up to the duration of a
 * checkpoint (~5 minutes).
 *
 * Depending on how it's invoked, the catchup system will then usually define
 * RESUME as either equal to NEXT or LAST, or in unusual cases some ledger
 * between the two. RESUME is the ledger at which the ledger state is
 * reconstituted "directly" from the bucket list, and from which hisory blocks
 * are replayed thereafter. It is therefore, practically, a kind of "new
 * beginning of history". At least the history that will be contiguously seen
 * on-hand on this peer.
 *
 * Therefore: if the peer is serving public API consumers that expect a
 * complete, uninterrupted view of history from LAST to the present, it will
 * define RESUME=LAST and replay all history blocks since LAST. If on the other
 * hand the peer is interested in a minimal catchup and does not want to serve
 * contiguous history records to its clients, or if too much time has passed
 * since LAST for this to be reasonable, it may define RESUME=NEXT and replay
 * the absolute least history it can, probably only the stuff buffered in
 * memory during catchup.
 *
 * If RESUME=LAST, no buckets need to be downloaded and the system can simply
 * download history blocks; if RESUME=NEXT, no history blocks need to be
 * downloaded and the system it can simply download buckets. In theory the
 * system can be invoked with RESUME at some other value between LAST and NEXT
 * but in practice we expect these two modes to be the majority of uses.
 *
 * Once the blocks and/or buckets required are obtained, history is
 * reconstituted and/or replayed as required, and control calls back the
 * provided handler, with the value of NEXT as an argument. The handler should
 * then drop any ledgers <= NEXT that it has buffered, replay those ledgers
 * between NEXT and TIP exclusive, and declare itself "caught up".
 *
 */

namespace asio
{
typedef std::error_code error_code;
};

namespace stellar
{
class Application;
class Bucket;
class BucketList;
class Config;
class HistoryArchive;
struct StateSnapshot;

class HistoryManager
{
  public:

    // The two supported styles of catchup. CATCHUP_COMPLETE will replay all
    // history blocks, from the last closed ledger to the present, when catching
    // up; CATCHUP_MINIMAL will attempt to "fast forward" to the next BucketList
    // checkpoint, skipping the history log that happened between the last
    // closed ledger and the catchup point. This is set by config, default is
    // CATCHUP_MINIMAL but it should be CATCHUP_COMPLETE for any server with
    // API clients. See LedgerManager::startCatchUp and its callers for uses.
    enum CatchupMode
    {
        CATCHUP_COMPLETE,
        CATCHUP_MINIMAL,
        CATCHUP_BUCKET_REPAIR
    };

    // Status code returned from LedgerManager::verifyCatchupCandidate. The
    // HistoryManager's catchup algorithm downloads _untrusted_ history from a
    // configured history archive, then (once it has done internal consistency
    // checking of the chain of history it downloaded) calls
    // verifyCatchupCandidate to check the validity of a proposed target ledger
    // against the running consensus of the SCP protocol, thus turning untrusted
    // history into trusted history.
    //
    // LedgerManager will return VERIFY_HASH_OK if the proposed ledger is
    // definitely part of the consensus history chain (i.e. the ledger hash
    // matches the consensus for the provided ledger number); VERIFY_HASH_BAD if
    // the proposed ledger is definitely _not_ valid (i.e. if it has a different
    // hash than the consensus ledger with its number); or VERIFY_HASH_UNKNOWN
    // if the network consensus has not yet advanced to the proposed catchup
    // target.
    //
    // In the first case, catchup will proceed; in the second it will fail (and
    // restart, possibly against a different untrusted history archive); in the
    // third it will pause and retry the query after a timeout.
    enum VerifyHashStatus
    {
        VERIFY_HASH_OK,
        VERIFY_HASH_BAD,
        VERIFY_HASH_UNKNOWN
    };

    // Initialize a named history archive by writing
    // .well-known/stellar-history.json to it.
    static bool initializeHistoryArchive(Application& app, std::string arch);

    // Check that config settings are at least somewhat reasonable.
    static bool checkSensibleConfig(Config const& cfg);

    static std::unique_ptr<HistoryManager> create(Application& app);

    // Checkpoints are made every getCheckpointFrequency() ledgers.
    // This should normally be a constant (64) but in testing cases
    // may be different (see ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING).
    virtual uint32_t getCheckpointFrequency() = 0;

    // Given a ledger, tell when the next checkpoint will occur.
    virtual uint32_t nextCheckpointLedger(uint32_t ledger) = 0;

    // Given a ledger, tell the number of seconds to sleep until the next catchup probe.
    virtual uint64_t nextCheckpointCatchupProbe(uint32_t ledger) = 0;

    // Verify that a file has a given hash.
    virtual void verifyHash(std::string const& filename, uint256 const& hash,
                            std::function<void(asio::error_code const&)> handler) const = 0;

    // Gunzip a file.
    virtual void decompress(std::string const& filename_gz,
                            std::function<void(asio::error_code const&)> handler,
                            bool keepExisting = false) const = 0;

    // Gzip a file.
    virtual void compress(std::string const& filename_nogz,
                          std::function<void(asio::error_code const&)> handler,
                          bool keepExisting = false) const = 0;

    // Put a file to a specific archive using it's `put` command.
    virtual void putFile(std::shared_ptr<HistoryArchive const> archive,
                         std::string const& local, std::string const& remote,
                         std::function<void(asio::error_code const&)> handler) const = 0;

    // Get a file from a specific archive using it's `get` command.
    virtual void getFile(std::shared_ptr<HistoryArchive const> archive,
                         std::string const& remote, std::string const& local,
                         std::function<void(asio::error_code const&)> handler) const = 0;

    // Make a directory on a specific archive using its `mkdir` command.
    virtual void mkdir(std::shared_ptr<HistoryArchive const> archive,
                       std::string const& dir,
                       std::function<void(asio::error_code const&)> handler) const = 0;

    // Publish history if the current ledger is a multiple of
    // getCheckpointFrequency() -- equivalently, the LCL is one _less_ than a
    // multiple of getCheckpointFrequency() -- and no publish action is currently in
    // progress. Returns true if checkpoint publication of the LCL was started
    // (and the completion-handler queued), otherwise false.
    virtual bool
    maybePublishHistory(std::function<void(asio::error_code const&)> handler) = 0;

    virtual bool hasAnyWritableHistoryArchive() = 0;

    // Checkpoint the LCL -- both the log of history from the previous
    // checkpoint to it, as well as the bucketlist of its state -- to all
    // writable history archives.
    virtual void publishHistory(std::function<void(asio::error_code const&)> handler) = 0;

    virtual void downloadMissingBuckets(
        HistoryArchiveState desiredState,
        std::function<void(asio::error_code const&ec)> handler) = 0;

    // Run catchup, we've just heard `initLedger` from the network. Mode can be
    // CATCHUP_COMPLETE, meaning replay history from last to present, or
    // CATCHUP_MINIMAL, meaning snap to the next state possible and discard
    // history. See larger comment above for more detail.
    virtual void catchupHistory(
        uint32_t initLedger, CatchupMode mode,
        std::function<void(asio::error_code const& ec, CatchupMode mode,
                           LedgerHeaderHistoryEntry const& lastClosed)>
        handler) = 0;

    // Call posted after a worker thread has finished taking a snapshot; calls
    // PublishStateMachine::snapshotWritten after bumping counter.
    virtual void snapshotWritten(asio::error_code const&) = 0;

    // Return the HistoryArchiveState of the LedgerManager's LCL
    virtual HistoryArchiveState getLastClosedHistoryArchiveState() const = 0;

    // Return the name of the HistoryManager's tmpdir (used for storing files in
    // transit).
    virtual std::string const& getTmpDir() = 0;

    // Return the path of `basename` situated inside the HistoryManager's tmpdir.
    virtual std::string localFilename(std::string const& basename) = 0;

    // Return the number of checkpoints that have been skipped due to
    // unavailability of any publish targets.
    virtual uint64_t getPublishSkipCount() = 0;

    // Return the number of checkpoints that have been enqueued for
    // publication. This may be less than the number "started", but every
    // enqueued checkpoint should eventually start.
    virtual uint64_t getPublishQueueCount() = 0;

    // Return the number of enqueued checkpoints that have been delayed due to
    // the publish system being busy with a previous checkpoint. This indicates
    // a degree of overloading in the publish system.
    virtual uint64_t getPublishDelayCount() = 0;

    // Return the number of enqueued checkpoints that have "started", meaning
    // that their history logs have been written to disk and the publish system
    // has commenced running the external put commands for them.
    virtual uint64_t getPublishStartCount() = 0;

    // Return the number of checkpoints that completed publication successfully.
    virtual uint64_t getPublishSuccessCount() = 0;

    // Return the number of checkpoints that failed publication.
    virtual uint64_t getPublishFailureCount() = 0;

    // Return the number of times the process has commenced catchup.
    virtual uint64_t getCatchupStartCount() = 0;

    // Return the number of times the catchup has completed successfully.
    virtual uint64_t getCatchupSuccessCount() = 0;

    // Return the number of times the catchup has failed.
    virtual uint64_t getCatchupFailureCount() = 0;

    virtual ~HistoryManager() {};
};
}
