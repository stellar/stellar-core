// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerEntryReference.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "transactions/TransactionUtils.h"
#include "util/types.h"

#include "ledger/AccountReference.h"
#include "ledger/TrustLineReference.h"

namespace stellar
{
using xdr::operator==;

uint32_t
getCurrentTxFee(LedgerStateRoot& lsr)
{
    LedgerState ls(lsr);
    return getCurrentTxFee(ls.loadHeader());
}

uint32_t
getCurrentTxFee(std::shared_ptr<LedgerHeaderReference const> header)
{
    return header->header().baseFee;
}

uint32_t
getCurrentMaxTxSetSize(LedgerStateRoot& lsr)
{
    LedgerState ls(lsr);
    return getCurrentMaxTxSetSize(ls.loadHeader());
}

uint32_t
getCurrentMaxTxSetSize(std::shared_ptr<LedgerHeaderReference const> header)
{
    return header->header().maxTxSetSize;
}

uint32_t
getCurrentLedgerNum(LedgerStateRoot& lsr)
{
    LedgerState ls(lsr);
    return getCurrentLedgerNum(ls.loadHeader());
}

uint32_t
getCurrentLedgerNum(std::shared_ptr<LedgerHeaderReference const> header)
{
    return header->header().ledgerSeq;
}

uint64_t
getCurrentCloseTime(LedgerStateRoot& lsr)
{
    LedgerState ls(lsr);
    return getCurrentCloseTime(ls.loadHeader());
}

uint64_t
getCurrentCloseTime(std::shared_ptr<LedgerHeaderReference const> header)
{
    return header->header().scpValue.closeTime;
}

int64_t
getCurrentMinBalance(LedgerStateRoot& lsr, uint32_t ownerCount)
{
    LedgerState ls(lsr);
    return getCurrentMinBalance(ls.loadHeader(), ownerCount);
}

int64_t
getCurrentMinBalance(std::shared_ptr<LedgerHeaderReference const> header, uint32_t ownerCount)
{
    if (header->header().ledgerVersion <= 8)
    {
        return (2 + ownerCount) * header->header().baseReserve;
    }
    else
    {
        return (2 + ownerCount) * int64_t(header->header().baseReserve);
    }
}

uint32_t
getCurrentLedgerVersion(LedgerStateRoot& lsr)
{
    LedgerState ls(lsr);
    return getCurrentLedgerVersion(ls.loadHeader());
}

uint32_t
getCurrentLedgerVersion(std::shared_ptr<LedgerHeaderReference const> header)
{
    return header->header().ledgerVersion;
}

uint64_t
getStartingSequenceNumber(LedgerStateRoot& lsr)
{
    LedgerState ls(lsr);
    return getStartingSequenceNumber(ls.loadHeader());
}

uint64_t
getStartingSequenceNumber(std::shared_ptr<LedgerHeaderReference const> header)
{
    return static_cast<uint64_t>(header->header().ledgerSeq) << 32;
}

AccountReference
loadAccount(LedgerState& ls, AccountID const& accountID)
{
    return loadAccountRaw(ls, accountID);
}

std::shared_ptr<LedgerEntryReference>
loadAccountRaw(LedgerState& ls, AccountID const& accountID)
{
    LedgerKey key(ACCOUNT);
    key.account().accountID = accountID;
    try
    {
        return ls.load(key);
    }
    catch (std::runtime_error& e)
    {
        return nullptr;
    }
}

std::shared_ptr<ExplicitTrustLineReference>
loadExplicitTrustLine(LedgerState& ls, AccountID const& accountID, Asset const& asset)
{
    LedgerKey key(TRUSTLINE);
    key.trustLine().accountID = accountID;
    key.trustLine().asset = asset;
    try
    {
        return std::make_shared<ExplicitTrustLineReference>(ls.load(key));
    }
    catch (std::runtime_error& e)
    {
        return nullptr;
    }
}

std::shared_ptr<TrustLineReference>
loadTrustLine(LedgerState& ls, AccountID const& accountID, Asset const& asset)
{
    if (accountID == getIssuer(asset))
    {
        return std::make_shared<IssuerTrustLineReference>();
    }
    else
    {
        return loadExplicitTrustLine(ls, accountID, asset);
    }
}

std::shared_ptr<LedgerEntryReference>
loadOffer(LedgerState& ls, AccountID const& accountID, uint64_t offerID)
{
    LedgerKey key(OFFER);
    key.offer().sellerID = accountID;
    key.offer().offerID = offerID;
    try
    {
        return ls.load(key);
    }
    catch (std::runtime_error& e)
    {
        return {nullptr};
    }
}

std::shared_ptr<LedgerEntryReference>
loadData(LedgerState& ls, AccountID const& accountID, std::string const& dataName)
{
    LedgerKey key(DATA);
    key.data().accountID = accountID;
    key.data().dataName = dataName;
    try
    {
        return ls.load(key);
    }
    catch (std::runtime_error& e)
    {
        return {nullptr};
    }
}
}
