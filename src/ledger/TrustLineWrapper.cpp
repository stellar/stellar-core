// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/TrustLineWrapper.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnHeader.h"
#include "transactions/TransactionUtils.h"
#include "util/XDROperators.h"
#include "util/types.h"

namespace stellar
{

// Declarations of TrustLineWrapper implementations ---------------------------
class TrustLineWrapper::NonIssuerImpl : public TrustLineWrapper::AbstractImpl
{
    LedgerTxnEntry mEntry;

  public:
    NonIssuerImpl(LedgerTxnEntry&& entry);

    operator bool() const override;

    AccountID const& getAccountID() const override;
    Asset const& getAsset() const override;

    int64_t getBalance() const override;
    bool addBalance(LedgerTxnHeader const& header, int64_t delta) override;

    int64_t getBuyingLiabilities(LedgerTxnHeader const& header) override;
    int64_t getSellingLiabilities(LedgerTxnHeader const& header) override;

    int64_t addBuyingLiabilities(LedgerTxnHeader const& header,
                                 int64_t delta) override;
    int64_t addSellingLiabilities(LedgerTxnHeader const& header,
                                  int64_t delta) override;

    bool isAuthorized() const override;

    int64_t getAvailableBalance(LedgerTxnHeader const& header) const override;

    int64_t getMaxAmountReceive(LedgerTxnHeader const& header) const override;
};

class TrustLineWrapper::IssuerImpl : public TrustLineWrapper::AbstractImpl
{
    AccountID const mAccountID;
    Asset const mAsset;

  public:
    IssuerImpl(AccountID const& accountID, Asset const& asset);

    operator bool() const override;

    AccountID const& getAccountID() const override;
    Asset const& getAsset() const override;

    int64_t getBalance() const override;
    bool addBalance(LedgerTxnHeader const& header, int64_t delta) override;

    int64_t getBuyingLiabilities(LedgerTxnHeader const& header) override;
    int64_t getSellingLiabilities(LedgerTxnHeader const& header) override;

    int64_t addBuyingLiabilities(LedgerTxnHeader const& header,
                                 int64_t delta) override;
    int64_t addSellingLiabilities(LedgerTxnHeader const& header,
                                  int64_t delta) override;

    bool isAuthorized() const override;

    int64_t getAvailableBalance(LedgerTxnHeader const& header) const override;

    int64_t getMaxAmountReceive(LedgerTxnHeader const& header) const override;
};

// Implementation of TrustLineWrapper -----------------------------------------
TrustLineWrapper::TrustLineWrapper()
{
}

TrustLineWrapper::TrustLineWrapper(AbstractLedgerTxn& ltx,
                                   AccountID const& accountID,
                                   Asset const& asset)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        throw std::runtime_error("trustline for native asset");
    }

    if (!(getIssuer(asset) == accountID))
    {
        LedgerKey key(TRUSTLINE);
        key.trustLine().accountID = accountID;
        key.trustLine().asset = asset;
        auto entry = ltx.load(key);
        if (entry)
        {
            mImpl = std::make_unique<NonIssuerImpl>(std::move(entry));
        }
    }
    else
    {
        mImpl = std::make_unique<IssuerImpl>(accountID, asset);
    }
}

TrustLineWrapper::TrustLineWrapper(LedgerTxnEntry&& entry)
{
    if (entry)
    {
        mImpl = std::make_unique<NonIssuerImpl>(std::move(entry));
    }
}

TrustLineWrapper::operator bool() const
{
    return (bool)mImpl && (bool)(*mImpl);
}

AccountID const&
TrustLineWrapper::getAccountID() const
{
    return getImpl()->getAccountID();
}

Asset const&
TrustLineWrapper::getAsset() const
{
    return getImpl()->getAsset();
}

int64_t
TrustLineWrapper::getBalance() const
{
    return getImpl()->getBalance();
}

bool
TrustLineWrapper::addBalance(LedgerTxnHeader const& header, int64_t delta)
{
    return getImpl()->addBalance(header, delta);
}

int64_t
TrustLineWrapper::getBuyingLiabilities(LedgerTxnHeader const& header)
{
    return getImpl()->getBuyingLiabilities(header);
}

int64_t
TrustLineWrapper::getSellingLiabilities(LedgerTxnHeader const& header)
{
    return getImpl()->getSellingLiabilities(header);
}

int64_t
TrustLineWrapper::addBuyingLiabilities(LedgerTxnHeader const& header,
                                       int64_t delta)
{
    return getImpl()->addBuyingLiabilities(header, delta);
}

int64_t
TrustLineWrapper::addSellingLiabilities(LedgerTxnHeader const& header,
                                        int64_t delta)
{
    return getImpl()->addSellingLiabilities(header, delta);
}

bool
TrustLineWrapper::isAuthorized() const
{
    return getImpl()->isAuthorized();
}

int64_t
TrustLineWrapper::getAvailableBalance(LedgerTxnHeader const& header) const
{
    return getImpl()->getAvailableBalance(header);
}

int64_t
TrustLineWrapper::getMaxAmountReceive(LedgerTxnHeader const& header) const
{
    return getImpl()->getMaxAmountReceive(header);
}

void
TrustLineWrapper::deactivate()
{
    mImpl.reset();
}

std::unique_ptr<TrustLineWrapper::AbstractImpl> const&
TrustLineWrapper::getImpl() const
{
    if (!(*this))
    {
        throw std::runtime_error("TrustLineWrapper not active");
    }
    return mImpl;
}

// Implementation of TrustLineWrapper::NonIssuerImpl --------------------------
TrustLineWrapper::NonIssuerImpl::NonIssuerImpl(LedgerTxnEntry&& entry)
    : mEntry(std::move(entry))
{
}

TrustLineWrapper::NonIssuerImpl::operator bool() const
{
    return (bool)mEntry;
}

AccountID const&
TrustLineWrapper::NonIssuerImpl::getAccountID() const
{
    return mEntry.current().data.trustLine().accountID;
}

Asset const&
TrustLineWrapper::NonIssuerImpl::getAsset() const
{
    return mEntry.current().data.trustLine().asset;
}

int64_t
TrustLineWrapper::NonIssuerImpl::getBalance() const
{
    return mEntry.current().data.trustLine().balance;
}

bool
TrustLineWrapper::NonIssuerImpl::addBalance(LedgerTxnHeader const& header,
                                            int64_t delta)
{
    return stellar::addBalance(header, mEntry, delta);
}

int64_t
TrustLineWrapper::NonIssuerImpl::getBuyingLiabilities(
    LedgerTxnHeader const& header)
{
    return stellar::getBuyingLiabilities(header, mEntry);
}

int64_t
TrustLineWrapper::NonIssuerImpl::getSellingLiabilities(
    LedgerTxnHeader const& header)
{
    return stellar::getSellingLiabilities(header, mEntry);
}

int64_t
TrustLineWrapper::NonIssuerImpl::addBuyingLiabilities(
    LedgerTxnHeader const& header, int64_t delta)
{
    return stellar::addBuyingLiabilities(header, mEntry, delta);
}

int64_t
TrustLineWrapper::NonIssuerImpl::addSellingLiabilities(
    LedgerTxnHeader const& header, int64_t delta)
{
    return stellar::addSellingLiabilities(header, mEntry, delta);
}

bool
TrustLineWrapper::NonIssuerImpl::isAuthorized() const
{
    return stellar::isAuthorized(mEntry);
}

int64_t
TrustLineWrapper::NonIssuerImpl::getAvailableBalance(
    LedgerTxnHeader const& header) const
{
    return stellar::getAvailableBalance(header, mEntry);
}

int64_t
TrustLineWrapper::NonIssuerImpl::getMaxAmountReceive(
    LedgerTxnHeader const& header) const
{
    return stellar::getMaxAmountReceive(header, mEntry);
}

// Implementation of TrustLineWrapper::IssuerImpl -----------------------------
TrustLineWrapper::IssuerImpl::IssuerImpl(AccountID const& accountID,
                                         Asset const& asset)
    : mAccountID(accountID), mAsset(asset)
{
}

TrustLineWrapper::IssuerImpl::operator bool() const
{
    return true;
}

AccountID const&
TrustLineWrapper::IssuerImpl::getAccountID() const
{
    return mAccountID;
}

Asset const&
TrustLineWrapper::IssuerImpl::getAsset() const
{
    return mAsset;
}

int64_t
TrustLineWrapper::IssuerImpl::getBalance() const
{
    return INT64_MAX;
}

bool
TrustLineWrapper::IssuerImpl::addBalance(LedgerTxnHeader const& header,
                                         int64_t delta)
{
    return true;
}

int64_t
TrustLineWrapper::IssuerImpl::getBuyingLiabilities(
    LedgerTxnHeader const& header)
{
    return 0;
}

int64_t
TrustLineWrapper::IssuerImpl::getSellingLiabilities(
    LedgerTxnHeader const& header)
{
    return 0;
}

int64_t
TrustLineWrapper::IssuerImpl::addBuyingLiabilities(
    LedgerTxnHeader const& header, int64_t delta)
{
    return true;
}

int64_t
TrustLineWrapper::IssuerImpl::addSellingLiabilities(
    LedgerTxnHeader const& header, int64_t delta)
{
    return true;
}

bool
TrustLineWrapper::IssuerImpl::isAuthorized() const
{
    return true;
}

int64_t
TrustLineWrapper::IssuerImpl::getAvailableBalance(
    LedgerTxnHeader const& header) const
{
    return INT64_MAX;
}

int64_t
TrustLineWrapper::IssuerImpl::getMaxAmountReceive(
    LedgerTxnHeader const& header) const
{
    return INT64_MAX;
}

// Declarations of ConstTrustLineWrapper implementations ----------------------
class ConstTrustLineWrapper::NonIssuerImpl
    : public ConstTrustLineWrapper::AbstractImpl
{
    ConstLedgerTxnEntry mEntry;

  public:
    NonIssuerImpl(ConstLedgerTxnEntry&& entry);

    operator bool() const override;

    int64_t getBalance() const override;

    bool isAuthorized() const override;

    int64_t getAvailableBalance(LedgerTxnHeader const& header) const override;

    int64_t getMaxAmountReceive(LedgerTxnHeader const& header) const override;
};

class ConstTrustLineWrapper::IssuerImpl
    : public ConstTrustLineWrapper::AbstractImpl
{
  public:
    operator bool() const override;

    int64_t getBalance() const override;

    bool isAuthorized() const override;

    int64_t getAvailableBalance(LedgerTxnHeader const& header) const override;

    int64_t getMaxAmountReceive(LedgerTxnHeader const& header) const override;
};

// Implementation of ConstTrustLineWrapper ------------------------------------
ConstTrustLineWrapper::ConstTrustLineWrapper()
{
}

ConstTrustLineWrapper::ConstTrustLineWrapper(AbstractLedgerTxn& ltx,
                                             AccountID const& accountID,
                                             Asset const& asset)
{
    if (!(getIssuer(asset) == accountID))
    {
        LedgerKey key(TRUSTLINE);
        key.trustLine().accountID = accountID;
        key.trustLine().asset = asset;
        auto entry = ltx.loadWithoutRecord(key);
        if (entry)
        {
            mImpl = std::make_unique<NonIssuerImpl>(std::move(entry));
        }
    }
    else
    {
        mImpl = std::make_unique<IssuerImpl>();
    }
}

ConstTrustLineWrapper::ConstTrustLineWrapper(ConstLedgerTxnEntry&& entry)
{
    if (entry)
    {
        mImpl = std::make_unique<NonIssuerImpl>(std::move(entry));
    }
}

ConstTrustLineWrapper::operator bool() const
{
    return (bool)mImpl && (bool)(*mImpl);
}

int64_t
ConstTrustLineWrapper::getBalance() const
{
    return getImpl()->getBalance();
}

bool
ConstTrustLineWrapper::isAuthorized() const
{
    return getImpl()->isAuthorized();
}

int64_t
ConstTrustLineWrapper::getAvailableBalance(LedgerTxnHeader const& header) const
{
    return getImpl()->getAvailableBalance(header);
}

int64_t
ConstTrustLineWrapper::getMaxAmountReceive(LedgerTxnHeader const& header) const
{
    return getImpl()->getMaxAmountReceive(header);
}

std::unique_ptr<ConstTrustLineWrapper::AbstractImpl> const&
ConstTrustLineWrapper::getImpl() const
{
    if (!(*this))
    {
        throw std::runtime_error("ConstTrustLineWrapper not active");
    }
    return mImpl;
}

// Implementation of ConstTrustLineWrapper::NonIssuerImpl ---------------------
ConstTrustLineWrapper::NonIssuerImpl::NonIssuerImpl(ConstLedgerTxnEntry&& entry)
    : mEntry(std::move(entry))
{
}

ConstTrustLineWrapper::NonIssuerImpl::operator bool() const
{
    return (bool)mEntry;
}

int64_t
ConstTrustLineWrapper::NonIssuerImpl::getBalance() const
{
    return mEntry.current().data.trustLine().balance;
}

bool
ConstTrustLineWrapper::NonIssuerImpl::isAuthorized() const
{
    return stellar::isAuthorized(mEntry);
}

int64_t
ConstTrustLineWrapper::NonIssuerImpl::getAvailableBalance(
    LedgerTxnHeader const& header) const
{
    return stellar::getAvailableBalance(header, mEntry);
}

int64_t
ConstTrustLineWrapper::NonIssuerImpl::getMaxAmountReceive(
    LedgerTxnHeader const& header) const
{
    return stellar::getMaxAmountReceive(header, mEntry);
}

// Implementation of ConstTrustLineWrapper::IssuerImpl ------------------------
ConstTrustLineWrapper::IssuerImpl::operator bool() const
{
    return true;
}

int64_t
ConstTrustLineWrapper::IssuerImpl::getBalance() const
{
    return INT64_MAX;
}

bool
ConstTrustLineWrapper::IssuerImpl::isAuthorized() const
{
    return true;
}

int64_t
ConstTrustLineWrapper::IssuerImpl::getAvailableBalance(
    LedgerTxnHeader const& header) const
{
    return INT64_MAX;
}

int64_t
ConstTrustLineWrapper::IssuerImpl::getMaxAmountReceive(
    LedgerTxnHeader const& header) const
{
    return INT64_MAX;
}
}
