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
class LedgerManagerImpl;
class Database;
class XDROutputFileStream;

class LedgerHeaderFrame
{
    mutable Hash mHash;

  public:
    typedef std::shared_ptr<LedgerHeaderFrame> pointer;

    LedgerHeader mHeader;

    // wraps the given ledger as is
    LedgerHeaderFrame(LedgerHeader const& lh);

    // creates a new, _subsequent_ ledger, following the provided closed ledger
    explicit LedgerHeaderFrame(LedgerHeaderHistoryEntry const& lastClosed);

    Hash const& getHash() const;

    // returns the first sequence number to use for new accounts
    SequenceNumber getStartingSequenceNumber() const;

    // methods to generate IDs
    uint64_t getLastGeneratedID() const;
    // generates a new ID and returns it
    uint64_t generateID();

    void storeInsert(LedgerManagerImpl& ledgerMaster) const;

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
