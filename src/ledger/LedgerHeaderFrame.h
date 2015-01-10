#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include "lib/json/json.h"

namespace stellar
{
    class LedgerMaster;
    class Database;

    class LedgerHeaderFrame
    {
    public:
        typedef std::shared_ptr<LedgerHeaderFrame> pointer;

        LedgerHeader mHeader;

        LedgerHeaderFrame(LedgerHeader lh);
        LedgerHeaderFrame(LedgerHeaderFrame::pointer previousLedger);

        void computeHash();

        void storeInsert(LedgerMaster& ledgerMaster);

        static LedgerHeaderFrame loadByHash(const uint256 &hash, LedgerMaster& ledgerMaster);
        static LedgerHeaderFrame loadBySequence(uint64_t seq, LedgerMaster& ledgerMaster);

        static void dropAll(Database &db);
        static const char *kSQLCreateStatement;
    };
}

