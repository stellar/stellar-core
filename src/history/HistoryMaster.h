#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

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
 *   2. History blocks -- a sequential log of ledger hashes and transactions,
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
 * denotes the archive format version number, the most-recent bucket-state
 * snapshot (as a set of bucket hashes) and the checkpoint number of the
 * most-recent history block, and finally a flag indicating whether to
 * numerically directory-prefix the various referenced files. As per RFC 5785,
 * this information is stored at /.well-known/stellar-history.json as a JSON
 * file.
 *
 * Checkpoints are made every 64 ledgers, which (at 5s ledger close time) is
 * 320s or about 5m20s. There will be 11 checkpoints per hour, 270 per day, and
 * 98,550 per year. Counting checkpoints within a 32bit value gives 43,581 years
 * of service for the system.
 *
 * When catching up, the system chooses an anchor ledger and attempts to download
 * the state at that anchor as well as all history _after_ that anchor. The node
 * catching-up might therefore have to wait as long as a single checkpoint cycle
 * in order to acquire history it is missing. It simply waits and retries until
 * the history becomes available.
 *
 * Each checkpoint is described by a history archive state file whose name
 * includes the checkpoint number (as a 32-bit hex string) and stored in a
 * 3-level deep directory tree of hex digit prefixes. For example, checkpoint
 * number 0x12345678 will be described by file
 * state/12/34/56/state-0x12345678.json and the associated history block will be
 * written to the two files ledger/12/34/56/ledger-0x12345678.xdr.gz and
 * transaction/12/34/56/transaction-0x12345678.xdr.gz
 *
 * Bucket files accompanying each checkpoint are stored by hash name, again
 * separated by 3-level-deep hex prefixing, though as the hash is random, the
 * directories will fill up in random order, not sequentially: if the bucket's
 * hex hash is <AABBCCDEFG...> then the bucket is stored as
 * bucket/AA/BB/CC/bucket-<AABBCCDEFG...>.xdr.gz
 *
 * The first history block (containing the genesis ledger) can therefore always
 * be found in history/00/00/history-0x00000000.xdr.gz and described by
 * state/00/00/state-0x00000000.json. The buckets will all be empty in
 * that state.
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
class HistoryArchive;
struct StateSnapshot;

class HistoryMaster
{
    class Impl;
    std::unique_ptr<Impl> mImpl;

  public:

    static const uint32_t kCheckpointFrequency;

    // Verify that a file has a given hash.
    void verifyHash(std::string const& filename,
                    uint256 const& hash,
                    std::function<void(asio::error_code const&)> handler);

    // Gunzip a file.
    void decompress(std::string const& filename_gz,
                    std::function<void(asio::error_code const&)> handler,
                    bool keepExisting=false);

    // Gzip a file.
    void compress(std::string const& filename_nogz,
                  std::function<void(asio::error_code const&)> handler,
                  bool keepExisting=false);

    // Put a file to a specific archive using it's `put` command.
    void putFile(std::shared_ptr<HistoryArchive> archive,
                 std::string const& filename,
                 std::string const& basename,
                 std::function<void(asio::error_code const&)> handler);

    // Get a file from a specific archive using it's `get` command.
    void getFile(std::shared_ptr<HistoryArchive> archive,
                 std::string const& basename,
                 std::string const& filename,
                 std::function<void(asio::error_code const&)> handler);

    // Make a directory on a specific archive using its `mkdir` command.
    void mkdir(std::shared_ptr<HistoryArchive> archive,
               std::string const& hexdir,
               std::function<void(asio::error_code const&)> handler);

    // For each writable archive, put all buckets in the CLF that have changed
    void publishHistory(std::function<void(asio::error_code const&)> handler);

    // Pick a readable archive and set the bucketlist to its content.
    void catchupHistory(std::function<void(asio::error_code const&)> handler);

    // Call posted after a worker thread has finished taking a snapshot; calls
    // PublishStateMachine::snapshotTaken iff state machine is live.
    void snapshotTaken(asio::error_code const&,
                       std::shared_ptr<StateSnapshot>);

    HistoryArchiveState getCurrentHistoryArchiveState() const;

    std::string const& getTmpDir();

    static bool initializeHistoryArchive(Application& app, std::string arch);

    std::string localFilename(std::string const& basename);


    HistoryMaster(Application& app);
    ~HistoryMaster();
};
}


