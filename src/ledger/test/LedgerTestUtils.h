#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryManager.h"
#include "overlay/StellarXDR.h"
#include "util/UnorderedSet.h"
#include "util/types.h"

namespace stellar
{
namespace LedgerTestUtils
{

// note: entries generated are valid in the sense that they are sane by
// themselves
// it does NOT mean that it makes sense relative to other entries:
// for example the numsubentries of a related account is not updated when
// generating a 'valid' trust line

template <typename T> void replaceControlCharacters(T& s, int minSize);

void randomlyModifyEntry(LedgerEntry& e);

void makeValid(AccountEntry& a);
void makeValid(TrustLineEntry& tl);
void makeValid(OfferEntry& o);
void makeValid(DataEntry& d);
void makeValid(ClaimableBalanceEntry& c);
void makeValid(LiquidityPoolEntry& lp);
void makeValid(ConfigSettingEntry& ce);
void makeValid(ContractDataEntry& cde);
void makeValid(ContractCodeEntry& cce);
void makeValid(TTLEntry& ee);
void makeValid(LedgerHeaderHistoryEntry& lh,
               LedgerHeaderHistoryEntry firstLedger,
               HistoryManager::LedgerVerificationStatus state);

LedgerEntry generateValidLedgerEntry(size_t b = 3);
LedgerEntry generateValidLedgerEntryOfType(LedgerEntryType type);

std::vector<LedgerEntry> generateValidLedgerEntries(size_t n);
std::vector<LedgerEntry> generateValidUniqueLedgerEntries(size_t n);

std::vector<LedgerKey> generateValidLedgerEntryKeysWithExclusions(
    std::unordered_set<LedgerEntryType> const& excludedTypes, size_t n);

std::vector<LedgerKey> generateValidUniqueLedgerKeysWithTypes(
    std::unordered_set<LedgerEntryType> const& types, size_t n,
    UnorderedSet<LedgerKey>& seenKeys);

std::vector<LedgerKey> generateUniqueValidSorobanLedgerEntryKeys(size_t n);

std::vector<LedgerKey> generateValidUniqueLedgerEntryKeysWithExclusions(
    std::unordered_set<LedgerEntryType> const& excludedTypes, size_t n);

std::vector<LedgerEntry> generateValidUniqueLedgerEntriesWithExclusions(
    std::unordered_set<LedgerEntryType> const& excludedTypes, size_t n);

LedgerEntry generateValidLedgerEntryWithExclusions(
    std::unordered_set<LedgerEntryType> const& excludedTypes, size_t b = 3);
std::vector<LedgerEntry> generateValidLedgerEntriesWithExclusions(
    std::unordered_set<LedgerEntryType> const& excludedTypes, size_t n);

LedgerEntry generateValidLedgerEntryWithTypes(
    std::unordered_set<LedgerEntryType> const& types, size_t b = 3);
std::vector<LedgerEntry> generateValidUniqueLedgerEntriesWithTypes(
    std::unordered_set<LedgerEntryType> const& types, size_t n);

AccountEntry generateValidAccountEntry(size_t b = 3);
std::vector<AccountEntry> generateValidAccountEntries(size_t n);

TrustLineEntry generateNonPoolShareValidTrustLineEntry(size_t b = 3);
TrustLineEntry generateValidTrustLineEntry(size_t b = 3);
std::vector<TrustLineEntry> generateValidTrustLineEntries(size_t n);

OfferEntry generateValidOfferEntry(size_t b = 3);
std::vector<OfferEntry> generateValidOfferEntries(size_t n);

DataEntry generateValidDataEntry(size_t b = 3);
std::vector<DataEntry> generateValidDataEntries(size_t n);

ClaimableBalanceEntry generateValidClaimableBalanceEntry(size_t b = 3);
std::vector<ClaimableBalanceEntry>
generateValidClaimableBalanceEntries(size_t n);

LiquidityPoolEntry generateValidLiquidityPoolEntry(size_t b = 3);
std::vector<LiquidityPoolEntry> generateValidLiquidityPoolEntries(size_t n);

ConfigSettingEntry generateValidConfigSettingEntry(size_t b = 3);
std::vector<ConfigSettingEntry> generateValidConfigSettingEntries(size_t n);

ContractDataEntry generateValidContractDataEntry(size_t b = 3);
std::vector<ContractDataEntry> generateValidContractDataEntries(size_t n);

ContractCodeEntry generateValidContractCodeEntry(size_t b = 3);
std::vector<ContractCodeEntry> generateValidContractCodeEntries(size_t n);

TTLEntry generateValidTTLEntry(size_t b = 3);
std::vector<TTLEntry> generateValidTTLEntries(size_t n);

std::vector<LedgerHeaderHistoryEntry> generateLedgerHeadersForCheckpoint(
    LedgerHeaderHistoryEntry firstLedger, uint32_t size,
    HistoryManager::LedgerVerificationStatus state =
        HistoryManager::VERIFY_STATUS_OK);

UpgradeType toUpgradeType(LedgerUpgrade const& upgrade);
}
}
