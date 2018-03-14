// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/AccountReference.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerState.h"
#include "transactions/TransactionUtils.h"
#include "util/types.h"

namespace stellar
{
using xdr::operator<;

AccountReference::AccountReference(
    std::shared_ptr<LedgerEntryReference> const& ler)
    : mEntry(ler)
{
}

AccountEntry&
AccountReference::account()
{
    return mEntry->entry()->data.account();
}

AccountEntry const&
AccountReference::account() const
{
    return mEntry->entry()->data.account();
}

AccountID const&
AccountReference::getID() const
{
    return account().accountID;
}

int64_t
AccountReference::getBalance() const
{
    return account().balance;
}

bool
AccountReference::addBalance(int64_t delta)
{
    return stellar::addBalance(account().balance, delta);
}

int64_t
AccountReference::getMinimumBalance(std::shared_ptr<LedgerHeaderReference> header) const
{
    return getCurrentMinBalance(header, account().numSubEntries);
}

int64_t
AccountReference::getBalanceAboveReserve(std::shared_ptr<LedgerHeaderReference> header) const
{
    int64_t avail = getBalance() - getMinimumBalance(header);
    if (avail < 0)
    {
        // nothing can leave this account if below the reserve
        // (this can happen if the reserve is raised)
        avail = 0;
    }
    return avail;
}

bool
AccountReference::addNumEntries(std::shared_ptr<LedgerHeaderReference> header, int count)
{
    int newEntriesCount = account().numSubEntries + count;
    if (newEntriesCount < 0)
    {
        throw std::runtime_error("invalid account state");
    }
    // only check minBalance when attempting to add subEntries
    if (count > 0 && getBalance() < getCurrentMinBalance(header, newEntriesCount))
    {
        // balance too low
        return false;
    }
    account().numSubEntries = newEntriesCount;
    return true;
}

bool
AccountReference::isAuthRequired() const
{
    return (account().flags & AUTH_REQUIRED_FLAG) != 0;
}

bool
AccountReference::isImmutableAuth() const
{
    return (account().flags & AUTH_IMMUTABLE_FLAG) != 0;
}

uint32_t
AccountReference::getMasterWeight() const
{
    return account().thresholds[THRESHOLD_MASTER_WEIGHT];
}

uint32_t
AccountReference::getHighThreshold() const
{
    return account().thresholds[THRESHOLD_HIGH];
}

uint32_t
AccountReference::getMediumThreshold() const
{
    return account().thresholds[THRESHOLD_MED];
}

uint32_t
AccountReference::getLowThreshold() const
{
    return account().thresholds[THRESHOLD_LOW];
}

void
AccountReference::normalizeSigners()
{
    std::sort(account().signers.begin(), account().signers.end(),
              &AccountReference::signerCompare);
}

void
AccountReference::setSeqNum(SequenceNumber seq)
{
    account().seqNum = seq;
}

SequenceNumber
AccountReference::getSeqNum() const
{
    return account().seqNum;
}

bool
AccountReference::signerCompare(Signer const& s1, Signer const& s2)
{
    return s1.key < s2.key;
}

void
AccountReference::invalidate()
{
    mEntry->invalidate();
}

void
AccountReference::forget(LedgerState& ls)
{
    ls.forget(mEntry);
}

void
AccountReference::erase()
{
    mEntry->erase();
}
};
