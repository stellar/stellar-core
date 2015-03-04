#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include "lib/json/json.h"

namespace soci
{
class session;
}

namespace stellar
{
    class LedgerMaster;
    class Database;
    class XDROutputFileStream;

    class LedgerHeaderFrame
    {
    public:
        typedef std::shared_ptr<LedgerHeaderFrame> pointer;

        LedgerHeader mHeader;

        LedgerHeaderFrame() {}
        LedgerHeaderFrame(LedgerHeader lh);
        LedgerHeaderFrame(LedgerHeaderFrame::pointer previousLedger);

        void computeHash();

        void storeInsert(LedgerMaster& ledgerMaster);

        static LedgerHeaderFrame::pointer loadByHash(const uint256 &hash, LedgerMaster& ledgerMaster);
        static LedgerHeaderFrame::pointer loadBySequence(uint64_t seq, LedgerMaster& ledgerMaster);

        static size_t copyLedgerHeadersToStream(Database& db,
                                                soci::session& sess,
                                                uint64_t ledgerSeq,
                                                uint64_t ledgerCount,
                                                XDROutputFileStream& txOut);

        static void dropAll(Database &db);
        static const char *kSQLCreateStatement;
    };
}

