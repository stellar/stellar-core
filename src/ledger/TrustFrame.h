#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/AccountFrame.h"
#include "ledger/EntryFrame.h"
#include <functional>
#include <unordered_map>

namespace soci
{
class session;
namespace details
{
class prepare_temp_type;
}
}

namespace stellar
{

class LedgerRange;
class TrustSetTx;
class StatementContext;

class TrustFrame : public EntryFrame
{
  public:
    typedef std::shared_ptr<TrustFrame> pointer;

  private:
    static void getKeyFields(LedgerKey const& key, std::string& actIDStrKey,
                             std::string& issuerStrKey, std::string& assetCode);

    static void
    loadLines(StatementContext& prep,
              std::function<void(LedgerEntry const&)> trustProcessor);

    TrustLineEntry& mTrustLine;

    static TrustFrame::pointer createIssuerFrame(Asset const& issuer);
    bool mIsIssuer; // the TrustFrame fakes an infinite trustline for issuers

    TrustFrame& operator=(TrustFrame const& other);

  public:
    TrustFrame();
    TrustFrame(LedgerEntry const& from);
    TrustFrame(TrustFrame const& from);

    EntryFrame::pointer
    copy() const override
    {
        return std::make_shared<TrustFrame>(*this);
    }

    // Instance-based overrides of EntryFrame.
    void storeDelete(LedgerDelta& delta, Database& db) const override;
    void storeChange(LedgerDelta& delta, Database& db) override;
    void storeAdd(LedgerDelta& delta, Database& db) override;

    // Static helper that don't assume an instance.
    static void storeDelete(LedgerDelta& delta, Database& db,
                            LedgerKey const& key);
    static bool exists(Database& db, LedgerKey const& key);
    static uint64_t countObjects(soci::session& sess);
    static uint64_t countObjects(soci::session& sess,
                                 LedgerRange const& ledgers);
    static void deleteTrustLinesModifiedOnOrAfterLedger(Database& db,
                                                        uint32_t oldestLedger);

    // returns the specified trustline or a generated one for issuers
    static pointer loadTrustLine(AccountID const& accountID, Asset const& asset,
                                 Database& db, LedgerDelta* delta = nullptr);

    // overload that also returns the issuer
    static std::pair<TrustFrame::pointer, AccountFrame::pointer>
    loadTrustLineIssuer(AccountID const& accountID, Asset const& asset,
                        Database& db, LedgerDelta& delta);

    // note: only returns trust lines stored in the database
    static void loadLines(AccountID const& accountID,
                          std::vector<TrustFrame::pointer>& retLines,
                          Database& db);

    // loads ALL trust lines from the database (very slow!)
    static std::unordered_map<AccountID, std::vector<TrustFrame::pointer>>
    loadAllLines(Database& db);

    int64_t getBalance() const;
    bool addBalance(int64_t delta);

    bool isAuthorized() const;
    void setAuthorized(bool authorized);

    // returns the maximum amount that can be added to this trust line
    int64_t getMaxAmountReceive() const;

    TrustLineEntry const&
    getTrustLine() const
    {
        return mTrustLine;
    }
    TrustLineEntry&
    getTrustLine()
    {
        return mTrustLine;
    }

    static void dropAll(Database& db);

  private:
    static const char* kSQLCreateStatement1;
    static const char* kSQLCreateStatement2;
};
}
