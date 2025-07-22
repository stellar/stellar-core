// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifdef BUILD_TESTS

#include "util/XDRCompare.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/MetaUtils.h"
#include "util/XDROperators.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-types.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fmt/format.h>
#include <sstream>

namespace stellar
{
namespace xdrcomp
{

// ComparisonContext implementation
std::string
Comparator::getCurrentPath() const
{
    std::string path;
    for (auto const& component : mPathStack)
    {
        if (!path.empty())
            path += ".";
        path += component;
    }
    return path;
}

void
Comparator::pushPath(std::string const& component)
{
    mPathStack.push_back(component);
}

void
Comparator::popPath()
{
    if (!mPathStack.empty())
        mPathStack.pop_back();
}

void
Comparator::reportDifference(std::string const& message)
{
    // Check if we're in a tolerated section
    if (isInToleratedSection())
    {
        // This entire section is tolerated, so we don't report the difference
        return;
    }

    std::string fullMessage = getCurrentPath() + ": " + message;
    mDifferences.push_back(fullMessage);
    CLOG_ERROR(Ledger, "{} vs {}: {}", mName1, mName2, fullMessage);
}

void
Comparator::reportDifference(std::string const& message, DifferenceType type)
{
    if (isDifferenceTolerated(type))
    {
        // This difference type is tolerated, so we don't report it
        return;
    }
    reportDifference(message);
}

uint64_t
Comparator::getToleratedDifferences()
{
    uint64_t toleratedDiffs = 0;
    char* env = std::getenv("STELLAR_COMPARISON_TOLERANCE");
    if (env)
    {
        std::string envStr(env);
        std::istringstream stream(envStr);
        std::string option;

        while (std::getline(stream, option, ','))
        {
            // Trim whitespace and convert to lowercase
            option.erase(0, option.find_first_not_of(" \t"));
            option.erase(option.find_last_not_of(" \t") + 1);
            std::transform(option.begin(), option.end(), option.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            // Map option names to difference types
            if (option == "balance")
                toleratedDiffs |= DIFF_BALANCE;
            else if (option == "sequence_number")
                toleratedDiffs |= DIFF_SEQUENCE_NUMBER;
            else if (option == "last_modified_ledger")
                toleratedDiffs |= DIFF_LAST_MODIFIED_LEDGER;
            else if (option == "num_sub_entries")
                toleratedDiffs |= DIFF_NUM_SUB_ENTRIES;
            else if (option == "liabilities")
                toleratedDiffs |= DIFF_LIABILITIES;
            else if (option == "sponsoring_id")
                toleratedDiffs |= DIFF_SPONSORING_ID;
            else if (option == "seq_ledger")
                toleratedDiffs |= DIFF_SEQ_LEDGER;
            else if (option == "seq_time")
                toleratedDiffs |= DIFF_SEQ_TIME;
            else if (option == "soroban_fees")
                toleratedDiffs |= DIFF_SOROBAN_FEES;
            else if (option == "soroban_return_value")
                toleratedDiffs |= DIFF_SOROBAN_RETURN_VALUE;
            else if (option == "events")
                toleratedDiffs |= DIFF_EVENTS;
            else if (option == "event_topics")
                toleratedDiffs |= DIFF_EVENT_TOPICS;
            else if (option == "diagnostic_events")
                toleratedDiffs |= DIFF_DIAGNOSTIC_EVENTS;
            else if (option == "transaction_result_code")
                toleratedDiffs |= DIFF_TRANSACTION_RESULT_CODE;
            else if (option == "operation_result_code")
                toleratedDiffs |= DIFF_OPERATION_RESULT_CODE;
            else if (option == "fee_charged")
                toleratedDiffs |= DIFF_FEE_CHARGED;
            else if (option == "tx_changes_before")
                toleratedDiffs |= DIFF_TX_CHANGES_BEFORE;
            else if (option == "tx_changes_after")
                toleratedDiffs |= DIFF_TX_CHANGES_AFTER;
            else if (option == "fee_changes" || option == "fees")
            {
                // Convenience option to tolerate all fee-related changes
                // This includes fee_charged and balance changes in before/after
                // sections
                toleratedDiffs |= DIFF_FEE_CHARGED | DIFF_TX_CHANGES_BEFORE |
                                  DIFF_TX_CHANGES_AFTER;
            }
            else if (!option.empty())
            {
                CLOG_WARNING(Ledger, "Unknown tolerance option: '{}'", option);
            }
        }
    }
    return toleratedDiffs;
}

void
Comparator::compareContractEvent(ContractEvent const& event1,
                                 ContractEvent const& event2)
{
    compareValue("type", static_cast<int>(event1.type),
                 static_cast<int>(event2.type));

    compareOptional("contractID", event1.contractID, event2.contractID);

    pushPath("body");
    if (event1.body.v() != event2.body.v())
    {
        reportDifference(fmt::format("version differs: {} vs {}",
                                     event1.body.v(), event2.body.v()));
    }
    else if (event1.body.v() == 0)
    {
        auto const& v0_1 = event1.body.v0();
        auto const& v0_2 = event2.body.v0();

        compareVector("topics", v0_1.topics, v0_2.topics,
                      [&](SCVal const& topic1, SCVal const& topic2) {
                          compareSCVal(topic1, topic2);
                      });

        pushPath("data");
        compareSCVal(v0_1.data, v0_2.data);
        popPath();
    }
    popPath();
}

void
Comparator::compareSCVal(SCVal const& val1, SCVal const& val2)
{
    if (val1.type() != val2.type())
    {
        reportDifference(fmt::format("type differs: {} vs {}",
                                     static_cast<int>(val1.type()),
                                     static_cast<int>(val2.type())));
        return;
    }

    switch (val1.type())
    {
    case SCV_BOOL:
        compareValue("b", val1.b(), val2.b());
        break;

    case SCV_VOID:
        // Nothing to compare
        break;

    case SCV_ERROR:
        pushPath("error");
        if (val1.error().type() != val2.error().type())
        {
            reportDifference(fmt::format(
                "type differs: {} vs {}", static_cast<int>(val1.error().type()),
                static_cast<int>(val2.error().type())));
        }
        else if (val1.error().type() == SCE_CONTRACT)
        {
            compareValue("contractCode", val1.error().contractCode(),
                         val2.error().contractCode());
        }
        popPath();
        break;

    case SCV_U32:
        compareValue("u32", val1.u32(), val2.u32());
        break;

    case SCV_I32:
        compareValue("i32", val1.i32(), val2.i32());
        break;

    case SCV_U64:
        compareValue("u64", val1.u64(), val2.u64());
        break;

    case SCV_I64:
        compareValue("i64", val1.i64(), val2.i64());
        break;

    case SCV_TIMEPOINT:
        compareValue("timepoint", val1.timepoint(), val2.timepoint());
        break;

    case SCV_DURATION:
        compareValue("duration", val1.duration(), val2.duration());
        break;

    case SCV_U128:
        pushPath("u128");
        compareValue("lo", val1.u128().lo, val2.u128().lo);
        compareValue("hi", val1.u128().hi, val2.u128().hi);
        popPath();
        break;

    case SCV_I128:
        pushPath("i128");
        compareValue("lo", val1.i128().lo, val2.i128().lo);
        compareValue("hi", val1.i128().hi, val2.i128().hi);
        popPath();
        break;

    case SCV_U256:
        pushPath("u256");
        compareValue("lo_lo", val1.u256().lo_lo, val2.u256().lo_lo);
        compareValue("lo_hi", val1.u256().lo_hi, val2.u256().lo_hi);
        compareValue("hi_lo", val1.u256().hi_lo, val2.u256().hi_lo);
        compareValue("hi_hi", val1.u256().hi_hi, val2.u256().hi_hi);
        popPath();
        break;

    case SCV_I256:
        pushPath("i256");
        compareValue("lo_lo", val1.i256().lo_lo, val2.i256().lo_lo);
        compareValue("lo_hi", val1.i256().lo_hi, val2.i256().lo_hi);
        compareValue("hi_lo", val1.i256().hi_lo, val2.i256().hi_lo);
        compareValue("hi_hi", val1.i256().hi_hi, val2.i256().hi_hi);
        popPath();
        break;

    case SCV_BYTES:
        if (val1.bytes() != val2.bytes())
        {
            pushPath("bytes");
            reportDifference(fmt::format("size differs: {} vs {}",
                                         val1.bytes().size(),
                                         val2.bytes().size()));
            popPath();
        }
        break;

    case SCV_STRING:
        if (val1.str() != val2.str())
        {
            pushPath("str");
            reportDifference(fmt::format("value differs: '{}' vs '{}'",
                                         val1.str(), val2.str()));
            popPath();
        }
        break;

    case SCV_SYMBOL:
        if (val1.sym() != val2.sym())
        {
            pushPath("sym");
            reportDifference(fmt::format("value differs: '{}' vs '{}'",
                                         val1.sym(), val2.sym()));
            popPath();
        }
        break;

    case SCV_VEC:
        if (val1.vec())
        {
            pushPath("vec");
            compareSCVec(*val1.vec(), *val2.vec());
            popPath();
        }
        break;

    case SCV_MAP:
        if (val1.map())
        {
            pushPath("map");
            compareSCMap(*val1.map(), *val2.map());
            popPath();
        }
        break;

    case SCV_ADDRESS:
        pushPath("address");
        if (val1.address().type() != val2.address().type())
        {
            reportDifference(
                fmt::format("type differs: {} vs {}",
                            static_cast<int>(val1.address().type()),
                            static_cast<int>(val2.address().type())));
        }
        else if (val1.address().type() == SC_ADDRESS_TYPE_ACCOUNT)
        {
            if (!(val1.address().accountId() == val2.address().accountId()))
            {
                reportDifference("accountId differs");
            }
        }
        else if (val1.address().type() == SC_ADDRESS_TYPE_CONTRACT)
        {
            if (!(val1.address().contractId() == val2.address().contractId()))
            {
                reportDifference("contractId differs");
            }
        }
        popPath();
        break;

    case SCV_CONTRACT_INSTANCE:
        pushPath("instance");
        compareSCContractInstance(val1.instance(), val2.instance());
        popPath();
        break;

    case SCV_LEDGER_KEY_CONTRACT_INSTANCE:
        // Nothing additional to compare
        break;

    case SCV_LEDGER_KEY_NONCE:
        pushPath("nonce_key");
        compareValue("nonce", val1.nonce_key().nonce, val2.nonce_key().nonce);
        popPath();
        break;

    default:
        reportDifference(fmt::format("unknown SCVal type: {}",
                                     static_cast<int>(val1.type())));
        break;
    }
}

void
Comparator::compareSCMap(SCMap const& map1, SCMap const& map2)
{
    if (map1.size() != map2.size())
    {
        reportDifference(
            fmt::format("size differs: {} vs {}", map1.size(), map2.size()));
        return;
    }

    for (size_t i = 0; i < map1.size(); ++i)
    {
        pushPath(fmt::format("entry[{}]", i));
        pushPath("key");
        compareSCVal(map1[i].key, map2[i].key);
        popPath();
        pushPath("val");
        compareSCVal(map1[i].val, map2[i].val);
        popPath();
        popPath();
    }
}

void
Comparator::compareSCVec(SCVec const& vec1, SCVec const& vec2)
{
    compareVector("", vec1, vec2, [&](SCVal const& val1, SCVal const& val2) {
        compareSCVal(val1, val2);
    });
}

void
Comparator::compareSCContractInstance(SCContractInstance const& inst1,
                                      SCContractInstance const& inst2)
{
    pushPath("executable");
    if (inst1.executable.type() != inst2.executable.type())
    {
        reportDifference(fmt::format(
            "type differs: {} vs {}", static_cast<int>(inst1.executable.type()),
            static_cast<int>(inst2.executable.type())));
    }
    else if (inst1.executable.type() == CONTRACT_EXECUTABLE_WASM)
    {
        if (!(inst1.executable.wasm_hash() == inst2.executable.wasm_hash()))
        {
            reportDifference("wasm_hash differs");
        }
    }
    else if (inst1.executable.type() == CONTRACT_EXECUTABLE_STELLAR_ASSET)
    {
        // Nothing to compare - stellar asset has no body
    }
    popPath();

    if (inst1.storage)
    {
        pushPath("storage");
        compareSCMap(*inst1.storage, *inst2.storage);
        popPath();
    }
}

void
Comparator::compareLedgerEntry(LedgerEntry const& entry1,
                               LedgerEntry const& entry2)
{
    compareValue("lastModifiedLedgerSeq", entry1.lastModifiedLedgerSeq,
                 entry2.lastModifiedLedgerSeq, DIFF_LAST_MODIFIED_LEDGER);

    pushPath("data");
    if (entry1.data.type() != entry2.data.type())
    {
        reportDifference(fmt::format("type differs: {} vs {}",
                                     static_cast<int>(entry1.data.type()),
                                     static_cast<int>(entry2.data.type())));
        popPath();
        return;
    }

    switch (entry1.data.type())
    {
    case ACCOUNT:
        pushPath("account");
        compareAccountEntry(entry1.data.account(), entry2.data.account());
        popPath();
        break;

    case TRUSTLINE:
        pushPath("trustLine");
        compareTrustLineEntry(entry1.data.trustLine(), entry2.data.trustLine());
        popPath();
        break;

    case OFFER:
        pushPath("offer");
        compareOfferEntry(entry1.data.offer(), entry2.data.offer());
        popPath();
        break;

    case DATA:
        pushPath("data");
        compareDataEntry(entry1.data.data(), entry2.data.data());
        popPath();
        break;

    case CLAIMABLE_BALANCE:
        pushPath("claimableBalance");
        compareClaimableBalanceEntry(entry1.data.claimableBalance(),
                                     entry2.data.claimableBalance());
        popPath();
        break;

    case LIQUIDITY_POOL:
        pushPath("liquidityPool");
        compareLiquidityPoolEntry(entry1.data.liquidityPool(),
                                  entry2.data.liquidityPool());
        popPath();
        break;

    case CONTRACT_DATA:
        pushPath("contractData");
        compareContractDataEntry(entry1.data.contractData(),
                                 entry2.data.contractData());
        popPath();
        break;

    case CONTRACT_CODE:
        pushPath("contractCode");
        compareContractCodeEntry(entry1.data.contractCode(),
                                 entry2.data.contractCode());
        popPath();
        break;

    case CONFIG_SETTING:
        pushPath("configSetting");
        compareConfigSettingEntry(entry1.data.configSetting(),
                                  entry2.data.configSetting());
        popPath();
        break;

    case TTL:
        pushPath("ttl");
        compareTTLEntry(entry1.data.ttl(), entry2.data.ttl());
        popPath();
        break;

    default:
        reportDifference(fmt::format("unknown entry type: {}",
                                     static_cast<int>(entry1.data.type())));
        break;
    }
    popPath();

    // Compare extension
    pushPath("ext");
    if (entry1.ext.v() != entry2.ext.v())
    {
        reportDifference(fmt::format("version differs: {} vs {}",
                                     entry1.ext.v(), entry2.ext.v()));
    }
    else if (entry1.ext.v() == 1)
    {
        pushPath("v1");
        compareValue("sponsoringID_presence",
                     entry1.ext.v1().sponsoringID.get() != nullptr,
                     entry2.ext.v1().sponsoringID.get() != nullptr);

        if (entry1.ext.v1().sponsoringID && entry2.ext.v1().sponsoringID)
        {
            if (!(*(entry1.ext.v1().sponsoringID) ==
                  *(entry2.ext.v1().sponsoringID)))
            {
                pushPath("sponsoringID");
                reportDifference("value differs", DIFF_SPONSORING_ID);
                popPath();
            }
        }
        popPath();
    }
    popPath();
}

void
Comparator::compareAccountEntry(AccountEntry const& acc1,
                                AccountEntry const& acc2)
{
    if (!(acc1.accountID == acc2.accountID))
    {
        pushPath("accountID");
        reportDifference("value differs");
        popPath();
    }

    compareValue("balance", acc1.balance, acc2.balance, DIFF_BALANCE);
    compareValue("seqNum", acc1.seqNum, acc2.seqNum, DIFF_SEQUENCE_NUMBER);
    compareValue("numSubEntries", acc1.numSubEntries, acc2.numSubEntries,
                 DIFF_NUM_SUB_ENTRIES);

    compareOptional("inflationDest", acc1.inflationDest, acc2.inflationDest);

    compareValue("flags", acc1.flags, acc2.flags);

    if (acc1.homeDomain != acc2.homeDomain)
    {
        pushPath("homeDomain");
        reportDifference(fmt::format("value differs: '{}' vs '{}'",
                                     acc1.homeDomain, acc2.homeDomain));
        popPath();
    }

    if (!(acc1.thresholds == acc2.thresholds))
    {
        pushPath("thresholds");
        reportDifference("value differs");
        popPath();
    }

    compareVector("signers", acc1.signers, acc2.signers,
                  [&](Signer const& s1, Signer const& s2) {
                      if (!(s1.key == s2.key))
                      {
                          pushPath("key");
                          reportDifference("value differs");
                          popPath();
                      }
                      compareValue("weight", s1.weight, s2.weight);
                  });

    // Compare account extension
    pushPath("ext");
    if (acc1.ext.v() != acc2.ext.v())
    {
        reportDifference(fmt::format("version differs: {} vs {}", acc1.ext.v(),
                                     acc2.ext.v()));
    }
    else if (acc1.ext.v() == 1)
    {
        pushPath("v1");
        auto const& v1_1 = acc1.ext.v1();
        auto const& v1_2 = acc2.ext.v1();

        pushPath("liabilities");
        compareValue("buying", v1_1.liabilities.buying, v1_2.liabilities.buying,
                     DIFF_LIABILITIES);
        compareValue("selling", v1_1.liabilities.selling,
                     v1_2.liabilities.selling, DIFF_LIABILITIES);
        popPath();

        // Check nested extension version matches
        pushPath("ext");
        if (v1_1.ext.v() != v1_2.ext.v())
        {
            reportDifference(fmt::format("version differs: {} vs {}",
                                         v1_1.ext.v(), v1_2.ext.v()));
        }
        else if (v1_1.ext.v() == 2)
        {
            pushPath("v2");
            auto const& v2_1 = v1_1.ext.v2();
            auto const& v2_2 = v1_2.ext.v2();

            compareValue("numSponsored", v2_1.numSponsored, v2_2.numSponsored);
            compareValue("numSponsoring", v2_1.numSponsoring,
                         v2_2.numSponsoring);

            compareVector("signerSponsoringIDs", v2_1.signerSponsoringIDs,
                          v2_2.signerSponsoringIDs,
                          [&](xdr::pointer<AccountID> const& id1,
                              xdr::pointer<AccountID> const& id2) {
                              compareOptional("", id1, id2);
                          });

            // Check nested extension version matches
            pushPath("ext");
            if (v2_1.ext.v() != v2_2.ext.v())
            {
                reportDifference(fmt::format("version differs: {} vs {}",
                                             v2_1.ext.v(), v2_2.ext.v()));
            }
            else if (v2_1.ext.v() == 3)
            {
                pushPath("v3");
                auto const& v3_1 = v2_1.ext.v3();
                auto const& v3_2 = v2_2.ext.v3();

                compareValue("seqLedger", v3_1.seqLedger, v3_2.seqLedger,
                             DIFF_SEQ_LEDGER);
                compareValue("seqTime", v3_1.seqTime, v3_2.seqTime,
                             DIFF_SEQ_TIME);
                popPath();
            }
            popPath();
            popPath();
        }
        popPath();
        popPath();
    }
    popPath();
}

void
Comparator::compareTrustLineEntry(TrustLineEntry const& tl1,
                                  TrustLineEntry const& tl2)
{
    if (!(tl1.accountID == tl2.accountID))
    {
        pushPath("accountID");
        reportDifference("value differs");
        popPath();
    }

    pushPath("asset");
    compareTrustLineAsset(tl1.asset, tl2.asset);
    popPath();

    compareValue("balance", tl1.balance, tl2.balance, DIFF_BALANCE);
    compareValue("limit", tl1.limit, tl2.limit);
    compareValue("flags", tl1.flags, tl2.flags);

    // Compare trustline extension
    pushPath("ext");
    if (tl1.ext.v() != tl2.ext.v())
    {
        reportDifference(
            fmt::format("version differs: {} vs {}", tl1.ext.v(), tl2.ext.v()));
    }
    else if (tl1.ext.v() == 1)
    {
        pushPath("v1");
        auto const& v1_1 = tl1.ext.v1();
        auto const& v1_2 = tl2.ext.v1();

        pushPath("liabilities");
        compareValue("buying", v1_1.liabilities.buying, v1_2.liabilities.buying,
                     DIFF_LIABILITIES);
        compareValue("selling", v1_1.liabilities.selling,
                     v1_2.liabilities.selling, DIFF_LIABILITIES);
        popPath();

        // Check nested extension version matches
        pushPath("ext");
        if (v1_1.ext.v() != v1_2.ext.v())
        {
            reportDifference(fmt::format("version differs: {} vs {}",
                                         v1_1.ext.v(), v1_2.ext.v()));
        }
        else if (v1_1.ext.v() == 2)
        {
            pushPath("v2");
            compareValue("liquidityPoolUseCount",
                         v1_1.ext.v2().liquidityPoolUseCount,
                         v1_2.ext.v2().liquidityPoolUseCount);
            popPath();
        }
        popPath();
        popPath();
    }
    popPath();
}

void
Comparator::compareLedgerEntryChanges(LedgerEntryChanges const& changes1,
                                      LedgerEntryChanges const& changes2)
{
    compareVector(
        "changes", changes1, changes2,
        [&](LedgerEntryChange const& c1, LedgerEntryChange const& c2) {
            compareLedgerEntryChange(c1, c2);
        });
}

void
Comparator::compareLedgerEntryChange(LedgerEntryChange const& change1,
                                     LedgerEntryChange const& change2)
{
    if (change1.type() != change2.type())
    {
        reportDifference(fmt::format("type differs: {} vs {}",
                                     static_cast<int>(change1.type()),
                                     static_cast<int>(change2.type())));
        return;
    }

    switch (change1.type())
    {
    case LEDGER_ENTRY_CREATED:
        pushPath("created");
        compareLedgerEntry(change1.created(), change2.created());
        popPath();
        break;

    case LEDGER_ENTRY_UPDATED:
        pushPath("updated");
        compareLedgerEntry(change1.updated(), change2.updated());
        popPath();
        break;

    case LEDGER_ENTRY_REMOVED:
    {
        pushPath("removed");
        auto const& key1 = change1.removed();
        auto const& key2 = change2.removed();

        if (key1.type() != key2.type())
        {
            reportDifference(fmt::format("key type differs: {} vs {}",
                                         static_cast<int>(key1.type()),
                                         static_cast<int>(key2.type())));
        }
        else if (!(key1 == key2))
        {
            reportDifference("key differs");
        }
        popPath();
    }
    break;

    case LEDGER_ENTRY_STATE:
        pushPath("state");
        compareLedgerEntry(change1.state(), change2.state());
        popPath();
        break;

    default:
        reportDifference(fmt::format("unknown change type: {}",
                                     static_cast<int>(change1.type())));
        break;
    }
}

// Context-aware implementations
void
Comparator::compareTrustLineAsset(TrustLineAsset const& asset1,
                                  TrustLineAsset const& asset2)
{
    if (asset1.type() != asset2.type())
    {
        reportDifference(fmt::format("type differs: {} vs {}",
                                     static_cast<int>(asset1.type()),
                                     static_cast<int>(asset2.type())));
        return;
    }

    switch (asset1.type())
    {
    case ASSET_TYPE_NATIVE:
        // Nothing to compare for native assets in trustlines
        break;

    case ASSET_TYPE_CREDIT_ALPHANUM4:
        pushPath("alphaNum4");
        if (asset1.alphaNum4().assetCode != asset2.alphaNum4().assetCode)
        {
            pushPath("assetCode");
            reportDifference(
                fmt::format("value differs: '{}' vs '{}'",
                            std::string(asset1.alphaNum4().assetCode.begin(),
                                        asset1.alphaNum4().assetCode.end()),
                            std::string(asset2.alphaNum4().assetCode.begin(),
                                        asset2.alphaNum4().assetCode.end())));
            popPath();
        }
        if (!(asset1.alphaNum4().issuer == asset2.alphaNum4().issuer))
        {
            pushPath("issuer");
            reportDifference("value differs");
            popPath();
        }
        popPath();
        break;

    case ASSET_TYPE_CREDIT_ALPHANUM12:
        pushPath("alphaNum12");
        if (asset1.alphaNum12().assetCode != asset2.alphaNum12().assetCode)
        {
            pushPath("assetCode");
            reportDifference(
                fmt::format("value differs: '{}' vs '{}'",
                            std::string(asset1.alphaNum12().assetCode.begin(),
                                        asset1.alphaNum12().assetCode.end()),
                            std::string(asset2.alphaNum12().assetCode.begin(),
                                        asset2.alphaNum12().assetCode.end())));
            popPath();
        }
        if (!(asset1.alphaNum12().issuer == asset2.alphaNum12().issuer))
        {
            pushPath("issuer");
            reportDifference("value differs");
            popPath();
        }
        popPath();
        break;

    case ASSET_TYPE_POOL_SHARE:
        pushPath("poolShare");
        if (!(asset1.liquidityPoolID() == asset2.liquidityPoolID()))
        {
            reportDifference("liquidityPoolID differs");
        }
        popPath();
        break;

    default:
        reportDifference(fmt::format("unknown asset type: {}",
                                     static_cast<int>(asset1.type())));
        break;
    }
}

void
Comparator::compareAsset(Asset const& asset1, Asset const& asset2)
{
    if (asset1.type() != asset2.type())
    {
        reportDifference(fmt::format("type differs: {} vs {}",
                                     static_cast<int>(asset1.type()),
                                     static_cast<int>(asset2.type())));
        return;
    }

    switch (asset1.type())
    {
    case ASSET_TYPE_NATIVE:
        // Nothing to compare
        break;

    case ASSET_TYPE_CREDIT_ALPHANUM4:
        pushPath("alphaNum4");
        if (asset1.alphaNum4().assetCode != asset2.alphaNum4().assetCode)
        {
            pushPath("assetCode");
            reportDifference(
                fmt::format("value differs: '{}' vs '{}'",
                            std::string(asset1.alphaNum4().assetCode.begin(),
                                        asset1.alphaNum4().assetCode.end()),
                            std::string(asset2.alphaNum4().assetCode.begin(),
                                        asset2.alphaNum4().assetCode.end())));
            popPath();
        }
        if (!(asset1.alphaNum4().issuer == asset2.alphaNum4().issuer))
        {
            pushPath("issuer");
            reportDifference("value differs");
            popPath();
        }
        popPath();
        break;

    case ASSET_TYPE_CREDIT_ALPHANUM12:
        pushPath("alphaNum12");
        if (asset1.alphaNum12().assetCode != asset2.alphaNum12().assetCode)
        {
            pushPath("assetCode");
            reportDifference(
                fmt::format("value differs: '{}' vs '{}'",
                            std::string(asset1.alphaNum12().assetCode.begin(),
                                        asset1.alphaNum12().assetCode.end()),
                            std::string(asset2.alphaNum12().assetCode.begin(),
                                        asset2.alphaNum12().assetCode.end())));
            popPath();
        }
        if (!(asset1.alphaNum12().issuer == asset2.alphaNum12().issuer))
        {
            pushPath("issuer");
            reportDifference("value differs");
            popPath();
        }
        popPath();
        break;

    default:
        reportDifference(fmt::format("unknown asset type: {}",
                                     static_cast<int>(asset1.type())));
        break;
    }
}

void
Comparator::compareOfferEntry(OfferEntry const& offer1,
                              OfferEntry const& offer2)
{
    if (!(offer1.sellerID == offer2.sellerID))
    {
        pushPath("sellerID");
        reportDifference("value differs");
        popPath();
    }

    compareValue("offerID", offer1.offerID, offer2.offerID);

    pushPath("selling");
    compareAsset(offer1.selling, offer2.selling);
    popPath();

    pushPath("buying");
    compareAsset(offer1.buying, offer2.buying);
    popPath();

    compareValue("amount", offer1.amount, offer2.amount);

    if (!(offer1.price == offer2.price))
    {
        pushPath("price");
        reportDifference(fmt::format("value differs: {}/{} vs {}/{}",
                                     offer1.price.n, offer1.price.d,
                                     offer2.price.n, offer2.price.d));
        popPath();
    }

    compareValue("flags", offer1.flags, offer2.flags);

    // Compare extension
    pushPath("ext");
    if (offer1.ext.v() != offer2.ext.v())
    {
        reportDifference(fmt::format("version differs: {} vs {}",
                                     offer1.ext.v(), offer2.ext.v()));
    }
    popPath();
}

void
Comparator::compareDataEntry(DataEntry const& data1, DataEntry const& data2)
{
    if (!(data1.accountID == data2.accountID))
    {
        pushPath("accountID");
        reportDifference("value differs");
        popPath();
    }

    if (data1.dataName != data2.dataName)
    {
        pushPath("dataName");
        reportDifference(fmt::format("value differs: '{}' vs '{}'",
                                     data1.dataName, data2.dataName));
        popPath();
    }

    if (data1.dataValue != data2.dataValue)
    {
        pushPath("dataValue");
        reportDifference(fmt::format("size differs: {} vs {}",
                                     data1.dataValue.size(),
                                     data2.dataValue.size()));
        popPath();
    }

    // Compare extension
    pushPath("ext");
    if (data1.ext.v() != data2.ext.v())
    {
        reportDifference(fmt::format("version differs: {} vs {}", data1.ext.v(),
                                     data2.ext.v()));
    }
    popPath();
}

void
Comparator::compareClaimableBalanceEntry(ClaimableBalanceEntry const& cb1,
                                         ClaimableBalanceEntry const& cb2)
{
    if (!(cb1.balanceID == cb2.balanceID))
    {
        pushPath("balanceID");
        reportDifference("value differs");
        popPath();
    }

    compareVector("claimants", cb1.claimants, cb2.claimants,
                  [&](Claimant const& c1, Claimant const& c2) {
                      if (!(c1.v0().destination == c2.v0().destination))
                      {
                          pushPath("destination");
                          reportDifference("value differs");
                          popPath();
                      }
                      if (!(c1.v0().predicate == c2.v0().predicate))
                      {
                          pushPath("predicate");
                          reportDifference("value differs");
                          popPath();
                      }
                  });

    pushPath("asset");
    compareAsset(cb1.asset, cb2.asset);
    popPath();

    compareValue("amount", cb1.amount, cb2.amount);

    // Compare extension
    pushPath("ext");
    if (cb1.ext.v() != cb2.ext.v())
    {
        reportDifference(
            fmt::format("version differs: {} vs {}", cb1.ext.v(), cb2.ext.v()));
    }
    popPath();
}

void
Comparator::compareLiquidityPoolEntry(LiquidityPoolEntry const& lp1,
                                      LiquidityPoolEntry const& lp2)
{
    if (!(lp1.liquidityPoolID == lp2.liquidityPoolID))
    {
        pushPath("liquidityPoolID");
        reportDifference("value differs");
        popPath();
    }

    pushPath("body");
    if (lp1.body.type() != lp2.body.type())
    {
        reportDifference(fmt::format("type differs: {} vs {}",
                                     static_cast<int>(lp1.body.type()),
                                     static_cast<int>(lp2.body.type())));
    }
    else if (lp1.body.type() == LIQUIDITY_POOL_CONSTANT_PRODUCT)
    {
        pushPath("constantProduct");
        auto const& cp1 = lp1.body.constantProduct();
        auto const& cp2 = lp2.body.constantProduct();

        pushPath("params");
        pushPath("assetA");
        compareAsset(cp1.params.assetA, cp2.params.assetA);
        popPath();
        pushPath("assetB");
        compareAsset(cp1.params.assetB, cp2.params.assetB);
        popPath();
        compareValue("fee", cp1.params.fee, cp2.params.fee);
        popPath();

        compareValue("reserveA", cp1.reserveA, cp2.reserveA);
        compareValue("reserveB", cp1.reserveB, cp2.reserveB);
        compareValue("totalPoolShares", cp1.totalPoolShares,
                     cp2.totalPoolShares);
        compareValue("poolSharesTrustLineCount", cp1.poolSharesTrustLineCount,
                     cp2.poolSharesTrustLineCount);
        popPath();
    }
    popPath();
}

void
Comparator::compareConfigSettingEntry(ConfigSettingEntry const& cs1,
                                      ConfigSettingEntry const& cs2)
{
    if (cs1.configSettingID() != cs2.configSettingID())
    {
        reportDifference(fmt::format("configSettingID differs: {} vs {}",
                                     static_cast<int>(cs1.configSettingID()),
                                     static_cast<int>(cs2.configSettingID())));
        return;
    }

    // Compare based on config setting type
    switch (cs1.configSettingID())
    {
    case CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES:
        compareValue("contractMaxSizeBytes", cs1.contractMaxSizeBytes(),
                     cs2.contractMaxSizeBytes());
        break;

    case CONFIG_SETTING_CONTRACT_COMPUTE_V0:
    {
        pushPath("contractCompute");
        auto const& cv1 = cs1.contractCompute();
        auto const& cv2 = cs2.contractCompute();

        compareValue("ledgerMaxInstructions", cv1.ledgerMaxInstructions,
                     cv2.ledgerMaxInstructions);
        compareValue("txMaxInstructions", cv1.txMaxInstructions,
                     cv2.txMaxInstructions);
        compareValue("feeRatePerInstructionsIncrement",
                     cv1.feeRatePerInstructionsIncrement,
                     cv2.feeRatePerInstructionsIncrement);
        compareValue("txMemoryLimit", cv1.txMemoryLimit, cv2.txMemoryLimit);
        popPath();
    }
    break;

    case CONFIG_SETTING_CONTRACT_LEDGER_COST_V0:
    {
        pushPath("contractLedgerCost");
        auto const& lc1 = cs1.contractLedgerCost();
        auto const& lc2 = cs2.contractLedgerCost();

        compareValue("ledgerMaxDiskReadEntries", lc1.ledgerMaxDiskReadEntries,
                     lc2.ledgerMaxDiskReadEntries);
        compareValue("ledgerMaxDiskReadBytes", lc1.ledgerMaxDiskReadBytes,
                     lc2.ledgerMaxDiskReadBytes);
        compareValue("ledgerMaxWriteLedgerEntries",
                     lc1.ledgerMaxWriteLedgerEntries,
                     lc2.ledgerMaxWriteLedgerEntries);
        compareValue("ledgerMaxWriteBytes", lc1.ledgerMaxWriteBytes,
                     lc2.ledgerMaxWriteBytes);
        compareValue("txMaxDiskReadEntries", lc1.txMaxDiskReadEntries,
                     lc2.txMaxDiskReadEntries);
        compareValue("txMaxDiskReadBytes", lc1.txMaxDiskReadBytes,
                     lc2.txMaxDiskReadBytes);
        compareValue("txMaxWriteLedgerEntries", lc1.txMaxWriteLedgerEntries,
                     lc2.txMaxWriteLedgerEntries);
        compareValue("txMaxWriteBytes", lc1.txMaxWriteBytes,
                     lc2.txMaxWriteBytes);
        compareValue("feeDiskReadLedgerEntry", lc1.feeDiskReadLedgerEntry,
                     lc2.feeDiskReadLedgerEntry);
        compareValue("feeWriteLedgerEntry", lc1.feeWriteLedgerEntry,
                     lc2.feeWriteLedgerEntry);
        compareValue("feeDiskRead1KB", lc1.feeDiskRead1KB, lc2.feeDiskRead1KB);
        compareValue("sorobanStateTargetSizeBytes",
                     lc1.sorobanStateTargetSizeBytes,
                     lc2.sorobanStateTargetSizeBytes);
        compareValue("rentFee1KBSorobanStateSizeLow",
                     lc1.rentFee1KBSorobanStateSizeLow,
                     lc2.rentFee1KBSorobanStateSizeLow);
        compareValue("rentFee1KBSorobanStateSizeHigh",
                     lc1.rentFee1KBSorobanStateSizeHigh,
                     lc2.rentFee1KBSorobanStateSizeHigh);
        compareValue("sorobanStateRentFeeGrowthFactor",
                     lc1.sorobanStateRentFeeGrowthFactor,
                     lc2.sorobanStateRentFeeGrowthFactor);
        popPath();
    }
    break;

        // Add other config setting types as needed...

    default:
        reportDifference(fmt::format("unknown config setting type: {}",
                                     static_cast<int>(cs1.configSettingID())));
        break;
    }
}

void
Comparator::compareContractDataEntry(ContractDataEntry const& cd1,
                                     ContractDataEntry const& cd2)
{
    compareValue("ext.v", cd1.ext.v(), cd2.ext.v());

    if (!(cd1.contract == cd2.contract))
    {
        pushPath("contract");
        reportDifference("value differs");
        popPath();
    }

    pushPath("key");
    compareSCVal(cd1.key, cd2.key);
    popPath();

    compareValue("durability", static_cast<int>(cd1.durability),
                 static_cast<int>(cd2.durability));

    pushPath("val");
    compareSCVal(cd1.val, cd2.val);
    popPath();
}

void
Comparator::compareContractCodeEntry(ContractCodeEntry const& cc1,
                                     ContractCodeEntry const& cc2)
{
    compareValue("ext.v", cc1.ext.v(), cc2.ext.v());

    if (!(cc1.hash == cc2.hash))
    {
        pushPath("hash");
        reportDifference("value differs");
        popPath();
    }

    if (cc1.code != cc2.code)
    {
        pushPath("code");
        reportDifference(fmt::format("size differs: {} vs {}", cc1.code.size(),
                                     cc2.code.size()));
        popPath();
    }
}

void
Comparator::compareTTLEntry(TTLEntry const& ttl1, TTLEntry const& ttl2)
{
    if (!(ttl1.keyHash == ttl2.keyHash))
    {
        pushPath("keyHash");
        reportDifference("value differs");
        popPath();
    }

    compareValue("liveUntilLedgerSeq", ttl1.liveUntilLedgerSeq,
                 ttl2.liveUntilLedgerSeq);
}

void
Comparator::compareTransactionMeta(TransactionMeta const& meta1,
                                   TransactionMeta const& meta2, size_t txIndex)
{
    pushPath(fmt::format("tx[{}]", txIndex));

    if (meta1.v() != meta2.v())
    {
        reportDifference(fmt::format("meta version differs: {} vs {}",
                                     meta1.v(), meta2.v()));
        popPath();
        return;
    }

    switch (meta1.v())
    {
    case 0:
        compareVector("operations", meta1.operations(), meta2.operations(),
                      [&](OperationMeta const& op1, OperationMeta const& op2) {
                          if (!(op1 == op2))
                          {
                              compareLedgerEntryChanges(op1.changes,
                                                        op2.changes);
                          }
                      });
        break;

    case 1:
    {
        auto const& v1_1 = meta1.v1();
        auto const& v1_2 = meta2.v1();

        pushPath("v1");
        pushPath("txChanges");
        compareLedgerEntryChanges(v1_1.txChanges, v1_2.txChanges);
        popPath();

        compareVector("operations", v1_1.operations, v1_2.operations,
                      [&](OperationMeta const& op1, OperationMeta const& op2) {
                          if (!(op1 == op2))
                          {
                              compareLedgerEntryChanges(op1.changes,
                                                        op2.changes);
                          }
                      });
        popPath();
    }
    break;

    case 2:
    {
        auto const& v2_1 = meta1.v2();
        auto const& v2_2 = meta2.v2();

        pushPath("v2");
        pushPath("txChangesBefore");
        compareLedgerEntryChanges(v2_1.txChangesBefore, v2_2.txChangesBefore);
        popPath();

        compareVector("operations", v2_1.operations, v2_2.operations,
                      [&](OperationMeta const& op1, OperationMeta const& op2) {
                          if (!(op1 == op2))
                          {
                              compareLedgerEntryChanges(op1.changes,
                                                        op2.changes);
                          }
                      });

        pushPath("txChangesAfter");
        compareLedgerEntryChanges(v2_1.txChangesAfter, v2_2.txChangesAfter);
        popPath();
        popPath();
    }
    break;

    case 3:
    {
        auto const& v3_1 = meta1.v3();
        auto const& v3_2 = meta2.v3();

        pushPath("v3");

        // Compare extension
        if (v3_1.ext.v() != v3_2.ext.v())
        {
            reportDifference(fmt::format("ext version differs: {} vs {}",
                                         v3_1.ext.v(), v3_2.ext.v()));
        }

        // Check Soroban meta
        if (v3_1.sorobanMeta && v3_2.sorobanMeta)
        {
            auto const& sm1 = *(v3_1.sorobanMeta);
            auto const& sm2 = *(v3_2.sorobanMeta);

            pushPath("sorobanMeta");

            // Compare extension for SorobanTransactionMeta
            if (sm1.ext.v() != sm2.ext.v())
            {
                reportDifference(fmt::format("ext version differs: {} vs {}",
                                             sm1.ext.v(), sm2.ext.v()));
            }
            else if (sm1.ext.v() == 1)
            {
                auto const& v1ext1 = sm1.ext.v1();
                auto const& v1ext2 = sm2.ext.v1();

                pushPath("ext.v1");
                compareValue("totalNonRefundableResourceFeeCharged",
                             v1ext1.totalNonRefundableResourceFeeCharged,
                             v1ext2.totalNonRefundableResourceFeeCharged,
                             DIFF_SOROBAN_FEES);

                compareValue("totalRefundableResourceFeeCharged",
                             v1ext1.totalRefundableResourceFeeCharged,
                             v1ext2.totalRefundableResourceFeeCharged,
                             DIFF_SOROBAN_FEES);

                compareValue("rentFeeCharged", v1ext1.rentFeeCharged,
                             v1ext2.rentFeeCharged, DIFF_SOROBAN_FEES);
                popPath();
            }

            // Compare events
            compareVector(
                "events", sm1.events, sm2.events,
                [&](ContractEvent const& event1, ContractEvent const& event2) {
                    compareContractEvent(event1, event2);
                });

            // Compare return value
            pushPath("returnValue");
            compareSCVal(sm1.returnValue, sm2.returnValue);
            popPath();

            // Compare diagnostic events
            compareVector("diagnosticEvents", sm1.diagnosticEvents,
                          sm2.diagnosticEvents,
                          [&](DiagnosticEvent const& diag1,
                              DiagnosticEvent const& diag2) {
                              pushPath("diagnosticEvent");
                              compareValue("inSuccessfulContractCall",
                                           diag1.inSuccessfulContractCall,
                                           diag2.inSuccessfulContractCall);

                              pushPath("event");
                              compareContractEvent(diag1.event, diag2.event);
                              popPath();
                              popPath();
                          });

            popPath();
        }
        else if (v3_1.sorobanMeta || v3_2.sorobanMeta)
        {
            reportDifference(
                fmt::format("sorobanMeta presence differs: {} vs {}",
                            static_cast<bool>(v3_1.sorobanMeta),
                            static_cast<bool>(v3_2.sorobanMeta)));
        }

        pushPath("txChangesBefore");
        compareLedgerEntryChanges(v3_1.txChangesBefore, v3_2.txChangesBefore);
        popPath();

        compareVector("operations", v3_1.operations, v3_2.operations,
                      [&](OperationMeta const& op1, OperationMeta const& op2) {
                          if (!(op1 == op2))
                          {
                              compareLedgerEntryChanges(op1.changes,
                                                        op2.changes);
                          }
                      });

        pushPath("txChangesAfter");
        compareLedgerEntryChanges(v3_1.txChangesAfter, v3_2.txChangesAfter);
        popPath();

        popPath();
    }
    break;

    default:
        // For newer versions, fall back to full comparison
        if (!(meta1 == meta2))
        {
            reportDifference(
                fmt::format("meta differs (version {})", meta1.v()));
        }
        break;
    }

    popPath();
}

void
Comparator::compareTransactionResult(TransactionResult const& result1,
                                     TransactionResult const& result2)
{
    pushPath("feeCharged");
    if (result1.feeCharged != result2.feeCharged)
    {
        reportDifference(fmt::format("feeCharged differs: {} vs {}",
                                     result1.feeCharged, result2.feeCharged),
                         DIFF_FEE_CHARGED);
    }
    popPath();

    pushPath("result");
    if (result1.result.code() != result2.result.code())
    {
        reportDifference(fmt::format("result code differs: {} vs {}",
                                     static_cast<int>(result1.result.code()),
                                     static_cast<int>(result2.result.code())),
                         DIFF_TRANSACTION_RESULT_CODE);
    }
    else
    {
        switch (result1.result.code())
        {
        case txSUCCESS:
        case txFAILED:
            // Compare results vector
            {
                auto const& results1 = result1.result.results();
                auto const& results2 = result2.result.results();

                if (results1.size() != results2.size())
                {
                    reportDifference(
                        fmt::format("results count differs: {} vs {}",
                                    results1.size(), results2.size()));
                }
                else
                {
                    for (size_t i = 0; i < results1.size(); ++i)
                    {
                        pushPath(fmt::format("results[{}]", i));
                        compareOperationResult(results1[i], results2[i]);
                        popPath();
                    }
                }
            }
            break;

        case txFEE_BUMP_INNER_SUCCESS:
        case txFEE_BUMP_INNER_FAILED:
            // Compare inner result pair
            {
                auto const& inner1 = result1.result.innerResultPair();
                auto const& inner2 = result2.result.innerResultPair();

                pushPath("innerResultPair");

                // Compare the transaction hash
                if (!(inner1.transactionHash == inner2.transactionHash))
                {
                    pushPath("transactionHash");
                    reportDifference("value differs");
                    popPath();
                }

                // Compare the inner result
                pushPath("result");
                // The inner result is an InnerTransactionResult, which has
                // feeCharged and result fields
                compareValue("feeCharged", inner1.result.feeCharged,
                             inner2.result.feeCharged, DIFF_FEE_CHARGED);

                pushPath("result");
                if (inner1.result.result.code() != inner2.result.result.code())
                {
                    reportDifference(
                        fmt::format(
                            "code differs: {} vs {}",
                            static_cast<int>(inner1.result.result.code()),
                            static_cast<int>(inner2.result.result.code())),
                        DIFF_TRANSACTION_RESULT_CODE);
                }
                popPath();

                popPath();
                popPath();
            }
            break;

        default:
            // For other error codes, no additional data to compare
            break;
        }
    }
    popPath();

    // Compare extension
    pushPath("ext");
    if (result1.ext.v() != result2.ext.v())
    {
        reportDifference(fmt::format("ext version differs: {} vs {}",
                                     result1.ext.v(), result2.ext.v()));
    }
    popPath();
}

void
Comparator::compareOperationResult(OperationResult const& result1,
                                   OperationResult const& result2)
{
    if (result1.code() != result2.code())
    {
        reportDifference(fmt::format("operation result code differs: {} vs {}",
                                     static_cast<int>(result1.code()),
                                     static_cast<int>(result2.code())),
                         DIFF_OPERATION_RESULT_CODE);
        return;
    }

    // Compare specific operation results based on the code
    if (result1.code() == opINNER)
    {
        auto const& tr1 = result1.tr();
        auto const& tr2 = result2.tr();

        if (tr1.type() != tr2.type())
        {
            reportDifference(fmt::format("operation type differs: {} vs {}",
                                         static_cast<int>(tr1.type()),
                                         static_cast<int>(tr2.type())));
            return;
        }

        // Compare specific operation result types
        switch (tr1.type())
        {
        case CREATE_ACCOUNT:
            // CreateAccountResult is empty, nothing to compare
            break;

        case PAYMENT:
            // PaymentResult is empty, nothing to compare
            break;

        case PATH_PAYMENT_STRICT_RECEIVE:
        case PATH_PAYMENT_STRICT_SEND:
            if (tr1.type() == PATH_PAYMENT_STRICT_RECEIVE)
            {
                auto const& pp1 = tr1.pathPaymentStrictReceiveResult();
                auto const& pp2 = tr2.pathPaymentStrictReceiveResult();

                if (pp1.code() == PATH_PAYMENT_STRICT_RECEIVE_SUCCESS &&
                    pp2.code() == PATH_PAYMENT_STRICT_RECEIVE_SUCCESS)
                {
                    auto const& success1 = pp1.success();
                    auto const& success2 = pp2.success();

                    pushPath("pathPaymentSuccess");
                    // Compare offers vector
                    if (success1.offers.size() != success2.offers.size())
                    {
                        reportDifference(fmt::format(
                            "offers count differs: {} vs {}",
                            success1.offers.size(), success2.offers.size()));
                    }

                    if (!(success1.last == success2.last))
                    {
                        reportDifference("last asset differs");
                    }
                    popPath();
                }
            }
            break;

        case MANAGE_SELL_OFFER:
        case MANAGE_BUY_OFFER:
        case CREATE_PASSIVE_SELL_OFFER:
            // Compare ManageOfferResult
            if (tr1.type() == MANAGE_SELL_OFFER)
            {
                auto const& mo1 = tr1.manageSellOfferResult();
                auto const& mo2 = tr2.manageSellOfferResult();

                if (mo1.code() == MANAGE_SELL_OFFER_SUCCESS &&
                    mo2.code() == MANAGE_SELL_OFFER_SUCCESS)
                {
                    auto const& success1 = mo1.success();
                    auto const& success2 = mo2.success();

                    pushPath("manageOfferSuccess");
                    if (success1.offersClaimed.size() !=
                        success2.offersClaimed.size())
                    {
                        reportDifference(
                            fmt::format("offersClaimed count differs: {} vs {}",
                                        success1.offersClaimed.size(),
                                        success2.offersClaimed.size()));
                    }

                    if (success1.offer.effect() != success2.offer.effect())
                    {
                        reportDifference(fmt::format(
                            "offer effect differs: {} vs {}",
                            static_cast<int>(success1.offer.effect()),
                            static_cast<int>(success2.offer.effect())));
                    }
                    popPath();
                }
            }
            break;

        case SET_OPTIONS:
            // SetOptionsResult is empty, nothing to compare
            break;

        case CHANGE_TRUST:
            // ChangeTrustResult is empty, nothing to compare
            break;

        case ALLOW_TRUST:
            // AllowTrustResult is empty, nothing to compare
            break;

        case ACCOUNT_MERGE:
            if (tr1.accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS &&
                tr2.accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS)
            {
                if (tr1.accountMergeResult().sourceAccountBalance() !=
                    tr2.accountMergeResult().sourceAccountBalance())
                {
                    reportDifference(
                        fmt::format(
                            "sourceAccountBalance differs: {} vs {}",
                            tr1.accountMergeResult().sourceAccountBalance(),
                            tr2.accountMergeResult().sourceAccountBalance()),
                        DIFF_BALANCE);
                }
            }
            break;

        case INFLATION:
            if (tr1.inflationResult().code() == INFLATION_SUCCESS &&
                tr2.inflationResult().code() == INFLATION_SUCCESS)
            {
                auto const& payouts1 = tr1.inflationResult().payouts();
                auto const& payouts2 = tr2.inflationResult().payouts();

                if (payouts1.size() != payouts2.size())
                {
                    reportDifference(
                        fmt::format("inflation payouts count differs: {} vs {}",
                                    payouts1.size(), payouts2.size()));
                }
            }
            break;

        case MANAGE_DATA:
            // ManageDataResult is empty, nothing to compare
            break;

        case BUMP_SEQUENCE:
            // BumpSequenceResult is empty, nothing to compare
            break;

        case CREATE_CLAIMABLE_BALANCE:
            if (tr1.createClaimableBalanceResult().code() ==
                    CREATE_CLAIMABLE_BALANCE_SUCCESS &&
                tr2.createClaimableBalanceResult().code() ==
                    CREATE_CLAIMABLE_BALANCE_SUCCESS)
            {
                if (!(tr1.createClaimableBalanceResult().balanceID() ==
                      tr2.createClaimableBalanceResult().balanceID()))
                {
                    reportDifference("claimable balance ID differs");
                }
            }
            break;

        case CLAIM_CLAIMABLE_BALANCE:
            // ClaimClaimableBalanceResult is empty, nothing to compare
            break;

        case BEGIN_SPONSORING_FUTURE_RESERVES:
            // BeginSponsoringFutureReservesResult is empty, nothing to compare
            break;

        case END_SPONSORING_FUTURE_RESERVES:
            // EndSponsoringFutureReservesResult is empty, nothing to compare
            break;

        case REVOKE_SPONSORSHIP:
            // RevokeSponsorshipResult is empty, nothing to compare
            break;

        case CLAWBACK:
            // ClawbackResult is empty, nothing to compare
            break;

        case CLAWBACK_CLAIMABLE_BALANCE:
            // ClawbackClaimableBalanceResult is empty, nothing to compare
            break;

        case SET_TRUST_LINE_FLAGS:
            // SetTrustLineFlagsResult is empty, nothing to compare
            break;

        case LIQUIDITY_POOL_DEPOSIT:
            // LiquidityPoolDepositResult is empty for success
            break;

        case LIQUIDITY_POOL_WITHDRAW:
            // LiquidityPoolWithdrawResult is empty for success
            break;

        case INVOKE_HOST_FUNCTION:
            if (tr1.invokeHostFunctionResult().code() ==
                    INVOKE_HOST_FUNCTION_SUCCESS &&
                tr2.invokeHostFunctionResult().code() ==
                    INVOKE_HOST_FUNCTION_SUCCESS)
            {
                // Success case is empty, nothing to compare
            }
            break;

        case EXTEND_FOOTPRINT_TTL:
            // ExtendFootprintTTLResult is empty, nothing to compare
            break;

        case RESTORE_FOOTPRINT:
            // RestoreFootprintResult is empty, nothing to compare
            break;

        default:
            reportDifference(fmt::format("unknown operation type: {}",
                                         static_cast<int>(tr1.type())));
            break;
        }
    }
}

} // namespace xdrcomp
} // namespace stellar

#endif // BUILD_TESTS