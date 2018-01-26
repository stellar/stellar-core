#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerEntryReference.h"
#include "xdr/Stellar-ledger-entries.h"
#include <memory>

namespace stellar
{

class LedgerManager;

class TrustLine
{
  public:
    virtual bool addBalance(int64_t delta) = 0;

    virtual bool isAuthorized() const = 0;

    // returns the maximum amount that can be added to this trust line
    virtual int64_t getMaxAmountReceive() const = 0;
};

class TrustLineReference : public TrustLine
{
    std::shared_ptr<LedgerEntryReference> mEntry;

  public:
    TrustLineReference(std::shared_ptr<LedgerEntryReference> const& ler);

    TrustLineEntry& trustLine();
    TrustLineEntry const& trustLine() const;

    int64_t getBalance() const;
    bool addBalance(int64_t delta) override;

    bool isAuthorized() const override;
    void setAuthorized(bool authorized);

    // returns the maximum amount that can be added to this trust line
    int64_t getMaxAmountReceive() const override;
};

class IssuerLine : public TrustLine
{
  public:
    bool addBalance(int64_t delta) override;

    bool isAuthorized() const override;

    // returns the maximum amount that can be added to this trust line
    int64_t getMaxAmountReceive() const override;
};
}
