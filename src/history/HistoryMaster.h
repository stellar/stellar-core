#ifndef __HISTORYMASTER__
#define __HISTORYMASTER__
// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"

/**
 * The history module is responsible for storing, mirroring, and retrieving
 * "historical records" on disk and in longer-term (public, internet)
 * archives. These records take two forms:
 *
 *   1. Buckets from the BucketList -- partial or full snapshots of "state"
 *   2. Ledger Headers + Transactions -- a sequential log of "changes"
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
 *     space. It should be practical to store truncated partial histories.  It
 *     should employ large-chunk-size bulk transport over commodity bulk file
 *     service protocols whenever possible. It should include "cold" portions
 *     that are not rewritten, that can gradually be moved to (eg.) offline
 *     storage or glacier-class "slow" storage.
 *
 *   - Tolerable latency for two main scenarios: "new peer joins network"
 *     and "peer temporarily de-synchronized, needs to catch up".
 *
 * The history store is served to clients over HTTP. In the root of the history
 * store is a file identifying the history-store format version number, and
 * providing any parameters for it. As per RFC 5785, this information is stored
 * at /.well-known/stellar-history.json as a JSON file.
 *
 * The history store is divided into two sections: hot and cold. They are served
 * through HTTP under the subdirectories /hot and /cold, and may be mapped by
 * HTTP redirects to entirely different servers. In this implementation, /hot is
 * provided from front-end servers dynamically pulling data out of the SQL
 * database, and /cold is from longer-term external (cloud) storage. In theory
 * both may be provided from the same repository, but in practice it is easier
 * for /hot to be updatd in an ACID fashion along with the current ledger.
 *
 * The hot section stores ledger/tx-block records only, at a fine enough
 * granularity to support the "temporarily de-synced, catching up"
 * scenario. Groups of hot records are periodically made redundant by
 * coarser-granularity records stored to the cold section. Once redundant, hot
 * records are deleted from the hot section (though they may persist for a
 * time). Cold records are never (automatically) deleted, though of course
 * someone operating a history archive may choose to delete whatever they like.
 *
 * The granularity of the hot and cold sections is defined by two
 * per-history-store parameters, each defining a power-of-two, by default 0 (=1
 * ledger) and 8 (=256 ledgers). The first is the granularity of history blocks
 * written to the hot section. The second is the granularity of history blocks
 * written to the cold section. The cold-section granularity is also implicitly
 * the expiration threshold for the hot section.
 *
 * Bucket snapshots are written to the cold section as they occur, always.  Only
 * the .snap side of each bucket level is written; the .curr side (which is
 * frequently-changing) is reconstructed on the fly when catching up to history,
 * downloading between 0 and 14 snapshots to reconstruct each .curr bucket. In
 * order to reconstruct the bucket list (and acquire the changed-objects, to
 * write to the database) for ledger 0x6789a, for example, the following bucket
 * snapshots need to be downloaded:
 *
 *    For level 0: the L0 snap from 0x67890-0x67898 (for .snap)
 *                 txlog replay for 0x67898-0x6789a (for .curr)
 *
 *    For level 1: the L1 snap from 0x67800-0x67880 (for .snap)
 *                 the L0 snap from 0x67880-0x67888 (for .curr)
 *           and   the L0 snap from 0x67888-0x67890 (for .curr)
 *
 *    For level 2: the L2 snap from 0x67000-0x67800 (for .snap)
 *                 --- empty ---                    (for .curr)
 *
 *    For level 3: --- empty ---                    (for .snap)
 *                 the L2 snap from 0x60000-0x60800 (for .curr)
 *           and   the L2 snap from 0x60800-0x61000 (for .curr)
 *           and   the L2 snap from 0x61000-0x61800 (for .curr)
 *           and   the L2 snap from 0x61800-0x62000 (for .curr)
 *           and   the L2 snap from 0x62000-0x62800 (for .curr)
 *           and   the L2 snap from 0x62800-0x63000 (for .curr)
 *           and   the L2 snap from 0x63000-0x63800 (for .curr)
 *           and   the L2 snap from 0x63800-0x64000 (for .curr)
 *           and   the L2 snap from 0x64000-0x64800 (for .curr)
 *           and   the L2 snap from 0x64800-0x65000 (for .curr)
 *           and   the L2 snap from 0x65000-0x65800 (for .curr)
 *           and   the L2 snap from 0x65800-0x66000 (for .curr)
 *           and   the L2 snap from 0x66000-0x66800 (for .curr)
 *           and   the L2 snap from 0x66800-0x67000 (for .curr)
 *    (14 files)
 *
 *    For level 4: --- empty ---                    (for .snap)
 *                 the L3 snap from 0x00000-0x08000 (for .curr)
 *           and   the L3 snap from 0x08000-0x10000 (for .curr)
 *           and   the L3 snap from 0x10000-0x18000 (for .curr)
 *           and   the L3 snap from 0x18000-0x20000 (for .curr)
 *           and   the L3 snap from 0x20000-0x28000 (for .curr)
 *           and   the L3 snap from 0x28000-0x30000 (for .curr)
 *           and   the L3 snap from 0x30000-0x38000 (for .curr)
 *           and   the L3 snap from 0x38000-0x40000 (for .curr)
 *           and   the L3 snap from 0x40000-0x48000 (for .curr)
 *           and   the L3 snap from 0x48000-0x50000 (for .curr)
 *           and   the L3 snap from 0x50000-0x58000 (for .curr)
 *           and   the L3 snap from 0x58000-0x60000 (for .curr)
 *    (12 files)
 *
 * An alternative implementation could also replay everything from the txlog,
 * downloding no snapshots. We write both bucket snapshots and txlog to long
 * term storage as a form of redundant cross-checking.
 *
 * If we take 1000 transactions/second as our design target: each transaction is
 * (say) 256 bytes, and a ledger closes every 5s, then we have 1.28MB of
 * transactions per ledger, so hot blocks are "written" (made available by the
 * database) every ledger, and cold blocks are written every 256 ledgers (21min,
 * 327MB), and we accumulate 26,280 cold blocks totalling 33GB per year in long
 * term transaction storage. Plus bucket snapshots, which should *roughly* follow
 * the same size profile, times a scalar factor for difference in size between
 * a transaction and the objects it changes, but divided by a scalar factor
 * for the level of redundancy between transactions, where N txs affect the
 * same object.
 *
 * Buckets are stored by hash with 2-level 256-ary prefixing: if the bucket's
 * hex hash is <AABBCDEFGH...> then the bucket is stored as
 * /cold/state/AA/BB/<AABBCDE...>.xdr.gz
 *
 * History blocks are stored by ledger sequence number range, with 2-level
 * 256-ary prefixing based on the low-order 16 bits of the _block_ sequence
 * number, expressed as a 64bit hex filename. That is, if the block sequence
 * number (some power-of-two division of the first ledger sequence number in the
 * block) is 0x000000005432ABCD, then the block is stored as
 * /cold/history/CD/AB/0x000000005432ABCD.xdr.gz
 *
 * The first ledger-block in all history can therefore always be found as
 * /cold/history/00/00/0x0000000000000000.xdr.gz
 */

namespace stellar
{
class Application;
class HistoryMaster
{
    class Impl;
    std::unique_ptr<Impl> mImpl;

  public:
    std::string writeLedgerHistoryToFile(History const& hist);
    void readLedgerHistoryFromFile(std::string const& fname,
                                   History& hist);
    HistoryMaster(Application& app);
    ~HistoryMaster();
};
}

#endif
