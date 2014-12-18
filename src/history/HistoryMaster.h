#ifndef __HISTORYMASTER__
#define __HISTORYMASTER__
// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "history/HistoryGateway.h"
#include "generated/StellarXDR.h"

/**
 * The history module is responsible for storing, mirroring, and retrieving
 * "historical records" on disk and in longer-term (public, internet)
 * archives. These records take two forms:
 *
 *   1. Buckets from the BucketList -- snapshots of "state"
 *   2. Ledger Headers + Transactions -- a sequential log of "changes"
 *
 * The module design attempts to satisfy several key constraints:
 *
 *   - Simplicity. Historical records must be simple enough that multiple
 *     implementation can digest them. The formats and protocols employed
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
 * The history store is divided into two sections: hot and cold. They are
 * served through HTTP under the subdirectories /hot and /cold, and may be
 * mapped by HTTP redirects to entirely different servers. We expect some
 * configurations to provide /hot from the local disks of validators and
 * /cold from longer-term external storage. Or both may be provided from
 * the same repository.
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
 * per-history-store parameters, each defining a power-of-two, by default 4 (=16
 * ledgers) and 12 (=4096 ledgers). The first is the granularity of history
 * blocks written to the hot section. The second is _both_ the granularity of
 * history blocks written to the cold section and also the granularity at which
 * complete BucketList snapshots are addedd to the cold section. The cold-section
 * granularity is also implicitly the expiration threshold for the hot section.
 *
 * If we take the worst-case scenario of 1000 transactions/second as our design
 * criterion: each transaction is (say) 200 bytes, and a ledger closes every 5s,
 * then we have 1MB of transactions per ledger, so hot blocks are (say) 16
 * ledgers (80 seconds, 16MB) and cold blocks are (say) 4096 ledgers (5.7h,
 * 4GB).
 *
 * In a smaller, simpler world of a mere 10tx/s, a hot block is 160kb and a cold
 * block is 40MB. Both of which are reasonable, to start.
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
class HistoryMaster : public HistoryGateway
{
    Application& mApp;

  public:
    std::string writeLedgerHistoryToFile(stellarxdr::History const& hist);
    void readLedgerHistoryFromFile(std::string const& fname,
                                   stellarxdr::History& hist);
    HistoryMaster(Application& app);
};
}

#endif
