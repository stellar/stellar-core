#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxSetFrame.h"
#include "history/HistoryArchive.h"
#include "overlay/StellarXDR.h"
#include "util/GlobalChecks.h"
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
 * Checkpoints are made "every 64 ledgers", when LCL is one-less-than a multiple
 * of 64. In other words, at LCL=63, 127, 191, 255, etc. or in other other words
 * checkpoint K covers the inclusive ledger range [K*64, ((K+1)*64)-1], and each
 * of those ranges should contain exactly 64 ledgers, with the exception of the
 * first checkpoint, which has only 63 ledgers: there is no ledger 0.
 *
 * Checkpointing every 64 ledgers means (at 5s ledger close time) each
 * checkpoint happens every 320s, or about 5m20s. There will be 11 checkpoints
 * per hour, 270 per day, and 98,550 per year. Counting checkpoints within a
 * 32bit value gives 43,581 years of service for the system.
 *
 * While the _most recent_ checkpoint is in .well-known/stellar-history.json,
 * each checkpoint is also stored permanently at a path whose name includes the
 * last ledger number in the checkpoint (as a 32-bit hex string) and stored in a
 * 3-level deep directory tree of hex digit prefixes.
 *
 * For example, checkpoint made with LCL=0x12345677 will be described by file
 * history/12/34/56/history-0x12345677.json and the associated history block
 * will be written to the two files ledger/12/34/56/ledger-0x12345677.xdr.gz and
 * transaction/12/34/56/transaction-0x12345677.xdr.gz
 *
 * Bucket files accompanying each checkpoint are stored by hash name, again
 * separated by 3-level-deep hex prefixing, though as the hash is unpredictable,
 * the directories will fill up in unpredictable order, not sequentially: if the
 * bucket's hex hash is <AABBCCDEFG...> then the bucket is stored as
 * bucket/AA/BB/CC/bucket-<AABBCCDEFG...>.xdr.gz
 *
 * The first ledger and transaction files (containing the genesis ledger #1, and
 * the subsequent 62 ledgers) are checkpointed at LCL 63 (a.k.a. 0x3f), and it's
 * stored in files ledger/00/00/00/ledger-0x0000003f.xdr.gz and
 * transaction/00/00/00/transaction-0x0000003f.xdr.gz, and described by
 * history/00/00/00/history-0x0000003f.json.
 *
 * A pseudo-checkpoint describing the system-state before any transactions are
 * applied -- the fictional "ledger zero" state -- is also made in each history
 * archive when it is initialized. This is described by
 * history/00/00/00/history-0x00000000.json, with all-zero buckets. No
 * transaction or ledger-history XDR files are associated with this
 * pseudo-checkpoint; its presence simply indicates that the archive has been
 * initialized, and gives the catchup system something to read when it probes
 * the archive for a most-recent state.
 *
 *
 * Boundary conditions and counts:
 * -------------------------------
 *
 * There is no ledger 0 -- that's the sequence number of a _fictional_ ledger
 * with no content, before "ledger 1, the genesis ledger" -- so the initial
 * ledger block (block 0x0000003f) has 63 "real" ledger objects in it, not 64 as
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
 * RESUME, INIT, NEXT and TIP.
 * The following explains the logic involving these 4 points:
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
 * reconstituted "directly" from the bucket list, and from which history blocks
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
class LiveBucketList;
class Config;
class Database;
class HistoryArchive;
struct StateSnapshot;

class HistoryManager
{
  public:
    // Status code returned from LedgerManager::verifyCatchupCandidate. Look
    // there for additional documentation.
    enum LedgerVerificationStatus
    {
        VERIFY_STATUS_OK,
        VERIFY_STATUS_ERR_BAD_HASH,
        VERIFY_STATUS_ERR_BAD_LEDGER_VERSION,
        VERIFY_STATUS_ERR_OVERSHOT,
        VERIFY_STATUS_ERR_UNDERSHOT,
        VERIFY_STATUS_ERR_MISSING_ENTRIES
    };

    // Check that config settings are at least somewhat reasonable.
    static bool checkSensibleConfig(Config const& cfg);

    static std::unique_ptr<HistoryManager> create(Application& app);

    // Initialize DB table for persistent publishing queue.
    static void dropAll(Database& db);
    static std::filesystem::path publishQueuePath(Config const& cfg);
    static void createPublishQueueDir(Config const& cfg);

    // Checkpoints are made every getCheckpointFrequency() ledgers.
    // This should normally be a constant (64) but in testing cases
    // may be different (see ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING).
    static uint32_t getCheckpointFrequency(Config const& cfg);

    // Return checkpoint that contains given ledger. Checkpoint is identified
    // by last ledger in range. This does not consult the network nor take
    // account of manual checkpoints.
    static uint32_t
    checkpointContainingLedger(uint32_t ledger, Config const& cfg)
    {
        uint32_t freq = getCheckpointFrequency(cfg);
        // Round-up to next multiple of freq, then subtract 1 since checkpoints
        // are numbered for (and cover ledgers up to) the last ledger in them,
        // which is one-before the next multiple of freq.
        return (((ledger / freq) + 1) * freq) - 1;
    }

    // Return true iff closing `ledger` should cause publishing a checkpoint.
    // Equivalent to `ledger == checkpointContainingLedger(ledger)` but a little
    // more obviously named.
    static bool
    publishCheckpointOnLedgerClose(uint32_t ledger, Config const& cfg)
    {
        return checkpointContainingLedger(ledger, cfg) == ledger;
    }

    static bool
    isFirstLedgerInCheckpoint(uint32_t ledger, Config const& cfg)
    {
        return firstLedgerInCheckpointContaining(ledger, cfg) == ledger;
    }

    static bool
    isLastLedgerInCheckpoint(uint32_t ledger, Config const& cfg)
    {
        return checkpointContainingLedger(ledger, cfg) == ledger;
    }

    // Return the number of ledgers in the checkpoint containing a given ledger.
    static uint32_t
    sizeOfCheckpointContaining(uint32_t ledger, Config const& cfg)
    {
        uint32_t freq = getCheckpointFrequency(cfg);
        if (ledger < freq)
        {
            return freq - 1;
        }
        return freq;
    }

    // Return the first ledger in the checkpoint containing a given ledger.
    static uint32_t
    firstLedgerInCheckpointContaining(uint32_t ledger, Config const& cfg)
    {
        uint32_t last =
            checkpointContainingLedger(ledger, cfg); // == 63, 127, 191
        uint32_t size =
            sizeOfCheckpointContaining(ledger, cfg); // == 63, 64, 64
        return last - (size - 1);                    // == 1, 64, 128
    }

    // Return the first ledger after the checkpoint containing a given ledger.
    static uint32_t
    firstLedgerAfterCheckpointContaining(uint32_t ledger, Config const& cfg)
    {
        uint32_t first =
            firstLedgerInCheckpointContaining(ledger, cfg); // == 1, 64, 128
        uint32_t size =
            sizeOfCheckpointContaining(ledger, cfg); // == 63, 64, 64
        return first + size;                         // == 64, 128, 192
    }

    // Return the last ledger before the checkpoint containing a given ledger,
    // or zero if `ledger` is contained inside the first checkpoint.
    static uint32_t
    lastLedgerBeforeCheckpointContaining(uint32_t ledger, Config const& cfg)
    {
        uint32_t last =
            checkpointContainingLedger(ledger, cfg); // == 63, 127, 191
        uint32_t size =
            sizeOfCheckpointContaining(ledger, cfg); // == 63, 64, 64
        releaseAssert(last >= size);
        return last - size; // == 0, 63, 127
    }

    // Return the ledger to trigger the catchup machinery on, given a ledger
    // that is the start of a checkpoint buffered in the catchup manager.
    static uint32_t
    ledgerToTriggerCatchup(uint32_t firstLedgerOfBufferedCheckpoint,
                           Config const& cfg)
    {
        releaseAssert(
            isFirstLedgerInCheckpoint(firstLedgerOfBufferedCheckpoint, cfg));
        return firstLedgerOfBufferedCheckpoint + 1;
    }

    // Return the length of the current publishing queue.
    static size_t publishQueueLength(Config const& cfg);

    // Emit a log message and set StatusManager HISTORY_PUBLISH status to
    // describe current publish state.
    virtual void logAndUpdatePublishStatus() = 0;

    // Calls queueCurrentHistory() if the current ledger is a multiple of
    // getCheckpointFrequency() -- equivalently, the LCL is one _less_ than
    // a multiple of getCheckpointFrequency(). Returns true if checkpoint
    // publication of the LCL was queued, otherwise false. ledgerVers must align
    // with lcl.
    virtual bool maybeQueueHistoryCheckpoint(uint32_t lcl,
                                             uint32_t ledgerVers) = 0;

    // Checkpoint the LCL -- both the log of history from the previous
    // checkpoint to it, as well as the bucketlist of its state -- to a
    // publication-queue in the database. This should be followed shortly
    // (typically after commit) with a call to publishQueuedHistory. ledgerVers
    // must align with lcl.
    virtual void queueCurrentHistory(uint32_t lcl, uint32_t ledgerVers) = 0;

    // Return the youngest ledger still in the outgoing publish queue;
    // returns 0 if the publish queue has nothing in it.
    static uint32_t getMinLedgerQueuedToPublish(Config const& cfg);

    // Return the oldest ledger still in the outgoing publish queue;
    // returns 0 if the publish queue has nothing in it.
    static uint32_t getMaxLedgerQueuedToPublish(Config const& cfg);

    // Publish any checkpoints queued (in the database) for publication.
    // Returns the number of publishes initiated.
    virtual size_t publishQueuedHistory() = 0;

    // Prepare checkpoint files for publishing
    virtual void maybeCheckpointComplete(uint32_t lcl) = 0;

    // Migrate SQL-based publish queue to the new file format
    // (one-time call during database schema upgrade path)
    virtual void dropSQLBasedPublish() = 0;

    // Return the set of buckets referenced by the persistent (DB) publish
    // queue that are not present in the BucketManager. These need to be
    // fetched from somewhere before publishing can begin again.
    virtual std::vector<std::string>
    getMissingBucketsReferencedByPublishQueue() = 0;

    // Return the set of buckets referenced by the persistent (DB) publish
    // queue.
    static std::set<std::string>
    getBucketsReferencedByPublishQueue(Config const& cfg);

    // Return the full set of HistoryArchiveStates in the persistent (DB)
    // publish queue.
    static std::vector<HistoryArchiveState>
    getPublishQueueStates(Config const& cfg);

    // Callback from Publication, indicates that a given snapshot was
    // published. The `success` parameter indicates whether _all_ the
    // configured archives published correctly; if so the snapshot
    // can be dequeued, otherwise it should remain and be tried again
    // later.
    virtual void
    historyPublished(uint32_t ledgerSeq,
                     std::vector<std::string> const& originalBuckets,
                     bool success) = 0;

    virtual void
    appendTransactionSet(uint32_t ledgerSeq, TxSetXDRFrameConstPtr const& txSet,
                         TransactionResultSet const& resultSet) = 0;
    virtual void appendLedgerHeader(LedgerHeader const& header) = 0;

    // On startup, restore checkpoint files based on the last committed LCL
    virtual void restoreCheckpoint(uint32_t lcl) = 0;

    // Cleanup published files. If core is reset to genesis, any unpublished
    // files will be cleaned by removal of the buckets directory.
    static void deletePublishedFiles(uint32_t ledgerSeq, Config const& cfg);

    // Return the name of the HistoryManager's tmpdir (used for storing files in
    // transit).
    virtual std::string const& getTmpDir() = 0;

    // Return the path of `basename` situated inside the HistoryManager's
    // tmpdir.
    virtual std::string localFilename(std::string const& basename) = 0;

    // Return the number of checkpoints that have been enqueued for
    // publication. This may be less than the number "started", but every
    // enqueued checkpoint should eventually start.
    virtual uint64_t getPublishQueueCount() const = 0;

    // Return the number of checkpoints that completed publication successfully.
    virtual uint64_t getPublishSuccessCount() const = 0;

    // Return the number of checkpoints that failed publication.
    virtual uint64_t getPublishFailureCount() const = 0;

#ifdef BUILD_TESTS
    // Enable or disable history publication, purely a testing interface.
    // History is still queued when publication is disabled.
    virtual void setPublicationEnabled(bool enabled) = 0;
#endif

    virtual ~HistoryManager(){};

    virtual Config const& getConfig() const = 0;
};
}
