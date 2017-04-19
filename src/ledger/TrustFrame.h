#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryFrame.h"

namespace stellar
{

LedgerKey trustLineKey(AccountID accountID, Asset line);

class TrustFrame : public EntryFrame
{
  public:
    TrustFrame();
    explicit TrustFrame(AccountID accountID, Asset line, int64 limit,
                        bool authorized);
    explicit TrustFrame(Asset line);
    explicit TrustFrame(TrustLineEntry trustLine);
    explicit TrustFrame(LedgerEntry entry);

    AccountID const& getAccountID() const;
    void setAccountID(AccountID accountID);
    Asset const& getAsset() const;
    void setAsset(Asset asset);
    int64_t getBalance() const;
    bool addBalance(int64_t delta);
    int64_t getLimit() const;
    void setLimit(int64_t limit);
    uint32_t getFlags() const;
    bool isAuthorized() const;
    void setAuthorized(bool authorized);

    // returns the maximum amount that can be added to this trust line
    int64_t getMaxAmountReceive() const;
    bool isIssuer() const;
    bool isValid() const;
};
}
