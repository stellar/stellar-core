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
              std::function<void(const TrustFrame&)> trustProcessor);

    TrustLineEntry& mTrustLine;

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

    static bool loadTrustLine(const uint256& accountID,
                              const Currency& currency, TrustFrame& retEntry,
                              Database& db);

    static void loadLines(const uint256& accountID,
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
