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
class LedgerState;

class TrustLineReference
{
  public:
    virtual int64_t getBalance() const = 0;
    virtual bool addBalance(int64_t delta) = 0;

    virtual bool isAuthorized() const = 0;
    virtual void setAuthorized(bool authorized) = 0;

    virtual void setLimit(int64_t limit) = 0;

    // returns the maximum amount that can be added to this trust line
    virtual int64_t getMaxAmountReceive() const = 0;

    virtual void invalidate() = 0;

    virtual void forget(LedgerState& ls) = 0;

    virtual void erase() = 0;
};

class ExplicitTrustLineReference : public TrustLineReference
{
    std::shared_ptr<LedgerEntryReference> mEntry;

  public:
    ExplicitTrustLineReference(std::shared_ptr<LedgerEntryReference> const& ler);

    operator bool() const
    {
        return static_cast<bool>(mEntry);
    }

    bool operator!() const
    {
        return !mEntry;
    }

    TrustLineEntry& trustLine();
    TrustLineEntry const& trustLine() const;

    int64_t getBalance() const override;
    bool addBalance(int64_t delta) override;

    bool isAuthorized() const override;
    void setAuthorized(bool authorized) override;

    void setLimit(int64_t limit) override;

    // returns the maximum amount that can be added to this trust line
    int64_t getMaxAmountReceive() const override;

    void invalidate() override;

    void forget(LedgerState& ls) override;

    void erase() override;
};

class IssuerTrustLineReference : public TrustLineReference
{
    bool mValid;

  public:
    IssuerTrustLineReference();

    operator bool() const
    {
        return true;
    }

    bool operator!() const
    {
        return false;
    }

    int64_t getBalance() const override;
    bool addBalance(int64_t delta) override;

    bool isAuthorized() const override;
    void setAuthorized(bool authorized) override;

    void setLimit(int64_t limit) override;

    // returns the maximum amount that can be added to this trust line
    int64_t getMaxAmountReceive() const override;

    void invalidate() override;

    void forget(LedgerState& ls) override;

    void erase() override;
};
}
