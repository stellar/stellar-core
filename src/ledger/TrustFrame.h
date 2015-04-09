#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryFrame.h"
#include <functional>

namespace soci
{
namespace details
{
class prepare_temp_type;
}
}

namespace stellar
{

class TrustSetTx;

class TrustFrame : public EntryFrame
{
    static void getKeyFields(LedgerKey const& key, std::string& base58AccountID,
                             std::string& base58Issuer,
                             std::string& currencyCode);

    static void
    loadLines(soci::details::prepare_temp_type& prep,
              std::function<void(TrustFrame const&)> trustProcessor);

    TrustLineEntry& mTrustLine;

    void setAsIssuer(Currency const& issuer);
    bool mIsIssuer; // the TrustFrame fakes an infinite trustline for issuers

  public:
    typedef std::shared_ptr<TrustFrame> pointer;

    TrustFrame();
    TrustFrame(LedgerEntry const& from);
    TrustFrame(TrustFrame const& from);

    TrustFrame& operator=(TrustFrame const& other);

    EntryFrame::pointer
    copy() const
    {
        return EntryFrame::pointer(new TrustFrame(*this));
    }

    // Instance-based overrides of EntryFrame.
    void storeDelete(LedgerDelta& delta, Database& db) const override;
    void storeChange(LedgerDelta& delta, Database& db) const override;
    void storeAdd(LedgerDelta& delta, Database& db) const override;

    // Static helper that don't assume an instance.
    static void storeDelete(LedgerDelta& delta, Database& db,
                            LedgerKey const& key);
    static bool exists(Database& db, LedgerKey const& key);

    // returns the specified trustline or a generated one for issuers
    static bool loadTrustLine(AccountID const& accountID,
                              Currency const& currency, TrustFrame& retEntry,
                              Database& db);

    // note: only returns trust lines stored in the database
    static void loadLines(AccountID const& accountID,
                          std::vector<TrustFrame>& retLines, Database& db);

    int64_t getBalance() const;
    bool addBalance(int64_t delta);

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

    bool isValid() const;

    static void dropAll(Database& db);
    static const char* kSQLCreateStatement;
};
}
