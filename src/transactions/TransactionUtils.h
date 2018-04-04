#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>

namespace stellar
{
class AccountReference;
class ExplicitTrustLineReference;
class LedgerEntryReference;
class LedgerHeaderReference;
class LedgerState;
class LedgerStateRoot;
class TrustLineReference;

uint32_t
getCurrentTxFee(LedgerStateRoot& lsr);
uint32_t
getCurrentTxFee(std::shared_ptr<LedgerHeaderReference const> header);

uint32_t
getCurrentMaxTxSetSize(LedgerStateRoot& lsr);
uint32_t
getCurrentMaxTxSetSize(std::shared_ptr<LedgerHeaderReference const> header);

uint32_t
getCurrentLedgerNum(LedgerStateRoot& lsr);
uint32_t
getCurrentLedgerNum(std::shared_ptr<LedgerHeaderReference const> header);

uint64_t
getCurrentCloseTime(LedgerStateRoot& lsr);
uint64_t
getCurrentCloseTime(std::shared_ptr<LedgerHeaderReference const> header);

int64_t
getCurrentMinBalance(LedgerStateRoot& lsr, uint32_t ownerCount);
int64_t
getCurrentMinBalance(std::shared_ptr<LedgerHeaderReference const> header, uint32_t ownerCount);

uint32_t
getCurrentLedgerVersion(LedgerStateRoot& lsr);
uint32_t
getCurrentLedgerVersion(std::shared_ptr<LedgerHeaderReference const> header);

uint64_t
getStartingSequenceNumber(LedgerStateRoot& lsr);
uint64_t
getStartingSequenceNumber(std::shared_ptr<LedgerHeaderReference const> header);

AccountReference
loadAccount(LedgerState& ls, AccountID const& accountID);

std::shared_ptr<LedgerEntryReference>
loadAccountRaw(LedgerState& ls, AccountID const& accountID);

std::shared_ptr<ExplicitTrustLineReference>
loadExplicitTrustLine(LedgerState& ls, AccountID const& accountID, Asset const& asset);

std::shared_ptr<TrustLineReference>
loadTrustLine(LedgerState& ls, AccountID const& accountID, Asset const& asset);

std::shared_ptr<LedgerEntryReference>
loadOffer(LedgerState& ls, AccountID const& accountID, uint64_t offerID);

std::shared_ptr<LedgerEntryReference>
loadData(LedgerState& ls, AccountID const& accountID, std::string const& dataName);
}
