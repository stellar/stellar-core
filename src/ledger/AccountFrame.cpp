// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/AccountFrame.h"
#include "ledger/LedgerManager.h"
#include "util/types.h"

#include <algorithm>

namespace stellar
{

using xdr::operator==;

LedgerKey accountKey(AccountID accountID)
{
    auto key = LedgerKey{};
    key.type(ACCOUNT);
    key.account().accountID = std::move(accountID);
    return key;
}

AccountFrame::AccountFrame(AccountID accountID, int64_t balance,
                           SequenceNumber seqNum)
{
    mEntry.data.type(ACCOUNT);
    mEntry.data.account().accountID = std::move(accountID);
    mEntry.data.account().balance = balance;
    mEntry.data.account().seqNum = seqNum;
    mEntry.data.account().thresholds[0] = 1;
}

AccountFrame::AccountFrame(AccountID accountID)
{
    mEntry.data.type(ACCOUNT);
    mEntry.data.account().accountID = std::move(accountID);
    mEntry.data.account().thresholds[0] = 1;
}

AccountFrame::AccountFrame(AccountEntry account)
{
    mEntry.data.type(ACCOUNT);
    mEntry.data.account() = std::move(account);
}

AccountFrame::AccountFrame(LedgerEntry entry) : EntryFrame{std::move(entry)}
{
    assert(mEntry.data.type() == ACCOUNT);
}

LedgerEntry
AccountFrame::makeAuthOnlyAccount(AccountID const& id)
{
    auto ret = LedgerEntry{};
    ret.data.type(ACCOUNT);
    ret.data.account().accountID = id;
    ret.data.account().thresholds[THRESHOLD_MASTER_WEIGHT] = 1;
    // puts a negative balance to trip any attempt to save this
    ret.data.account().balance = INT64_MIN;

    return ret;
}

AccountID const&
AccountFrame::getAccountID() const
{
    return mEntry.data.account().accountID;
}

void
AccountFrame::setAccountID(AccountID accountID)
{
    mEntry.data.account().accountID = std::move(accountID);
}

void
AccountFrame::setHomeDomain(string32 homeDomain)
{
    mEntry.data.account().homeDomain = std::move(homeDomain);
}

int64_t
AccountFrame::getBalance() const
{
    return mEntry.data.account().balance;
}

void
AccountFrame::setBalance(int64_t balance)
{
    mEntry.data.account().balance = balance;
}

int64_t
AccountFrame::getMinimumBalance(LedgerManager const& lm)
{
    return lm.getMinBalance(mEntry.data.account().numSubEntries);
}

bool
AccountFrame::addBalance(int64_t delta)
{
    return stellar::addBalance(mEntry.data.account().balance, delta);
}

int64_t
AccountFrame::getBalanceAboveReserve(LedgerManager const& lm)
{
    int64_t avail =
        getBalance() - lm.getMinBalance(mEntry.data.account().numSubEntries);
    // nothing can leave this account if below the reserve
    // (this can happen if the reserve is raised)
    return std::max(int64_t{0}, avail);
}

int
AccountFrame::getNumSubEntries() const
{
    return mEntry.data.account().numSubEntries;
}

xdr::xvector<Signer, 20> const&
AccountFrame::getSigners() const
{
    return mEntry.data.account().signers;
}

void AccountFrame::setSigners(xdr::xvector<Signer, 20> signers)
{
    mEntry.data.account().signers = signers;
    std::sort(std::begin(mEntry.data.account().signers),
              std::end(mEntry.data.account().signers), &signerCompare);
}

int
AccountFrame::getNumSigners() const
{
    return mEntry.data.account().signers.size();
}

bool
AccountFrame::addNumEntries(int count, LedgerManager const& lm)
{
    int newEntriesCount = mEntry.data.account().numSubEntries + count;
    if (newEntriesCount < 0)
    {
        throw std::runtime_error("invalid account state");
    }
    // only check minBalance when attempting to add subEntries
    if (count > 0 && getBalance() < lm.getMinBalance(newEntriesCount))
    {
        // balance too low
        return false;
    }
    mEntry.data.account().numSubEntries = newEntriesCount;
    return true;
}

bool
AccountFrame::isAuthRequired()
{
    return (mEntry.data.account().flags & AUTH_REQUIRED_FLAG) != 0;
}

bool
AccountFrame::isRevocableAuth()
{
    return (mEntry.data.account().flags & AUTH_REVOCABLE_FLAG) != 0;
}

bool
AccountFrame::isImmutableAuth()
{
    return (mEntry.data.account().flags & AUTH_IMMUTABLE_FLAG) != 0;
}

void
AccountFrame::setFlags(uint32_t flags)
{
    mEntry.data.account().flags |= flags;
}

void
AccountFrame::clearFlags(uint32_t flags)
{
    mEntry.data.account().flags &= ~flags;
}

xdr::pointer<AccountID> const&
AccountFrame::getInflationDest() const
{
    return mEntry.data.account().inflationDest;
}

void
AccountFrame::setInflationDest(AccountID inflationDest)
{
    mEntry.data.account().inflationDest.activate() = inflationDest;
}

uint32_t
AccountFrame::getMasterWeight() const
{
    return mEntry.data.account().thresholds[THRESHOLD_MASTER_WEIGHT];
}

void
AccountFrame::setMasterWeight(uint32_t masterWeight)
{
    mEntry.data.account().thresholds[THRESHOLD_MASTER_WEIGHT] = masterWeight;
}

uint32_t
AccountFrame::getHighThreshold() const
{
    return mEntry.data.account().thresholds[THRESHOLD_HIGH];
}

void
AccountFrame::setHighThreshold(uint32_t highThreshold)
{
    mEntry.data.account().thresholds[THRESHOLD_HIGH] = highThreshold;
}

uint32_t
AccountFrame::getMediumThreshold() const
{
    return mEntry.data.account().thresholds[THRESHOLD_MED];
}

void
AccountFrame::setMediumThreshold(uint32_t mediumThreshold)
{
    mEntry.data.account().thresholds[THRESHOLD_MED] = mediumThreshold;
}

uint32_t
AccountFrame::getLowThreshold() const
{
    return mEntry.data.account().thresholds[THRESHOLD_LOW];
}

void
AccountFrame::setLowThreshold(uint32_t lowThreshold)
{
    mEntry.data.account().thresholds[THRESHOLD_LOW] = lowThreshold;
}

void
AccountFrame::setSeqNum(SequenceNumber seq)
{
    mEntry.data.account().seqNum = seq;
}

SequenceNumber
AccountFrame::getSeqNum() const
{
    return mEntry.data.account().seqNum;
}

bool
AccountFrame::isValid()
{
    auto& a = mEntry.data.account();
    return isString32Valid(a.homeDomain) && a.balance >= 0 &&
           std::is_sorted(a.signers.begin(), a.signers.end(), &signerCompare);
}

bool
operator==(AccountFrame const& x, AccountFrame const& y)
{
    return x.mEntry == y.mEntry;
}

bool
operator!=(AccountFrame const& x, AccountFrame const& y)
{
    return !(x == y);
}
}
