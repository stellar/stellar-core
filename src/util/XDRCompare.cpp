// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/XDRCompare.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-types.h"
#include <fmt/format.h>

namespace stellar
{
namespace xdrcomp
{

// ComparisonContext implementation
std::string
ComparisonContext::getCurrentPath() const
{
    std::string path;
    for (auto const& component : pathStack)
    {
        if (!path.empty())
            path += ".";
        path += component;
    }
    return path;
}

void
ComparisonContext::pushPath(std::string const& component)
{
    pathStack.push_back(component);
}

void
ComparisonContext::popPath()
{
    if (!pathStack.empty())
        pathStack.pop_back();
}

void
ComparisonContext::reportDifference(std::string const& message)
{
    std::string fullMessage = getCurrentPath() + ": " + message;
    if (collectDifferences)
    {
        differences.push_back(fullMessage);
    }
    else
    {
        CLOG_ERROR(Ledger, "{} vs {}: {}", name1, name2, fullMessage);
    }
}

void
compareContractEvent(std::string const& name1, std::string const& name2,
                     ContractEvent const& event1, ContractEvent const& event2,
                     std::string const& eventDesc)
{
    compareValue(name1, name2, static_cast<int>(event1.type),
                 static_cast<int>(event2.type), eventDesc + " type");

    compareOptional(name1, name2, event1.contractID, event2.contractID,
                    eventDesc + " contractID");

    if (event1.body.v() != event2.body.v())
    {
        CLOG_ERROR(Ledger, "{} body version differs: {} {} vs {} {}", eventDesc,
                   name1, event1.body.v(), name2, event2.body.v());
    }
    else if (event1.body.v() == 0)
    {
        auto const& v0_1 = event1.body.v0();
        auto const& v0_2 = event2.body.v0();

        compareVector(
            name1, name2, v0_1.topics, v0_2.topics, eventDesc + " topic",
            [&](SCVal const& topic1, SCVal const& topic2, size_t j) {
                compareSCVal(name1, name2, topic1, topic2,
                             fmt::format("{} topic[{}]", eventDesc, j));
            });

        compareSCVal(name1, name2, v0_1.data, v0_2.data, eventDesc + " data");
    }
}

void
compareSCVal(std::string const& name1, std::string const& name2,
             SCVal const& val1, SCVal const& val2, std::string const& valDesc)
{
    if (val1.type() != val2.type())
    {
        CLOG_ERROR(Ledger, "{} type differs: {} {} vs {} {}", valDesc, name1,
                   static_cast<int>(val1.type()), name2,
                   static_cast<int>(val2.type()));
        return;
    }

    switch (val1.type())
    {
    case SCV_BOOL:
        compareValue(name1, name2, val1.b(), val2.b(), valDesc + " bool");
        break;

    case SCV_VOID:
        // Nothing to compare
        break;

    case SCV_ERROR:
        if (val1.error().type() != val2.error().type())
        {
            CLOG_ERROR(Ledger, "{} error type differs: {} {} vs {} {}", valDesc,
                       name1, static_cast<int>(val1.error().type()), name2,
                       static_cast<int>(val2.error().type()));
        }
        else if (val1.error().type() == SCE_CONTRACT)
        {
            compareValue(name1, name2, val1.error().contractCode(),
                         val2.error().contractCode(),
                         valDesc + " error contract code");
        }
        break;

    case SCV_U32:
        compareValue(name1, name2, val1.u32(), val2.u32(), valDesc + " u32");
        break;

    case SCV_I32:
        compareValue(name1, name2, val1.i32(), val2.i32(), valDesc + " i32");
        break;

    case SCV_U64:
        compareValue(name1, name2, val1.u64(), val2.u64(), valDesc + " u64");
        break;

    case SCV_I64:
        compareValue(name1, name2, val1.i64(), val2.i64(), valDesc + " i64");
        break;

    case SCV_TIMEPOINT:
        compareValue(name1, name2, val1.timepoint(), val2.timepoint(),
                     valDesc + " timepoint");
        break;

    case SCV_DURATION:
        compareValue(name1, name2, val1.duration(), val2.duration(),
                     valDesc + " duration");
        break;

    case SCV_U128:
        compareValue(name1, name2, val1.u128().lo, val2.u128().lo,
                     valDesc + " u128.lo");
        compareValue(name1, name2, val1.u128().hi, val2.u128().hi,
                     valDesc + " u128.hi");
        break;

    case SCV_I128:
        compareValue(name1, name2, val1.i128().lo, val2.i128().lo,
                     valDesc + " i128.lo");
        compareValue(name1, name2, val1.i128().hi, val2.i128().hi,
                     valDesc + " i128.hi");
        break;

    case SCV_U256:
        compareValue(name1, name2, val1.u256().lo_lo, val2.u256().lo_lo,
                     valDesc + " u256.lo_lo");
        compareValue(name1, name2, val1.u256().lo_hi, val2.u256().lo_hi,
                     valDesc + " u256.lo_hi");
        compareValue(name1, name2, val1.u256().hi_lo, val2.u256().hi_lo,
                     valDesc + " u256.hi_lo");
        compareValue(name1, name2, val1.u256().hi_hi, val2.u256().hi_hi,
                     valDesc + " u256.hi_hi");
        break;

    case SCV_I256:
        compareValue(name1, name2, val1.i256().lo_lo, val2.i256().lo_lo,
                     valDesc + " i256.lo_lo");
        compareValue(name1, name2, val1.i256().lo_hi, val2.i256().lo_hi,
                     valDesc + " i256.lo_hi");
        compareValue(name1, name2, val1.i256().hi_lo, val2.i256().hi_lo,
                     valDesc + " i256.hi_lo");
        compareValue(name1, name2, val1.i256().hi_hi, val2.i256().hi_hi,
                     valDesc + " i256.hi_hi");
        break;

    case SCV_BYTES:
        if (val1.bytes() != val2.bytes())
        {
            CLOG_ERROR(Ledger, "{} bytes differ: {} size {} vs {} size {}",
                       valDesc, name1, val1.bytes().size(), name2,
                       val2.bytes().size());
        }
        break;

    case SCV_STRING:
        if (val1.str() != val2.str())
        {
            CLOG_ERROR(Ledger, "{} string differs: {} '{}' vs {} '{}'", valDesc,
                       name1, val1.str(), name2, val2.str());
        }
        break;

    case SCV_SYMBOL:
        if (val1.sym() != val2.sym())
        {
            CLOG_ERROR(Ledger, "{} symbol differs: {} '{}' vs {} '{}'", valDesc,
                       name1, val1.sym(), name2, val2.sym());
        }
        break;

    case SCV_VEC:
        if (val1.vec())
        {
            compareSCVec(name1, name2, *val1.vec(), *val2.vec(),
                         valDesc + " vec");
        }
        break;

    case SCV_MAP:
        if (val1.map())
        {
            compareSCMap(name1, name2, *val1.map(), *val2.map(),
                         valDesc + " map");
        }
        break;

    case SCV_ADDRESS:
        if (val1.address().type() != val2.address().type())
        {
            CLOG_ERROR(Ledger, "{} address type differs: {} {} vs {} {}",
                       valDesc, name1, static_cast<int>(val1.address().type()),
                       name2, static_cast<int>(val2.address().type()));
        }
        else if (val1.address().type() == SC_ADDRESS_TYPE_ACCOUNT)
        {
            if (!(val1.address().accountId() == val2.address().accountId()))
            {
                CLOG_ERROR(Ledger, "{} address accountId differs", valDesc);
            }
        }
        else if (val1.address().type() == SC_ADDRESS_TYPE_CONTRACT)
        {
            if (!(val1.address().contractId() == val2.address().contractId()))
            {
                CLOG_ERROR(Ledger, "{} address contractId differs", valDesc);
            }
        }
        break;

    case SCV_CONTRACT_INSTANCE:
        compareSCContractInstance(name1, name2, val1.instance(),
                                  val2.instance(), valDesc + " instance");
        break;

    case SCV_LEDGER_KEY_CONTRACT_INSTANCE:
        // Nothing additional to compare
        break;

    case SCV_LEDGER_KEY_NONCE:
        compareValue(name1, name2, val1.nonce_key().nonce,
                     val2.nonce_key().nonce, valDesc + " nonce");
        break;

    default:
        CLOG_ERROR(Ledger, "{} unknown SCVal type: {}", valDesc,
                   static_cast<int>(val1.type()));
        break;
    }
}

void
compareSCMap(std::string const& name1, std::string const& name2,
             SCMap const& map1, SCMap const& map2, std::string const& mapDesc)
{
    if (map1.size() != map2.size())
    {
        CLOG_ERROR(Ledger, "{} size differs: {} {} vs {} {}", mapDesc, name1,
                   map1.size(), name2, map2.size());
        return;
    }

    for (size_t i = 0; i < map1.size(); ++i)
    {
        compareSCVal(name1, name2, map1[i].key, map2[i].key,
                     fmt::format("{} entry[{}].key", mapDesc, i));
        compareSCVal(name1, name2, map1[i].val, map2[i].val,
                     fmt::format("{} entry[{}].val", mapDesc, i));
    }
}

void
compareSCVec(std::string const& name1, std::string const& name2,
             SCVec const& vec1, SCVec const& vec2, std::string const& vecDesc)
{
    compareVector(name1, name2, vec1, vec2, vecDesc,
                  [&](SCVal const& val1, SCVal const& val2, size_t i) {
                      compareSCVal(name1, name2, val1, val2,
                                   fmt::format("{} [{}]", vecDesc, i));
                  });
}

void
compareSCContractInstance(std::string const& name1, std::string const& name2,
                          SCContractInstance const& inst1,
                          SCContractInstance const& inst2,
                          std::string const& instDesc)
{
    if (inst1.executable.type() != inst2.executable.type())
    {
        CLOG_ERROR(Ledger, "{} executable type differs: {} {} vs {} {}",
                   instDesc, name1, static_cast<int>(inst1.executable.type()),
                   name2, static_cast<int>(inst2.executable.type()));
    }
    else if (inst1.executable.type() == CONTRACT_EXECUTABLE_WASM)
    {
        if (!(inst1.executable.wasm_hash() == inst2.executable.wasm_hash()))
        {
            CLOG_ERROR(Ledger, "{} wasm hash differs", instDesc);
        }
    }
    else if (inst1.executable.type() == CONTRACT_EXECUTABLE_STELLAR_ASSET)
    {
        // Nothing to compare - stellar asset has no body
    }

    if (inst1.storage)
    {
        compareSCMap(name1, name2, *inst1.storage, *inst2.storage,
                     instDesc + " storage");
    }
}

void
compareLedgerEntry(std::string const& name1, std::string const& name2,
                   LedgerEntry const& entry1, LedgerEntry const& entry2,
                   std::string const& entryDesc)
{
    compareValue(name1, name2, entry1.lastModifiedLedgerSeq,
                 entry2.lastModifiedLedgerSeq,
                 entryDesc + " lastModifiedLedgerSeq");

    if (entry1.data.type() != entry2.data.type())
    {
        CLOG_ERROR(Ledger, "{} data type differs: {} {} vs {} {}", entryDesc,
                   name1, static_cast<int>(entry1.data.type()), name2,
                   static_cast<int>(entry2.data.type()));
        return;
    }

    switch (entry1.data.type())
    {
    case ACCOUNT:
        compareAccountEntry(name1, name2, entry1.data.account(),
                            entry2.data.account(), entryDesc + " account");
        break;

    case TRUSTLINE:
        compareTrustLineEntry(name1, name2, entry1.data.trustLine(),
                              entry2.data.trustLine(),
                              entryDesc + " trustline");
        break;

    case OFFER:
        compareOfferEntry(name1, name2, entry1.data.offer(),
                          entry2.data.offer(), entryDesc + " offer");
        break;

    case DATA:
        compareDataEntry(name1, name2, entry1.data.data(), entry2.data.data(),
                         entryDesc + " data");
        break;

    case CLAIMABLE_BALANCE:
        compareClaimableBalanceEntry(
            name1, name2, entry1.data.claimableBalance(),
            entry2.data.claimableBalance(), entryDesc + " claimableBalance");
        break;

    case LIQUIDITY_POOL:
        compareLiquidityPoolEntry(name1, name2, entry1.data.liquidityPool(),
                                  entry2.data.liquidityPool(),
                                  entryDesc + " liquidityPool");
        break;

    case CONTRACT_DATA:
        compareContractDataEntry(name1, name2, entry1.data.contractData(),
                                 entry2.data.contractData(),
                                 entryDesc + " contractData");
        break;

    case CONTRACT_CODE:
        compareContractCodeEntry(name1, name2, entry1.data.contractCode(),
                                 entry2.data.contractCode(),
                                 entryDesc + " contractCode");
        break;

    case CONFIG_SETTING:
        compareConfigSettingEntry(name1, name2, entry1.data.configSetting(),
                                  entry2.data.configSetting(),
                                  entryDesc + " configSetting");
        break;

    case TTL:
        compareTTLEntry(name1, name2, entry1.data.ttl(), entry2.data.ttl(),
                        entryDesc + " ttl");
        break;

    default:
        CLOG_ERROR(Ledger, "{} unknown entry type: {}", entryDesc,
                   static_cast<int>(entry1.data.type()));
        break;
    }

    // Compare extension
    if (entry1.ext.v() != entry2.ext.v())
    {
        CLOG_ERROR(Ledger, "{} ext version differs: {} {} vs {} {}", entryDesc,
                   name1, entry1.ext.v(), name2, entry2.ext.v());
    }
    else if (entry1.ext.v() == 1)
    {
        compareValue(name1, name2,
                     entry1.ext.v1().sponsoringID.get() != nullptr,
                     entry2.ext.v1().sponsoringID.get() != nullptr,
                     entryDesc + " sponsoringID presence");

        if (entry1.ext.v1().sponsoringID && entry2.ext.v1().sponsoringID)
        {
            if (!(*(entry1.ext.v1().sponsoringID) ==
                  *(entry2.ext.v1().sponsoringID)))
            {
                CLOG_ERROR(Ledger, "{} sponsoringID differs", entryDesc);
            }
        }
    }
}

void
compareAccountEntry(std::string const& name1, std::string const& name2,
                    AccountEntry const& acc1, AccountEntry const& acc2,
                    std::string const& accDesc)
{
    if (!(acc1.accountID == acc2.accountID))
    {
        CLOG_ERROR(Ledger, "{} accountID differs", accDesc);
    }

    compareValue(name1, name2, acc1.balance, acc2.balance,
                 accDesc + " balance");
    compareValue(name1, name2, acc1.seqNum, acc2.seqNum, accDesc + " seqNum");
    compareValue(name1, name2, acc1.numSubEntries, acc2.numSubEntries,
                 accDesc + " numSubEntries");

    compareOptional(name1, name2, acc1.inflationDest, acc2.inflationDest,
                    accDesc + " inflationDest");

    compareValue(name1, name2, acc1.flags, acc2.flags, accDesc + " flags");

    if (acc1.homeDomain != acc2.homeDomain)
    {
        CLOG_ERROR(Ledger, "{} homeDomain differs: {} '{}' vs {} '{}'", accDesc,
                   name1, acc1.homeDomain, name2, acc2.homeDomain);
    }

    if (!(acc1.thresholds == acc2.thresholds))
    {
        CLOG_ERROR(Ledger, "{} thresholds differ", accDesc);
    }

    compareVector(
        name1, name2, acc1.signers, acc2.signers, accDesc + " signers",
        [&](Signer const& s1, Signer const& s2, size_t i) {
            if (!(s1.key == s2.key))
            {
                CLOG_ERROR(Ledger, "{} signer[{}] key differs", accDesc, i);
            }
            compareValue(name1, name2, s1.weight, s2.weight,
                         fmt::format("{} signer[{}] weight", accDesc, i));
        });

    // Compare account extension
    if (acc1.ext.v() != acc2.ext.v())
    {
        CLOG_ERROR(Ledger, "{} ext version differs: {} {} vs {} {}", accDesc,
                   name1, acc1.ext.v(), name2, acc2.ext.v());
    }
    else if (acc1.ext.v() == 1)
    {
        auto const& v1_1 = acc1.ext.v1();
        auto const& v1_2 = acc2.ext.v1();

        compareValue(name1, name2, v1_1.liabilities.buying,
                     v1_2.liabilities.buying, accDesc + " liabilities.buying");
        compareValue(name1, name2, v1_1.liabilities.selling,
                     v1_2.liabilities.selling,
                     accDesc + " liabilities.selling");

        // Check nested extension version matches
        if (v1_1.ext.v() != v1_2.ext.v())
        {
            CLOG_ERROR(Ledger, "{} ext.v1.ext version differs: {} {} vs {} {}",
                       accDesc, name1, v1_1.ext.v(), name2, v1_2.ext.v());
        }
        else if (v1_1.ext.v() == 2)
        {
            auto const& v2_1 = v1_1.ext.v2();
            auto const& v2_2 = v1_2.ext.v2();

            compareValue(name1, name2, v2_1.numSponsored, v2_2.numSponsored,
                         accDesc + " numSponsored");
            compareValue(name1, name2, v2_1.numSponsoring, v2_2.numSponsoring,
                         accDesc + " numSponsoring");

            compareVector(
                name1, name2, v2_1.signerSponsoringIDs,
                v2_2.signerSponsoringIDs, accDesc + " signerSponsoringIDs",
                [&](xdr::pointer<AccountID> const& id1,
                    xdr::pointer<AccountID> const& id2, size_t i) {
                    compareOptional(
                        name1, name2, id1, id2,
                        fmt::format("{} signerSponsoringID[{}]", accDesc, i));
                });

            // Check nested extension version matches
            if (v2_1.ext.v() != v2_2.ext.v())
            {
                CLOG_ERROR(
                    Ledger,
                    "{} ext.v1.ext.v2.ext version differs: {} {} vs {} {}",
                    accDesc, name1, v2_1.ext.v(), name2, v2_2.ext.v());
            }
            else if (v2_1.ext.v() == 3)
            {
                auto const& v3_1 = v2_1.ext.v3();
                auto const& v3_2 = v2_2.ext.v3();

                compareValue(name1, name2, v3_1.seqLedger, v3_2.seqLedger,
                             accDesc + " seqLedger");
                compareValue(name1, name2, v3_1.seqTime, v3_2.seqTime,
                             accDesc + " seqTime");
            }
        }
    }
}

void
compareTrustLineEntry(std::string const& name1, std::string const& name2,
                      TrustLineEntry const& tl1, TrustLineEntry const& tl2,
                      std::string const& tlDesc)
{
    if (!(tl1.accountID == tl2.accountID))
    {
        CLOG_ERROR(Ledger, "{} accountID differs", tlDesc);
    }

    compareTrustLineAsset(name1, name2, tl1.asset, tl2.asset,
                          tlDesc + " asset");

    compareValue(name1, name2, tl1.balance, tl2.balance, tlDesc + " balance");
    compareValue(name1, name2, tl1.limit, tl2.limit, tlDesc + " limit");
    compareValue(name1, name2, tl1.flags, tl2.flags, tlDesc + " flags");

    // Compare trustline extension
    if (tl1.ext.v() != tl2.ext.v())
    {
        CLOG_ERROR(Ledger, "{} ext version differs: {} {} vs {} {}", tlDesc,
                   name1, tl1.ext.v(), name2, tl2.ext.v());
    }
    else if (tl1.ext.v() == 1)
    {
        auto const& v1_1 = tl1.ext.v1();
        auto const& v1_2 = tl2.ext.v1();

        compareValue(name1, name2, v1_1.liabilities.buying,
                     v1_2.liabilities.buying, tlDesc + " liabilities.buying");
        compareValue(name1, name2, v1_1.liabilities.selling,
                     v1_2.liabilities.selling, tlDesc + " liabilities.selling");

        // Check nested extension version matches
        if (v1_1.ext.v() != v1_2.ext.v())
        {
            CLOG_ERROR(Ledger, "{} ext.v1.ext version differs: {} {} vs {} {}",
                       tlDesc, name1, v1_1.ext.v(), name2, v1_2.ext.v());
        }
        else if (v1_1.ext.v() == 2)
        {
            compareValue(name1, name2, v1_1.ext.v2().liquidityPoolUseCount,
                         v1_2.ext.v2().liquidityPoolUseCount,
                         tlDesc + " liquidityPoolUseCount");
        }
    }
}

void
compareOfferEntry(std::string const& name1, std::string const& name2,
                  OfferEntry const& offer1, OfferEntry const& offer2,
                  std::string const& offerDesc)
{
    if (!(offer1.sellerID == offer2.sellerID))
    {
        CLOG_ERROR(Ledger, "{} sellerID differs", offerDesc);
    }

    compareValue(name1, name2, offer1.offerID, offer2.offerID,
                 offerDesc + " offerID");

    compareAsset(name1, name2, offer1.selling, offer2.selling,
                 offerDesc + " selling");
    compareAsset(name1, name2, offer1.buying, offer2.buying,
                 offerDesc + " buying");

    compareValue(name1, name2, offer1.amount, offer2.amount,
                 offerDesc + " amount");

    if (!(offer1.price == offer2.price))
    {
        CLOG_ERROR(Ledger, "{} price differs: {} {}/{} vs {} {}/{}", offerDesc,
                   name1, offer1.price.n, offer1.price.d, name2, offer2.price.n,
                   offer2.price.d);
    }

    compareValue(name1, name2, offer1.flags, offer2.flags,
                 offerDesc + " flags");
}

void
compareDataEntry(std::string const& name1, std::string const& name2,
                 DataEntry const& data1, DataEntry const& data2,
                 std::string const& dataDesc)
{
    if (!(data1.accountID == data2.accountID))
    {
        CLOG_ERROR(Ledger, "{} accountID differs", dataDesc);
    }

    if (data1.dataName != data2.dataName)
    {
        CLOG_ERROR(Ledger, "{} dataName differs: {} '{}' vs {} '{}'", dataDesc,
                   name1, data1.dataName, name2, data2.dataName);
    }

    if (data1.dataValue != data2.dataValue)
    {
        CLOG_ERROR(Ledger, "{} dataValue differs: {} size {} vs {} size {}",
                   dataDesc, name1, data1.dataValue.size(), name2,
                   data2.dataValue.size());
    }
}

void
compareClaimableBalanceEntry(std::string const& name1, std::string const& name2,
                             ClaimableBalanceEntry const& cb1,
                             ClaimableBalanceEntry const& cb2,
                             std::string const& cbDesc)
{
    if (!(cb1.balanceID == cb2.balanceID))
    {
        CLOG_ERROR(Ledger, "{} balanceID differs", cbDesc);
    }

    compareVector(
        name1, name2, cb1.claimants, cb2.claimants, cbDesc + " claimants",
        [&](Claimant const& c1, Claimant const& c2, size_t i) {
            if (!(c1.v0().destination == c2.v0().destination))
            {
                CLOG_ERROR(Ledger, "{} claimant[{}] destination differs",
                           cbDesc, i);
            }
            if (!(c1.v0().predicate == c2.v0().predicate))
            {
                CLOG_ERROR(Ledger, "{} claimant[{}] predicate differs", cbDesc,
                           i);
            }
        });

    compareAsset(name1, name2, cb1.asset, cb2.asset, cbDesc + " asset");
    compareValue(name1, name2, cb1.amount, cb2.amount, cbDesc + " amount");
}

void
compareLiquidityPoolEntry(std::string const& name1, std::string const& name2,
                          LiquidityPoolEntry const& lp1,
                          LiquidityPoolEntry const& lp2,
                          std::string const& lpDesc)
{
    if (!(lp1.liquidityPoolID == lp2.liquidityPoolID))
    {
        CLOG_ERROR(Ledger, "{} liquidityPoolID differs", lpDesc);
    }

    if (lp1.body.type() != lp2.body.type())
    {
        CLOG_ERROR(Ledger, "{} body type differs: {} {} vs {} {}", lpDesc,
                   name1, static_cast<int>(lp1.body.type()), name2,
                   static_cast<int>(lp2.body.type()));
        return;
    }

    if (lp1.body.type() == LIQUIDITY_POOL_CONSTANT_PRODUCT)
    {
        auto const& cp1 = lp1.body.constantProduct();
        auto const& cp2 = lp2.body.constantProduct();

        compareAsset(name1, name2, cp1.params.assetA, cp2.params.assetA,
                     lpDesc + " assetA");
        compareAsset(name1, name2, cp1.params.assetB, cp2.params.assetB,
                     lpDesc + " assetB");
        compareValue(name1, name2, cp1.params.fee, cp2.params.fee,
                     lpDesc + " fee");

        compareValue(name1, name2, cp1.reserveA, cp2.reserveA,
                     lpDesc + " reserveA");
        compareValue(name1, name2, cp1.reserveB, cp2.reserveB,
                     lpDesc + " reserveB");
        compareValue(name1, name2, cp1.totalPoolShares, cp2.totalPoolShares,
                     lpDesc + " totalPoolShares");
        compareValue(name1, name2, cp1.poolSharesTrustLineCount,
                     cp2.poolSharesTrustLineCount,
                     lpDesc + " poolSharesTrustLineCount");
    }
}

void
compareConfigSettingEntry(std::string const& name1, std::string const& name2,
                          ConfigSettingEntry const& cs1,
                          ConfigSettingEntry const& cs2,
                          std::string const& csDesc)
{
    if (cs1.configSettingID() != cs2.configSettingID())
    {
        CLOG_ERROR(Ledger, "{} configSettingID differs: {} {} vs {} {}", csDesc,
                   name1, static_cast<int>(cs1.configSettingID()), name2,
                   static_cast<int>(cs2.configSettingID()));
        return;
    }

    // Compare based on config setting type
    switch (cs1.configSettingID())
    {
    case CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES:
        compareValue(name1, name2, cs1.contractMaxSizeBytes(),
                     cs2.contractMaxSizeBytes(),
                     csDesc + " contractMaxSizeBytes");
        break;

    case CONFIG_SETTING_CONTRACT_COMPUTE_V0:
    {
        auto const& cv1 = cs1.contractCompute();
        auto const& cv2 = cs2.contractCompute();

        compareValue(name1, name2, cv1.ledgerMaxInstructions,
                     cv2.ledgerMaxInstructions,
                     csDesc + " ledgerMaxInstructions");
        compareValue(name1, name2, cv1.txMaxInstructions, cv2.txMaxInstructions,
                     csDesc + " txMaxInstructions");
        compareValue(name1, name2, cv1.feeRatePerInstructionsIncrement,
                     cv2.feeRatePerInstructionsIncrement,
                     csDesc + " feeRatePerInstructionsIncrement");
        compareValue(name1, name2, cv1.txMemoryLimit, cv2.txMemoryLimit,
                     csDesc + " txMemoryLimit");
    }
    break;

    case CONFIG_SETTING_CONTRACT_LEDGER_COST_V0:
    {
        auto const& lc1 = cs1.contractLedgerCost();
        auto const& lc2 = cs2.contractLedgerCost();

        compareValue(name1, name2, lc1.ledgerMaxDiskReadEntries,
                     lc2.ledgerMaxDiskReadEntries,
                     csDesc + " ledgerMaxDiskReadEntries");
        compareValue(name1, name2, lc1.ledgerMaxDiskReadBytes,
                     lc2.ledgerMaxDiskReadBytes,
                     csDesc + " ledgerMaxDiskReadBytes");
        compareValue(name1, name2, lc1.ledgerMaxWriteLedgerEntries,
                     lc2.ledgerMaxWriteLedgerEntries,
                     csDesc + " ledgerMaxWriteLedgerEntries");
        compareValue(name1, name2, lc1.ledgerMaxWriteBytes,
                     lc2.ledgerMaxWriteBytes, csDesc + " ledgerMaxWriteBytes");
        compareValue(name1, name2, lc1.txMaxDiskReadEntries,
                     lc2.txMaxDiskReadEntries,
                     csDesc + " txMaxDiskReadEntries");
        compareValue(name1, name2, lc1.txMaxDiskReadBytes,
                     lc2.txMaxDiskReadBytes, csDesc + " txMaxDiskReadBytes");
        compareValue(name1, name2, lc1.txMaxWriteLedgerEntries,
                     lc2.txMaxWriteLedgerEntries,
                     csDesc + " txMaxWriteLedgerEntries");
        compareValue(name1, name2, lc1.txMaxWriteBytes, lc2.txMaxWriteBytes,
                     csDesc + " txMaxWriteBytes");
        compareValue(name1, name2, lc1.feeDiskReadLedgerEntry,
                     lc2.feeDiskReadLedgerEntry,
                     csDesc + " feeDiskReadLedgerEntry");
        compareValue(name1, name2, lc1.feeWriteLedgerEntry,
                     lc2.feeWriteLedgerEntry, csDesc + " feeWriteLedgerEntry");
        compareValue(name1, name2, lc1.feeDiskRead1KB, lc2.feeDiskRead1KB,
                     csDesc + " feeDiskRead1KB");
        compareValue(name1, name2, lc1.sorobanStateTargetSizeBytes,
                     lc2.sorobanStateTargetSizeBytes,
                     csDesc + " sorobanStateTargetSizeBytes");
        compareValue(name1, name2, lc1.rentFee1KBSorobanStateSizeLow,
                     lc2.rentFee1KBSorobanStateSizeLow,
                     csDesc + " rentFee1KBSorobanStateSizeLow");
        compareValue(name1, name2, lc1.rentFee1KBSorobanStateSizeHigh,
                     lc2.rentFee1KBSorobanStateSizeHigh,
                     csDesc + " rentFee1KBSorobanStateSizeHigh");
        compareValue(name1, name2, lc1.sorobanStateRentFeeGrowthFactor,
                     lc2.sorobanStateRentFeeGrowthFactor,
                     csDesc + " sorobanStateRentFeeGrowthFactor");
    }
    break;

    case CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0:
    {
        auto const& hd1 = cs1.contractHistoricalData();
        auto const& hd2 = cs2.contractHistoricalData();

        compareValue(name1, name2, hd1.feeHistorical1KB, hd2.feeHistorical1KB,
                     csDesc + " feeHistorical1KB");
    }
    break;

    case CONFIG_SETTING_CONTRACT_EVENTS_V0:
    {
        auto const& ev1 = cs1.contractEvents();
        auto const& ev2 = cs2.contractEvents();

        compareValue(name1, name2, ev1.txMaxContractEventsSizeBytes,
                     ev2.txMaxContractEventsSizeBytes,
                     csDesc + " txMaxContractEventsSizeBytes");
        compareValue(name1, name2, ev1.feeContractEvents1KB,
                     ev2.feeContractEvents1KB,
                     csDesc + " feeContractEvents1KB");
    }
    break;

    case CONFIG_SETTING_CONTRACT_BANDWIDTH_V0:
    {
        auto const& bw1 = cs1.contractBandwidth();
        auto const& bw2 = cs2.contractBandwidth();

        compareValue(name1, name2, bw1.ledgerMaxTxsSizeBytes,
                     bw2.ledgerMaxTxsSizeBytes,
                     csDesc + " ledgerMaxTxsSizeBytes");
        compareValue(name1, name2, bw1.txMaxSizeBytes, bw2.txMaxSizeBytes,
                     csDesc + " txMaxSizeBytes");
        compareValue(name1, name2, bw1.feeTxSize1KB, bw2.feeTxSize1KB,
                     csDesc + " feeTxSize1KB");
    }
    break;

    case CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS:
    {
        auto const& cpu1 = cs1.contractCostParamsCpuInsns();
        auto const& cpu2 = cs2.contractCostParamsCpuInsns();

        if (cpu1.size() != cpu2.size())
        {
            CLOG_ERROR(Ledger, "{} cpu params size differs: {} {} vs {} {}",
                       csDesc, name1, cpu1.size(), name2, cpu2.size());
        }
        else
        {
            for (size_t i = 0; i < cpu1.size(); ++i)
            {
                compareValue(name1, name2, cpu1[i].constTerm, cpu2[i].constTerm,
                             fmt::format("{} cpu[{}] constTerm", csDesc, i));
                compareValue(name1, name2, cpu1[i].linearTerm,
                             cpu2[i].linearTerm,
                             fmt::format("{} cpu[{}] linearTerm", csDesc, i));
            }
        }
    }
    break;

    case CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES:
    {
        auto const& mem1 = cs1.contractCostParamsMemBytes();
        auto const& mem2 = cs2.contractCostParamsMemBytes();

        if (mem1.size() != mem2.size())
        {
            CLOG_ERROR(Ledger, "{} memory params size differs: {} {} vs {} {}",
                       csDesc, name1, mem1.size(), name2, mem2.size());
        }
        else
        {
            for (size_t i = 0; i < mem1.size(); ++i)
            {
                compareValue(name1, name2, mem1[i].constTerm, mem2[i].constTerm,
                             fmt::format("{} mem[{}] constTerm", csDesc, i));
                compareValue(name1, name2, mem1[i].linearTerm,
                             mem2[i].linearTerm,
                             fmt::format("{} mem[{}] linearTerm", csDesc, i));
            }
        }
    }
    break;

    case CONFIG_SETTING_CONTRACT_DATA_KEY_SIZE_BYTES:
        compareValue(name1, name2, cs1.contractDataKeySizeBytes(),
                     cs2.contractDataKeySizeBytes(),
                     csDesc + " contractDataKeySizeBytes");
        break;

    case CONFIG_SETTING_CONTRACT_DATA_ENTRY_SIZE_BYTES:
        compareValue(name1, name2, cs1.contractDataEntrySizeBytes(),
                     cs2.contractDataEntrySizeBytes(),
                     csDesc + " contractDataEntrySizeBytes");
        break;

    case CONFIG_SETTING_STATE_ARCHIVAL:
    {
        auto const& sa1 = cs1.stateArchivalSettings();
        auto const& sa2 = cs2.stateArchivalSettings();

        compareValue(name1, name2, sa1.maxEntryTTL, sa2.maxEntryTTL,
                     csDesc + " maxEntryTTL");
        compareValue(name1, name2, sa1.minTemporaryTTL, sa2.minTemporaryTTL,
                     csDesc + " minTemporaryTTL");
        compareValue(name1, name2, sa1.minPersistentTTL, sa2.minPersistentTTL,
                     csDesc + " minPersistentTTL");
        compareValue(name1, name2, sa1.persistentRentRateDenominator,
                     sa2.persistentRentRateDenominator,
                     csDesc + " persistentRentRateDenominator");
        compareValue(name1, name2, sa1.tempRentRateDenominator,
                     sa2.tempRentRateDenominator,
                     csDesc + " tempRentRateDenominator");
        compareValue(name1, name2, sa1.maxEntriesToArchive,
                     sa2.maxEntriesToArchive, csDesc + " maxEntriesToArchive");
        compareValue(name1, name2, sa1.liveSorobanStateSizeWindowSampleSize,
                     sa2.liveSorobanStateSizeWindowSampleSize,
                     csDesc + " liveSorobanStateSizeWindowSampleSize");
        compareValue(name1, name2, sa1.liveSorobanStateSizeWindowSamplePeriod,
                     sa2.liveSorobanStateSizeWindowSamplePeriod,
                     csDesc + " liveSorobanStateSizeWindowSamplePeriod");
        compareValue(name1, name2, sa1.evictionScanSize, sa2.evictionScanSize,
                     csDesc + " evictionScanSize");
        compareValue(name1, name2, sa1.startingEvictionScanLevel,
                     sa2.startingEvictionScanLevel,
                     csDesc + " startingEvictionScanLevel");
    }
    break;

    case CONFIG_SETTING_CONTRACT_EXECUTION_LANES:
    {
        auto const& el1 = cs1.contractExecutionLanes();
        auto const& el2 = cs2.contractExecutionLanes();

        compareValue(name1, name2, el1.ledgerMaxTxCount, el2.ledgerMaxTxCount,
                     csDesc + " ledgerMaxTxCount");
    }
    break;

    case CONFIG_SETTING_LIVE_SOROBAN_STATE_SIZE_WINDOW:
    {
        auto const& bw1 = cs1.liveSorobanStateSizeWindow();
        auto const& bw2 = cs2.liveSorobanStateSizeWindow();

        if (bw1.size() != bw2.size())
        {
            CLOG_ERROR(Ledger,
                       "{} bucketListSizeWindow size differs: {} {} vs {} {}",
                       csDesc, name1, bw1.size(), name2, bw2.size());
        }
        else
        {
            for (size_t i = 0; i < bw1.size(); ++i)
            {
                compareValue(
                    name1, name2, bw1[i], bw2[i],
                    fmt::format("{} bucketListSizeWindow[{}]", csDesc, i));
            }
        }
    }
    break;

    case CONFIG_SETTING_EVICTION_ITERATOR:
    {
        auto const& ei1 = cs1.evictionIterator();
        auto const& ei2 = cs2.evictionIterator();

        compareValue(name1, name2, ei1.bucketListLevel, ei2.bucketListLevel,
                     csDesc + " bucketListLevel");
        compareValue(name1, name2, ei1.isCurrBucket, ei2.isCurrBucket,
                     csDesc + " isCurrBucket");
        compareValue(name1, name2, ei1.bucketFileOffset, ei2.bucketFileOffset,
                     csDesc + " bucketFileOffset");
    }
    break;

    default:
        CLOG_ERROR(Ledger, "{} unknown config setting type: {}", csDesc,
                   static_cast<int>(cs1.configSettingID()));
        break;
    }
}

void
compareContractDataEntry(std::string const& name1, std::string const& name2,
                         ContractDataEntry const& cd1,
                         ContractDataEntry const& cd2,
                         std::string const& cdDesc)
{
    compareValue(name1, name2, cd1.ext.v(), cd2.ext.v(),
                 cdDesc + " ext version");

    if (!(cd1.contract == cd2.contract))
    {
        CLOG_ERROR(Ledger, "{} contract differs", cdDesc);
    }

    compareSCVal(name1, name2, cd1.key, cd2.key, cdDesc + " key");

    compareValue(name1, name2, static_cast<int>(cd1.durability),
                 static_cast<int>(cd2.durability), cdDesc + " durability");

    compareSCVal(name1, name2, cd1.val, cd2.val, cdDesc + " val");
}

void
compareContractCodeEntry(std::string const& name1, std::string const& name2,
                         ContractCodeEntry const& cc1,
                         ContractCodeEntry const& cc2,
                         std::string const& ccDesc)
{
    compareValue(name1, name2, cc1.ext.v(), cc2.ext.v(),
                 ccDesc + " ext version");

    if (!(cc1.hash == cc2.hash))
    {
        CLOG_ERROR(Ledger, "{} hash differs", ccDesc);
    }

    if (cc1.code != cc2.code)
    {
        CLOG_ERROR(Ledger, "{} code differs: {} size {} vs {} size {}", ccDesc,
                   name1, cc1.code.size(), name2, cc2.code.size());
    }
}

void
compareTTLEntry(std::string const& name1, std::string const& name2,
                TTLEntry const& ttl1, TTLEntry const& ttl2,
                std::string const& ttlDesc)
{
    if (!(ttl1.keyHash == ttl2.keyHash))
    {
        CLOG_ERROR(Ledger, "{} keyHash differs", ttlDesc);
    }

    compareValue(name1, name2, ttl1.liveUntilLedgerSeq, ttl2.liveUntilLedgerSeq,
                 ttlDesc + " liveUntilLedgerSeq");
}

void
compareLedgerEntryChanges(std::string const& name1, std::string const& name2,
                          LedgerEntryChanges const& changes1,
                          LedgerEntryChanges const& changes2,
                          std::string const& changesDesc)
{
    compareVector(name1, name2, changes1, changes2, changesDesc,
                  [&](LedgerEntryChange const& c1, LedgerEntryChange const& c2,
                      size_t i) {
                      compareLedgerEntryChange(
                          name1, name2, c1, c2,
                          fmt::format("{} [{}]", changesDesc, i));
                  });
}

void
compareLedgerEntryChange(std::string const& name1, std::string const& name2,
                         LedgerEntryChange const& change1,
                         LedgerEntryChange const& change2,
                         std::string const& changeDesc)
{
    if (change1.type() != change2.type())
    {
        CLOG_ERROR(Ledger, "{} type differs: {} {} vs {} {}", changeDesc, name1,
                   static_cast<int>(change1.type()), name2,
                   static_cast<int>(change2.type()));
        return;
    }

    switch (change1.type())
    {
    case LEDGER_ENTRY_CREATED:
        compareLedgerEntry(name1, name2, change1.created(), change2.created(),
                           changeDesc + " created");
        break;

    case LEDGER_ENTRY_UPDATED:
        compareLedgerEntry(name1, name2, change1.updated(), change2.updated(),
                           changeDesc + " updated");
        break;

    case LEDGER_ENTRY_REMOVED:
    {
        auto const& key1 = change1.removed();
        auto const& key2 = change2.removed();

        if (key1.type() != key2.type())
        {
            CLOG_ERROR(Ledger, "{} removed key type differs: {} {} vs {} {}",
                       changeDesc, name1, static_cast<int>(key1.type()), name2,
                       static_cast<int>(key2.type()));
        }
        else if (!(key1 == key2))
        {
            CLOG_ERROR(Ledger, "{} removed key differs", changeDesc);
        }
    }
    break;

    case LEDGER_ENTRY_STATE:
        compareLedgerEntry(name1, name2, change1.state(), change2.state(),
                           changeDesc + " state");
        break;

    default:
        CLOG_ERROR(Ledger, "{} unknown change type: {}", changeDesc,
                   static_cast<int>(change1.type()));
        break;
    }
}

void
compareTrustLineAsset(std::string const& name1, std::string const& name2,
                      TrustLineAsset const& asset1,
                      TrustLineAsset const& asset2,
                      std::string const& assetDesc)
{
    if (asset1.type() != asset2.type())
    {
        CLOG_ERROR(Ledger, "{} type differs: {} {} vs {} {}", assetDesc, name1,
                   static_cast<int>(asset1.type()), name2,
                   static_cast<int>(asset2.type()));
        return;
    }

    switch (asset1.type())
    {
    case ASSET_TYPE_NATIVE:
        // Nothing to compare for native assets in trustlines
        break;

    case ASSET_TYPE_CREDIT_ALPHANUM4:
        if (asset1.alphaNum4().assetCode != asset2.alphaNum4().assetCode)
        {
            CLOG_ERROR(Ledger, "{} alphaNum4 code differs: {} '{}' vs {} '{}'",
                       assetDesc, name1,
                       std::string(asset1.alphaNum4().assetCode.begin(),
                                   asset1.alphaNum4().assetCode.end()),
                       name2,
                       std::string(asset2.alphaNum4().assetCode.begin(),
                                   asset2.alphaNum4().assetCode.end()));
        }
        if (!(asset1.alphaNum4().issuer == asset2.alphaNum4().issuer))
        {
            CLOG_ERROR(Ledger, "{} alphaNum4 issuer differs", assetDesc);
        }
        break;

    case ASSET_TYPE_CREDIT_ALPHANUM12:
        if (asset1.alphaNum12().assetCode != asset2.alphaNum12().assetCode)
        {
            CLOG_ERROR(Ledger, "{} alphaNum12 code differs: {} '{}' vs {} '{}'",
                       assetDesc, name1,
                       std::string(asset1.alphaNum12().assetCode.begin(),
                                   asset1.alphaNum12().assetCode.end()),
                       name2,
                       std::string(asset2.alphaNum12().assetCode.begin(),
                                   asset2.alphaNum12().assetCode.end()));
        }
        if (!(asset1.alphaNum12().issuer == asset2.alphaNum12().issuer))
        {
            CLOG_ERROR(Ledger, "{} alphaNum12 issuer differs", assetDesc);
        }
        break;

    case ASSET_TYPE_POOL_SHARE:
        // Pool share assets don't exist in Asset type
        // This case should never be reached for Asset
        break;

    default:
        CLOG_ERROR(Ledger, "{} unknown asset type: {}", assetDesc,
                   static_cast<int>(asset1.type()));
        break;
    }
}

void
compareAsset(std::string const& name1, std::string const& name2,
             Asset const& asset1, Asset const& asset2,
             std::string const& assetDesc)
{
    if (asset1.type() != asset2.type())
    {
        CLOG_ERROR(Ledger, "{} type differs: {} {} vs {} {}", assetDesc, name1,
                   static_cast<int>(asset1.type()), name2,
                   static_cast<int>(asset2.type()));
        return;
    }

    switch (asset1.type())
    {
    case ASSET_TYPE_NATIVE:
        // Nothing to compare
        break;

    case ASSET_TYPE_CREDIT_ALPHANUM4:
        if (asset1.alphaNum4().assetCode != asset2.alphaNum4().assetCode)
        {
            CLOG_ERROR(Ledger, "{} alphaNum4 code differs: {} '{}' vs {} '{}'",
                       assetDesc, name1,
                       std::string(asset1.alphaNum4().assetCode.begin(),
                                   asset1.alphaNum4().assetCode.end()),
                       name2,
                       std::string(asset2.alphaNum4().assetCode.begin(),
                                   asset2.alphaNum4().assetCode.end()));
        }
        if (!(asset1.alphaNum4().issuer == asset2.alphaNum4().issuer))
        {
            CLOG_ERROR(Ledger, "{} alphaNum4 issuer differs", assetDesc);
        }
        break;

    case ASSET_TYPE_CREDIT_ALPHANUM12:
        if (asset1.alphaNum12().assetCode != asset2.alphaNum12().assetCode)
        {
            CLOG_ERROR(Ledger, "{} alphaNum12 code differs: {} '{}' vs {} '{}'",
                       assetDesc, name1,
                       std::string(asset1.alphaNum12().assetCode.begin(),
                                   asset1.alphaNum12().assetCode.end()),
                       name2,
                       std::string(asset2.alphaNum12().assetCode.begin(),
                                   asset2.alphaNum12().assetCode.end()));
        }
        if (!(asset1.alphaNum12().issuer == asset2.alphaNum12().issuer))
        {
            CLOG_ERROR(Ledger, "{} alphaNum12 issuer differs", assetDesc);
        }
        break;

    default:
        CLOG_ERROR(Ledger, "{} unknown asset type: {}", assetDesc,
                   static_cast<int>(asset1.type()));
        break;
    }
}

void
compareTransactionMeta(std::string const& name1, std::string const& name2,
                       TransactionMeta const& meta1,
                       TransactionMeta const& meta2, size_t txIndex)
{
    if (meta1.v() != meta2.v())
    {
        CLOG_ERROR(Ledger, "tx meta {} version differs: {} {} vs {} {}",
                   txIndex, name1, meta1.v(), name2, meta2.v());
        return;
    }

    switch (meta1.v())
    {
    case 0:
        compareVector(
            name1, name2, meta1.operations(), meta2.operations(),
            fmt::format("tx meta {} v0 operation", txIndex),
            [&](OperationMeta const& op1, OperationMeta const& op2, size_t i) {
                if (!(op1 == op2))
                {
                    compareLedgerEntryChanges(
                        name1, name2, op1.changes, op2.changes,
                        fmt::format("tx {} v0.operations[{}]", txIndex, i));
                }
            });
        break;

    case 1:
    {
        auto const& v1_1 = meta1.v1();
        auto const& v1_2 = meta2.v1();

        compareLedgerEntryChanges(name1, name2, v1_1.txChanges, v1_2.txChanges,
                                  fmt::format("tx {} v1.txChanges", txIndex));

        compareVector(
            name1, name2, v1_1.operations, v1_2.operations,
            fmt::format("tx meta {} v1 operation", txIndex),
            [&](OperationMeta const& op1, OperationMeta const& op2, size_t i) {
                if (!(op1 == op2))
                {
                    compareLedgerEntryChanges(
                        name1, name2, op1.changes, op2.changes,
                        fmt::format("tx {} v1.operations[{}]", txIndex, i));
                }
            });
    }
    break;

    case 2:
    {
        auto const& v2_1 = meta1.v2();
        auto const& v2_2 = meta2.v2();

        compareLedgerEntryChanges(
            name1, name2, v2_1.txChangesBefore, v2_2.txChangesBefore,
            fmt::format("tx {} v2.txChangesBefore", txIndex));

        compareVector(
            name1, name2, v2_1.operations, v2_2.operations,
            fmt::format("tx meta {} v2 operation", txIndex),
            [&](OperationMeta const& op1, OperationMeta const& op2, size_t i) {
                if (!(op1 == op2))
                {
                    compareLedgerEntryChanges(
                        name1, name2, op1.changes, op2.changes,
                        fmt::format("tx {} v2.operations[{}]", txIndex, i));
                }
            });

        compareLedgerEntryChanges(
            name1, name2, v2_1.txChangesAfter, v2_2.txChangesAfter,
            fmt::format("tx {} v2.txChangesAfter", txIndex));
    }
    break;

    case 3:
    {
        auto const& v3_1 = meta1.v3();
        auto const& v3_2 = meta2.v3();

        // Compare extension
        if (v3_1.ext.v() != v3_2.ext.v())
        {
            CLOG_ERROR(Ledger, "tx {} v3 ext version differs: {} {} vs {} {}",
                       txIndex, name1, v3_1.ext.v(), name2, v3_2.ext.v());
        }
        // TransactionMetaV3 has ExtensionPoint, not a versioned extension

        // Check Soroban meta
        if (v3_1.sorobanMeta && v3_2.sorobanMeta)
        {
            auto const& sm1 = *(v3_1.sorobanMeta);
            auto const& sm2 = *(v3_2.sorobanMeta);

            // Compare extension for SorobanTransactionMeta
            if (sm1.ext.v() != sm2.ext.v())
            {
                CLOG_ERROR(
                    Ledger,
                    "tx {} soroban meta ext version differs: {} {} vs {} {}",
                    txIndex, name1, sm1.ext.v(), name2, sm2.ext.v());
            }
            else if (sm1.ext.v() == 1)
            {
                auto const& v1ext1 = sm1.ext.v1();
                auto const& v1ext2 = sm2.ext.v1();

                compareValue(
                    name1, name2, v1ext1.totalNonRefundableResourceFeeCharged,
                    v1ext2.totalNonRefundableResourceFeeCharged,
                    fmt::format("tx {} soroban non-refundable fee", txIndex));

                compareValue(
                    name1, name2, v1ext1.totalRefundableResourceFeeCharged,
                    v1ext2.totalRefundableResourceFeeCharged,
                    fmt::format("tx {} soroban refundable fee", txIndex));

                compareValue(name1, name2, v1ext1.rentFeeCharged,
                             v1ext2.rentFeeCharged,
                             fmt::format("tx {} soroban rent fee", txIndex));
            }

            // Compare events
            compareVector(
                name1, name2, sm1.events, sm2.events,
                fmt::format("tx {} soroban event", txIndex),
                [&](ContractEvent const& event1, ContractEvent const& event2,
                    size_t i) {
                    compareContractEvent(
                        name1, name2, event1, event2,
                        fmt::format("tx {} soroban event[{}]", txIndex, i));
                });

            // Compare return value
            if (!(sm1.returnValue == sm2.returnValue))
            {
                CLOG_ERROR(Ledger, "tx {} soroban return value differs",
                           txIndex);
            }

            // Compare diagnostic events
            compareVector(
                name1, name2, sm1.diagnosticEvents, sm2.diagnosticEvents,
                fmt::format("tx {} soroban diagnostic event", txIndex),
                [&](DiagnosticEvent const& diag1, DiagnosticEvent const& diag2,
                    size_t i) {
                    compareValue(
                        name1, name2, diag1.inSuccessfulContractCall,
                        diag2.inSuccessfulContractCall,
                        fmt::format(
                            "tx {} soroban diagnostic event[{}] success flag",
                            txIndex, i));

                    compareContractEvent(
                        name1, name2, diag1.event, diag2.event,
                        fmt::format("tx {} soroban diagnostic event[{}]",
                                    txIndex, i));
                });
        }
        else if (v3_1.sorobanMeta || v3_2.sorobanMeta)
        {
            CLOG_ERROR(Ledger,
                       "tx {} soroban meta presence differs: {} {} vs {} {}",
                       txIndex, name1, static_cast<bool>(v3_1.sorobanMeta),
                       name2, static_cast<bool>(v3_2.sorobanMeta));
        }

        compareLedgerEntryChanges(
            name1, name2, v3_1.txChangesBefore, v3_2.txChangesBefore,
            fmt::format("tx {} v3.txChangesBefore", txIndex));

        compareVector(
            name1, name2, v3_1.operations, v3_2.operations,
            fmt::format("tx meta {} v3 operation", txIndex),
            [&](OperationMeta const& op1, OperationMeta const& op2, size_t i) {
                if (!(op1 == op2))
                {
                    compareLedgerEntryChanges(
                        name1, name2, op1.changes, op2.changes,
                        fmt::format("tx {} v3.operations[{}]", txIndex, i));
                }
            });

        compareLedgerEntryChanges(
            name1, name2, v3_1.txChangesAfter, v3_2.txChangesAfter,
            fmt::format("tx {} v3.txChangesAfter", txIndex));
    }
    break;

    case 4:
    {
        auto const& v4_1 = meta1.v4();
        auto const& v4_2 = meta2.v4();

        compareLedgerEntryChanges(
            name1, name2, v4_1.txChangesBefore, v4_2.txChangesBefore,
            fmt::format("tx {} v4.txChangesBefore", txIndex));

        compareVector(
            name1, name2, v4_1.operations, v4_2.operations,
            fmt::format("tx meta {} v4 operation", txIndex),
            [&](OperationMetaV2 const& op1, OperationMetaV2 const& op2,
                size_t i) {
                if (!(op1 == op2))
                {
                    compareLedgerEntryChanges(
                        name1, name2, op1.changes, op2.changes,
                        fmt::format("tx {} v4.operations[{}]", txIndex, i));
                }
            });

        compareLedgerEntryChanges(
            name1, name2, v4_1.txChangesAfter, v4_2.txChangesAfter,
            fmt::format("tx {} v4.txChangesAfter", txIndex));

        // Check Soroban meta presence
        if (v4_1.sorobanMeta && v4_2.sorobanMeta)
        {
            auto const& sm1 = *(v4_1.sorobanMeta);
            auto const& sm2 = *(v4_2.sorobanMeta);

            auto const& ext1 = sm1.ext;
            auto const& ext2 = sm2.ext;

            if (ext1.v() == 1 && ext2.v() == 1)
            {
                auto const& v1ext1 = ext1.v1();
                auto const& v1ext2 = ext2.v1();

                compareValue(
                    name1, name2, v1ext1.totalNonRefundableResourceFeeCharged,
                    v1ext2.totalNonRefundableResourceFeeCharged,
                    fmt::format("tx {} soroban non-refundable fee", txIndex));

                compareValue(
                    name1, name2, v1ext1.totalRefundableResourceFeeCharged,
                    v1ext2.totalRefundableResourceFeeCharged,
                    fmt::format("tx {} soroban refundable fee", txIndex));

                compareValue(name1, name2, v1ext1.rentFeeCharged,
                             v1ext2.rentFeeCharged,
                             fmt::format("tx {} soroban rent fee", txIndex));
            }

            // Compare optional return value
            compareOptional(name1, name2, sm1.returnValue, sm2.returnValue,
                            fmt::format("tx {} soroban return value", txIndex));

            // Compare events
            compareVector(
                name1, name2, v4_1.events, v4_2.events,
                fmt::format("tx {} transaction event", txIndex),
                [&](TransactionEvent const& event1,
                    TransactionEvent const& event2, size_t i) {
                    compareValue(
                        name1, name2, static_cast<int>(event1.stage),
                        static_cast<int>(event2.stage),
                        fmt::format("tx {} transaction event[{}] stage",
                                    txIndex, i));

                    compareContractEvent(
                        name1, name2, event1.event, event2.event,
                        fmt::format("tx {} transaction event[{}]", txIndex, i));
                });

            // Compare diagnostic events
            compareVector(
                name1, name2, v4_1.diagnosticEvents, v4_2.diagnosticEvents,
                fmt::format("tx {} diagnostic event", txIndex),
                [&](DiagnosticEvent const& diag1, DiagnosticEvent const& diag2,
                    size_t i) {
                    compareValue(
                        name1, name2, diag1.inSuccessfulContractCall,
                        diag2.inSuccessfulContractCall,
                        fmt::format("tx {} diagnostic event[{}] success flag",
                                    txIndex, i));

                    compareContractEvent(
                        name1, name2, diag1.event, diag2.event,
                        fmt::format("tx {} diagnostic event[{}]", txIndex, i));
                });
        }
        else if (v4_1.sorobanMeta || v4_2.sorobanMeta)
        {
            CLOG_ERROR(Ledger,
                       "tx {} soroban meta presence differs: {} {} vs {} {}",
                       txIndex, name1, static_cast<bool>(v4_1.sorobanMeta),
                       name2, static_cast<bool>(v4_2.sorobanMeta));
        }
    }
    break;

    default:
        // For newer versions, fall back to full comparison
        if (!(meta1 == meta2))
        {
            CLOG_ERROR(Ledger,
                       "tx meta {} differs (version {}): {} {} vs {} {}",
                       txIndex, meta1.v(), name1, "<meta1>", name2, "<meta2>");
        }
        break;
    }
}

} // namespace xdrcomp
} // namespace stellar
