#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifdef BUILD_TESTS
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

// Enum for difference types that can be tolerated
// Use STELLAR_COMPARISON_TOLERANCE environment variable with comma-separated
// options:
// - balance, sequence_number, last_modified_ledger, num_sub_entries
// - liabilities, sponsoring_id, signer_sponsoring_id
// - seq_ledger, seq_time
// - soroban_fees, soroban_return_value, soroban_events,
// soroban_diagnostic_events
// - contract_events, transaction_result_code, operation_result_code
// - fee_charged, tx_changes_before, tx_changes_after
// - fees (alias for fee_charged,tx_changes_before,tx_changes_after)
// Example: export STELLAR_COMPARISON_TOLERANCE="fees,balance"
enum DifferenceType : uint64_t
{
    DIFF_NONE = 0,
    DIFF_BALANCE =
        1ULL << 0, // Note: When DIFF_FEE_CHARGED is set, balance
                   // differences in txChangesBefore/After are also tolerated
    DIFF_SEQUENCE_NUMBER = 1ULL << 1,
    DIFF_LAST_MODIFIED_LEDGER = 1ULL << 2,
    DIFF_NUM_SUB_ENTRIES = 1ULL << 3,
    DIFF_LIABILITIES = 1ULL << 4,
    DIFF_SPONSORING_ID = 1ULL << 5,
    DIFF_SEQ_LEDGER = 1ULL << 6,
    DIFF_SEQ_TIME = 1ULL << 7,
    DIFF_SOROBAN_FEES = 1ULL << 8,
    DIFF_SOROBAN_RETURN_VALUE = 1ULL << 9,
    DIFF_EVENTS = 1ULL << 10,
    DIFF_EVENT_TOPICS = 1ULL << 11,
    DIFF_DIAGNOSTIC_EVENTS = 1ULL << 12,
    DIFF_TRANSACTION_RESULT_CODE = 1ULL << 13,
    DIFF_OPERATION_RESULT_CODE = 1ULL << 14,
    DIFF_FEE_CHARGED = 1ULL << 15,
    DIFF_TX_CHANGES_BEFORE = 1ULL << 16,
    DIFF_TX_CHANGES_AFTER = 1ULL << 17,
    // Add more difference types as needed up to bit 63
};

// Comparator for structured output
class Comparator
{
    std::string mName1;
    std::string mName2;
    std::vector<std::string> mPathStack;
    std::vector<std::string> mDifferences;
    uint64_t mToleratedDifferences;

    std::string getCurrentPath() const;
    void pushPath(std::string const& component);
    void popPath();
    void reportDifference(std::string const& message);
    bool
    isDifferenceTolerated(DifferenceType type) const
    {
        return (mToleratedDifferences & type) != 0;
    }

    bool
    isInToleratedSection() const
    {
        // Check if we're in txChangesBefore or txChangesAfter sections
        for (auto const& path : mPathStack)
        {
            if (path == "txChangesBefore" &&
                isDifferenceTolerated(DIFF_TX_CHANGES_BEFORE))
                return true;
            if (path == "txChangesAfter" &&
                isDifferenceTolerated(DIFF_TX_CHANGES_AFTER))
                return true;
            if ((path == "event" || path == "events") &&
                isDifferenceTolerated(DIFF_EVENTS))
                return true;
            if (path == "topics" && isDifferenceTolerated(DIFF_EVENT_TOPICS))
                return true;
            if (path == "diagnosticEvents" &&
                isDifferenceTolerated(DIFF_DIAGNOSTIC_EVENTS))
                return true;
            if (path == "returnValue" &&
                isDifferenceTolerated(DIFF_SOROBAN_RETURN_VALUE))
                return true;
        }
        return false;
    }

    bool
    isAccountBalanceInBeforeAfterSection() const
    {
        // Check if we're comparing an account balance within txChangesBefore or
        // txChangesAfter
        bool inBeforeAfter = false;
        bool inAccount = false;
        bool inBalance = false;

        for (auto const& path : mPathStack)
        {
            if (path == "txChangesBefore" || path == "txChangesAfter")
                inBeforeAfter = true;
            if (path == "account")
                inAccount = true;
            if (path == "balance")
                inBalance = true;
        }

        return inBeforeAfter && inAccount && inBalance;
    }
    void reportDifference(std::string const& message, DifferenceType type);

    // Get tolerated differences from environment variable
    static uint64_t getToleratedDifferences();

  public:
    Comparator(std::string const& n1, std::string const& n2)
        : mName1(n1)
        , mName2(n2)
        , mToleratedDifferences(getToleratedDifferences())
    {
    }

    std::vector<std::string> const&
    getDifferences()
    {
        return mDifferences;
    }

    // Comparison methods
    void compareContractEvent(ContractEvent const& event1,
                              ContractEvent const& event2);
    void compareSCVal(SCVal const& val1, SCVal const& val2);
    void compareSCMap(SCMap const& map1, SCMap const& map2);
    void compareSCVec(SCVec const& vec1, SCVec const& vec2);
    void compareSCContractInstance(SCContractInstance const& inst1,
                                   SCContractInstance const& inst2);
    void compareLedgerEntry(LedgerEntry const& entry1,
                            LedgerEntry const& entry2);
    void compareAccountEntry(AccountEntry const& acc1,
                             AccountEntry const& acc2);
    void compareTrustLineEntry(TrustLineEntry const& tl1,
                               TrustLineEntry const& tl2);
    void compareLedgerEntryChanges(LedgerEntryChanges const& changes1,
                                   LedgerEntryChanges const& changes2);
    void compareLedgerEntryChange(LedgerEntryChange const& change1,
                                  LedgerEntryChange const& change2);
    void compareTrustLineAsset(TrustLineAsset const& asset1,
                               TrustLineAsset const& asset2);
    void compareAsset(Asset const& asset1, Asset const& asset2);
    void compareOfferEntry(OfferEntry const& offer1, OfferEntry const& offer2);
    void compareDataEntry(DataEntry const& data1, DataEntry const& data2);
    void compareClaimableBalanceEntry(ClaimableBalanceEntry const& cb1,
                                      ClaimableBalanceEntry const& cb2);
    void compareLiquidityPoolEntry(LiquidityPoolEntry const& lp1,
                                   LiquidityPoolEntry const& lp2);
    void compareConfigSettingEntry(ConfigSettingEntry const& cs1,
                                   ConfigSettingEntry const& cs2);
    void compareContractDataEntry(ContractDataEntry const& cd1,
                                  ContractDataEntry const& cd2);
    void compareContractCodeEntry(ContractCodeEntry const& cc1,
                                  ContractCodeEntry const& cc2);
    void compareTTLEntry(TTLEntry const& ttl1, TTLEntry const& ttl2);
    void compareTransactionMeta(TransactionMeta const& meta1,
                                TransactionMeta const& meta2, size_t txIndex);
    void compareTransactionResult(TransactionResult const& result1,
                                  TransactionResult const& result2);
    void compareOperationResult(OperationResult const& result1,
                                OperationResult const& result2);

    // Template comparison methods
    template <typename T>
    void
    compareValue(std::string const& fieldName, T const& val1, T const& val2)
    {
        if (val1 != val2)
        {
            pushPath(fieldName);
            reportDifference(
                fmt::format("value differs: {} vs {}", val1, val2));
            popPath();
        }
    }

    template <typename T>
    void
    compareValue(std::string const& fieldName, T const& val1, T const& val2,
                 DifferenceType diffType)
    {
        if (val1 != val2)
        {
            pushPath(fieldName);
            // Special case: account balance differences in before/after
            // sections when fee tolerance is enabled
            if (fieldName == "balance" && diffType == DIFF_BALANCE &&
                isAccountBalanceInBeforeAfterSection() &&
                isDifferenceTolerated(DIFF_FEE_CHARGED))
            {
                // This is likely a fee-related balance change, don't report it
            }
            else
            {
                reportDifference(
                    fmt::format("value differs: {} vs {}", val1, val2),
                    diffType);
            }
            popPath();
        }
    }

    template <typename T, typename CompareFunc>
    void
    compareVector(std::string const& fieldName, std::vector<T> const& vec1,
                  std::vector<T> const& vec2, CompareFunc compareElement)
    {
        pushPath(fieldName);
        if (vec1.size() != vec2.size())
        {
            reportDifference(fmt::format("count differs: {} vs {}", vec1.size(),
                                         vec2.size()));
        }
        else
        {
            for (size_t i = 0; i < vec1.size(); ++i)
            {
                pushPath(fmt::format("[{}]", i));
                compareElement(vec1[i], vec2[i]);
                popPath();
            }
        }
        popPath();
    }

    template <typename T>
    void
    compareOptional(std::string const& fieldName, xdr::pointer<T> const& opt1,
                    xdr::pointer<T> const& opt2)
    {
        pushPath(fieldName);
        bool has1 = opt1.get() != nullptr;
        bool has2 = opt2.get() != nullptr;

        if (has1 != has2)
        {
            reportDifference(
                fmt::format("presence differs: {} vs {}", has1, has2));
        }
        else if (has1 && !(*(opt1) == *(opt2)))
        {
            reportDifference("value differs");
        }
        popPath();
    }
};

} // namespace xdrcomp
} // namespace stellar

#endif // BUILD_TESTS