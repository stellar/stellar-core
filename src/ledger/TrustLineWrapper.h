#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxnEntry.h"
#include "xdr/Stellar-ledger-entries.h"
#include <memory>

namespace stellar
{

class LedgerTxn;
class LedgerTxnHeader;

class TrustLineWrapper
{
    class AbstractImpl;
    class IssuerImpl;
    class NonIssuerImpl;

    std::unique_ptr<AbstractImpl> mImpl;

    std::unique_ptr<AbstractImpl> const& getImpl() const;

  public:
    TrustLineWrapper();
    TrustLineWrapper(AbstractLedgerTxn& ltx, AccountID const& accountID,
                     Asset const& asset);
    explicit TrustLineWrapper(LedgerTxnEntry&& entry);

    TrustLineWrapper(TrustLineWrapper const&) = delete;
    TrustLineWrapper& operator=(TrustLineWrapper const&) = delete;

    TrustLineWrapper(TrustLineWrapper&&) = default;
    TrustLineWrapper& operator=(TrustLineWrapper&&) = default;

    explicit operator bool() const;

    AccountID const& getAccountID() const;
    Asset const& getAsset() const;

    int64_t getBalance() const;
    bool addBalance(LedgerTxnHeader const& header, int64_t delta);

    int64_t getBuyingLiabilities(LedgerTxnHeader const& header);
    int64_t getSellingLiabilities(LedgerTxnHeader const& header);

    int64_t addBuyingLiabilities(LedgerTxnHeader const& header, int64_t delta);
    int64_t addSellingLiabilities(LedgerTxnHeader const& header, int64_t delta);

    bool isAuthorized() const;

    int64_t getAvailableBalance(LedgerTxnHeader const& header) const;

    int64_t getMaxAmountReceive(LedgerTxnHeader const& header) const;

    void deactivate();
};

class TrustLineWrapper::AbstractImpl
{
  public:
    AbstractImpl() = default;

    AbstractImpl(AbstractImpl const&) = delete;
    AbstractImpl& operator=(AbstractImpl const&) = delete;

    AbstractImpl(AbstractImpl&&) = delete;
    AbstractImpl& operator=(AbstractImpl&&) = delete;

    virtual ~AbstractImpl(){};

    virtual operator bool() const = 0;

    virtual AccountID const& getAccountID() const = 0;
    virtual Asset const& getAsset() const = 0;

    virtual int64_t getBalance() const = 0;
    virtual bool addBalance(LedgerTxnHeader const& header, int64_t delta) = 0;

    virtual int64_t getBuyingLiabilities(LedgerTxnHeader const& header) = 0;
    virtual int64_t getSellingLiabilities(LedgerTxnHeader const& header) = 0;

    virtual int64_t addBuyingLiabilities(LedgerTxnHeader const& header,
                                         int64_t delta) = 0;
    virtual int64_t addSellingLiabilities(LedgerTxnHeader const& header,
                                          int64_t delta) = 0;

    virtual bool isAuthorized() const = 0;

    virtual int64_t
    getAvailableBalance(LedgerTxnHeader const& header) const = 0;

    virtual int64_t
    getMaxAmountReceive(LedgerTxnHeader const& header) const = 0;
};

class ConstTrustLineWrapper
{
    class AbstractImpl;
    class IssuerImpl;
    class NonIssuerImpl;

    std::unique_ptr<AbstractImpl> mImpl;

    std::unique_ptr<AbstractImpl> const& getImpl() const;

  public:
    ConstTrustLineWrapper();
    ConstTrustLineWrapper(AbstractLedgerTxn& ltx, AccountID const& accountID,
                          Asset const& asset);
    explicit ConstTrustLineWrapper(ConstLedgerTxnEntry&& entry);

    ConstTrustLineWrapper(ConstTrustLineWrapper const&) = delete;
    ConstTrustLineWrapper& operator=(ConstTrustLineWrapper const&) = delete;

    ConstTrustLineWrapper(ConstTrustLineWrapper&&) = default;
    ConstTrustLineWrapper& operator=(ConstTrustLineWrapper&&) = default;

    explicit operator bool() const;

    int64_t getBalance() const;

    bool isAuthorized() const;

    int64_t getAvailableBalance(LedgerTxnHeader const& header) const;

    int64_t getMaxAmountReceive(LedgerTxnHeader const& header) const;

    void deactivate();
};

class ConstTrustLineWrapper::AbstractImpl
{
  public:
    AbstractImpl() = default;

    AbstractImpl(AbstractImpl const&) = delete;
    AbstractImpl& operator=(AbstractImpl const&) = delete;

    AbstractImpl(AbstractImpl&&) = delete;
    AbstractImpl& operator=(AbstractImpl&&) = delete;

    virtual ~AbstractImpl(){};

    virtual operator bool() const = 0;

    virtual int64_t getBalance() const = 0;

    virtual bool isAuthorized() const = 0;

    virtual int64_t
    getAvailableBalance(LedgerTxnHeader const& header) const = 0;

    virtual int64_t
    getMaxAmountReceive(LedgerTxnHeader const& header) const = 0;
};
}
