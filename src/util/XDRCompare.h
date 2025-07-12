#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Logging.h"
#include "xdr/Stellar-contract.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-transaction.h"
#include <fmt/format.h>
#include <functional>
#include <string>
#include <vector>

namespace stellar
{

namespace xdrcomp
{

// Comparison context for structured output
struct ComparisonContext
{
    std::string name1;
    std::string name2;
    std::vector<std::string> pathStack;
    bool collectDifferences = false;
    std::vector<std::string> differences;

    ComparisonContext(std::string const& n1, std::string const& n2)
        : name1(n1), name2(n2)
    {
    }

    std::string getCurrentPath() const;
    void pushPath(std::string const& component);
    void popPath();
    void reportDifference(std::string const& message);
};

// Context-aware comparison functions
template <typename T>
void compareValueCtx(ComparisonContext& ctx, std::string const& fieldName,
                     T const& val1, T const& val2);

void compareLedgerEntryCtx(ComparisonContext& ctx, LedgerEntry const& entry1,
                           LedgerEntry const& entry2);

void compareTransactionMetaCtx(ComparisonContext& ctx,
                               TransactionMeta const& meta1,
                               TransactionMeta const& meta2, size_t txIndex);

void compareTransactionMeta(std::string const& name1, std::string const& name2,
                            TransactionMeta const& meta1,
                            TransactionMeta const& meta2, size_t txIndex);

// Generic comparison helpers
template <typename T>
void compareValue(std::string const& name1, std::string const& name2,
                  T const& val1, T const& val2, std::string const& fieldDesc);

template <typename T, typename CompareFunc>
void compareVector(std::string const& name1, std::string const& name2,
                   std::vector<T> const& vec1, std::vector<T> const& vec2,
                   std::string const& vectorDesc, CompareFunc compareElement);

template <typename T>
void compareOptional(std::string const& name1, std::string const& name2,
                     xdr::pointer<T> const& opt1, xdr::pointer<T> const& opt2,
                     std::string const& fieldDesc);

// XDR-specific comparison functions

// Contract and Soroban comparisons
void compareContractEvent(std::string const& name1, std::string const& name2,
                          ContractEvent const& event1,
                          ContractEvent const& event2,
                          std::string const& eventDesc);

void compareSCVal(std::string const& name1, std::string const& name2,
                  SCVal const& val1, SCVal const& val2,
                  std::string const& valDesc);

void compareSCMap(std::string const& name1, std::string const& name2,
                  SCMap const& map1, SCMap const& map2,
                  std::string const& mapDesc);

void compareSCVec(std::string const& name1, std::string const& name2,
                  SCVec const& vec1, SCVec const& vec2,
                  std::string const& vecDesc);

void compareSCContractInstance(std::string const& name1,
                               std::string const& name2,
                               SCContractInstance const& inst1,
                               SCContractInstance const& inst2,
                               std::string const& instDesc);

// Ledger entry comparisons
void compareLedgerEntry(std::string const& name1, std::string const& name2,
                        LedgerEntry const& entry1, LedgerEntry const& entry2,
                        std::string const& entryDesc);

void compareAccountEntry(std::string const& name1, std::string const& name2,
                         AccountEntry const& acc1, AccountEntry const& acc2,
                         std::string const& accDesc);

void compareTrustLineEntry(std::string const& name1, std::string const& name2,
                           TrustLineEntry const& tl1, TrustLineEntry const& tl2,
                           std::string const& tlDesc);

void compareOfferEntry(std::string const& name1, std::string const& name2,
                       OfferEntry const& offer1, OfferEntry const& offer2,
                       std::string const& offerDesc);

void compareDataEntry(std::string const& name1, std::string const& name2,
                      DataEntry const& data1, DataEntry const& data2,
                      std::string const& dataDesc);

void compareClaimableBalanceEntry(std::string const& name1,
                                  std::string const& name2,
                                  ClaimableBalanceEntry const& cb1,
                                  ClaimableBalanceEntry const& cb2,
                                  std::string const& cbDesc);

void compareLiquidityPoolEntry(std::string const& name1,
                               std::string const& name2,
                               LiquidityPoolEntry const& lp1,
                               LiquidityPoolEntry const& lp2,
                               std::string const& lpDesc);

void compareConfigSettingEntry(std::string const& name1,
                               std::string const& name2,
                               ConfigSettingEntry const& cs1,
                               ConfigSettingEntry const& cs2,
                               std::string const& csDesc);

void compareContractDataEntry(std::string const& name1,
                              std::string const& name2,
                              ContractDataEntry const& cd1,
                              ContractDataEntry const& cd2,
                              std::string const& cdDesc);

void compareContractCodeEntry(std::string const& name1,
                              std::string const& name2,
                              ContractCodeEntry const& cc1,
                              ContractCodeEntry const& cc2,
                              std::string const& ccDesc);

void compareTTLEntry(std::string const& name1, std::string const& name2,
                     TTLEntry const& ttl1, TTLEntry const& ttl2,
                     std::string const& ttlDesc);

// Ledger entry changes
void compareLedgerEntryChanges(std::string const& name1,
                               std::string const& name2,
                               LedgerEntryChanges const& changes1,
                               LedgerEntryChanges const& changes2,
                               std::string const& changesDesc);

void compareLedgerEntryChange(std::string const& name1,
                              std::string const& name2,
                              LedgerEntryChange const& change1,
                              LedgerEntryChange const& change2,
                              std::string const& changeDesc);

// Asset comparisons
void compareAsset(std::string const& name1, std::string const& name2,
                  Asset const& asset1, Asset const& asset2,
                  std::string const& assetDesc);

void compareTrustLineAsset(std::string const& name1, std::string const& name2,
                           TrustLineAsset const& asset1,
                           TrustLineAsset const& asset2,
                           std::string const& assetDesc);

// Implementation of templates (must be in header)
template <typename T>
void
compareValue(std::string const& name1, std::string const& name2, T const& val1,
             T const& val2, std::string const& fieldDesc)
{
    if (val1 != val2)
    {
        CLOG_ERROR(Ledger, "{} differs: {} {} vs {} {}", fieldDesc, name1, val1,
                   name2, val2);
    }
}

template <typename T, typename CompareFunc>
void
compareVector(std::string const& name1, std::string const& name2,
              std::vector<T> const& vec1, std::vector<T> const& vec2,
              std::string const& vectorDesc, CompareFunc compareElement)
{
    if (vec1.size() != vec2.size())
    {
        CLOG_ERROR(Ledger, "{} count differs: {} {} vs {} {}", vectorDesc,
                   name1, vec1.size(), name2, vec2.size());
    }
    else
    {
        for (size_t i = 0; i < vec1.size(); ++i)
        {
            compareElement(vec1[i], vec2[i], i);
        }
    }
}

template <typename T>
void
compareOptional(std::string const& name1, std::string const& name2,
                xdr::pointer<T> const& opt1, xdr::pointer<T> const& opt2,
                std::string const& fieldDesc)
{
    bool has1 = opt1.get() != nullptr;
    bool has2 = opt2.get() != nullptr;

    if (has1 != has2)
    {
        CLOG_ERROR(Ledger, "{} presence differs: {} {} vs {} {}", fieldDesc,
                   name1, has1, name2, has2);
    }
    else if (has1 && !(*(opt1) == *(opt2)))
    {
        CLOG_ERROR(Ledger, "{} differs", fieldDesc);
    }
}

// Template implementation for context-aware comparison
template <typename T>
void
compareValueCtx(ComparisonContext& ctx, std::string const& fieldName,
                T const& val1, T const& val2)
{
    if (val1 != val2)
    {
        ctx.pushPath(fieldName);
        ctx.reportDifference(
            fmt::format("value differs: {} vs {}", val1, val2));
        ctx.popPath();
    }
}

} // namespace xdrcomp
} // namespace stellar
