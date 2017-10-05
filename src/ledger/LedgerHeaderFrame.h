#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"

namespace soci
{
class session;
}

namespace stellar
{
class LedgerManager;
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

    void storeInsert(LedgerManager& ledgerManager) const;

    static LedgerHeaderFrame::pointer loadByHash(Hash const& hash,
                                                 Database& db);
    static LedgerHeaderFrame::pointer loadBySequence(uint32_t seq, Database& db,
                                                     soci::session& sess);

    static size_t copyLedgerHeadersToStream(Database& db, soci::session& sess,
                                            uint32_t ledgerSeq,
                                            uint32_t ledgerCount,
                                            XDROutputFileStream& headersOut);

    static void deleteOldEntries(Database& db, uint32_t ledgerSeq);

    static void dropAll(Database& db);

  private:
    static bool isValid(LedgerHeader const& lh);
    static LedgerHeaderFrame::pointer decodeFromData(std::string const& data);

    static const char* kSQLCreateStatement;
};
}
