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
    Hash mHash;

  public:
    typedef std::shared_ptr<LedgerHeaderFrame> pointer;

    LedgerHeader mHeader;

    LedgerHeaderFrame(LedgerHeader const& lh);
    LedgerHeaderFrame(LedgerHeaderHistoryEntry const& lastClosed); // creates a
                                                                   // new ledger
                                                                   // based on
                                                                   // the given
                                                                   // closed
                                                                   // ledger

    Hash const& getHash();

    SequenceNumber getStartingSequenceNumber(); // returns the first sequence
                                                // number to use for new
                                                // accounts

    void storeInsert(LedgerMaster& ledgerMaster);

    static LedgerHeaderFrame::pointer loadByHash(Hash const& hash,
                                                 Database& ledgerMaster);
    static LedgerHeaderFrame::pointer loadBySequence(uint32_t seq,
                                                     Database& ledgerMaster);

    static size_t copyLedgerHeadersToStream(Database& db, soci::session& sess,
                                            uint32_t ledgerSeq,
                                            uint32_t ledgerCount,
                                            XDROutputFileStream& headersOut);

    static void dropAll(Database& db);
    static const char* kSQLCreateStatement;

  private:
    static LedgerHeaderFrame::pointer decodeFromData(std::string const& data);
};
}
