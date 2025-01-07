// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TransactionQueue.h"
#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "herder/SurgePricingUtils.h"
#include "herder/TxQueueLimiter.h"
#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "overlay/OverlayManager.h"
#include "transactions/FeeBumpTransactionFrame.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/OperationFrame.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionUtils.h"
#include "util/BitSet.h"
#include "util/GlobalChecks.h"
#include "util/HashOfHash.h"
#include "util/Math.h"
#include "util/ProtocolVersion.h"
#include "util/TarjanSCCCalculator.h"
#include "util/XDROperators.h"
#include "util/numeric128.h"

#include <Tracy.hpp>
#include <algorithm>
#include <fmt/format.h>
#include <functional>
#include <limits>
#include <medida/meter.h>
#include <medida/metrics_registry.h>
#include <medida/timer.h>
#include <numeric>
#include <optional>
#include <random>

#ifdef BUILD_TESTS
#include "test/TxTests.h"
#include "transactions/test/TransactionTestFrame.h"
#endif

namespace stellar
{
const uint64_t TransactionQueue::FEE_MULTIPLIER = 10;

std::array<const char*,
           static_cast<int>(TransactionQueue::AddResultCode::ADD_STATUS_COUNT)>
    TX_STATUS_STRING = std::array{"PENDING", "DUPLICATE", "ERROR",
                                  "TRY_AGAIN_LATER", "FILTERED"};

TransactionQueue::AddResult::AddResult(AddResultCode addCode)
    : code(addCode), txResult()
{
}

TransactionQueue::AddResult::AddResult(AddResultCode addCode,
                                       MutableTxResultPtr payload)
    : code(addCode), txResult(payload)
{
    releaseAssert(txResult);
}

TransactionQueue::AddResult::AddResult(AddResultCode addCode,
                                       TransactionFrameBasePtr tx,
                                       TransactionResultCode txErrorCode)
    : code(addCode), txResult(tx->createSuccessResult())
{
    releaseAssert(txErrorCode != txSUCCESS);
    txResult->setResultCode(txErrorCode);
}

TransactionQueue::TransactionQueue(Application& app, uint32 pendingDepth,
                                   uint32 banDepth, uint32 poolLedgerMultiplier,
                                   bool isSoroban)
    : mApp(app)
    , mPendingDepth(pendingDepth)
    , mBannedTransactions(banDepth)
    , mBroadcastTimer(app)
{
    mTxQueueLimiter =
        std::make_unique<TxQueueLimiter>(poolLedgerMultiplier, app, isSoroban);

    auto const& filteredTypes =
        app.getConfig().EXCLUDE_TRANSACTIONS_CONTAINING_OPERATION_TYPE;
    mFilteredTypes.insert(filteredTypes.begin(), filteredTypes.end());
    mBroadcastSeed =
        rand_uniform<uint64>(0, std::numeric_limits<uint64>::max());
}

ClassicTransactionQueue::ClassicTransactionQueue(Application& app,
                                                 uint32 pendingDepth,
                                                 uint32 banDepth,
                                                 uint32 poolLedgerMultiplier)
    : TransactionQueue(app, pendingDepth, banDepth, poolLedgerMultiplier, false)
    // Arb tx damping is only relevant to classic txs
    , mArbTxSeenCounter(
          app.getMetrics().NewCounter({"herder", "arb-tx", "seen"}))
    , mArbTxDroppedCounter(
          app.getMetrics().NewCounter({"herder", "arb-tx", "dropped"}))
{
    std::vector<medida::Counter*> sizeByAge;
    for (uint32 i = 0; i < mPendingDepth; i++)
    {
        sizeByAge.emplace_back(&app.getMetrics().NewCounter(
            {"herder", "pending-txs", fmt::format(FMT_STRING("age{:d}"), i)}));
    }
    mQueueMetrics = std::make_unique<QueueMetrics>(
        sizeByAge,
        app.getMetrics().NewCounter({"herder", "pending-txs", "banned"}),
        app.getMetrics().NewTimer({"herder", "pending-txs", "delay"}),
        app.getMetrics().NewTimer({"herder", "pending-txs", "self-delay"}));
    mBroadcastOpCarryover.resize(1,
                                 Resource::makeEmpty(NUM_CLASSIC_TX_RESOURCES));
}

bool
ClassicTransactionQueue::allowTxBroadcast(TimestampedTx const& tx)
{
    bool allowTx{true};

    int32_t const signedAllowance =
        mApp.getConfig().FLOOD_ARB_TX_BASE_ALLOWANCE;
    if (signedAllowance >= 0)
    {
        uint32_t const allowance = static_cast<uint32_t>(signedAllowance);

        // If arb tx damping is enabled, we only flood the first few arb txs
        // touching an asset pair in any given ledger, exponentially
        // reducing the odds of further arb ftx broadcast on a
        // per-asset-pair basis. This lets _some_ arbitrage occur (and
        // retains price-based competition among arbitrageurs earlier in the
        // queue) but avoids filling up ledgers with excessive (mostly
        // failed) arb attempts.
        auto arbPairs = findAllAssetPairsInvolvedInPaymentLoops(tx.mTx);
        if (!arbPairs.empty())
        {
            mArbTxSeenCounter.inc();
            uint32_t maxBroadcast{0};
            std::vector<
                UnorderedMap<AssetPair, uint32_t, AssetPairHash>::iterator>
                hashMapIters;

            // NB: it's essential to reserve() on the hashmap so that we
            // can store iterators to positions in it _as we emplace them_
            // in the loop that follows, without rehashing. Do not remove.
            mArbitrageFloodDamping.reserve(mArbitrageFloodDamping.size() +
                                           arbPairs.size());

            for (auto const& key : arbPairs)
            {
                auto pair = mArbitrageFloodDamping.emplace(key, 0);
                hashMapIters.emplace_back(pair.first);
                maxBroadcast = std::max(maxBroadcast, pair.first->second);
            }

            // Admit while no pair on the path has hit the allowance.
            allowTx = maxBroadcast < allowance;

            // If any pair is over the allowance, dampen transmission
            // randomly based on it.
            if (!allowTx)
            {
                std::geometric_distribution<uint32_t> dist(
                    mApp.getConfig().FLOOD_ARB_TX_DAMPING_FACTOR);
                uint32_t k = maxBroadcast - allowance;
                allowTx = dist(gRandomEngine) >= k;
            }

            // If we've decided to admit a tx, bump all pairs on the path.
            if (allowTx)
            {
                for (auto i : hashMapIters)
                {
                    i->second++;
                }
            }
            else
            {
                mArbTxDroppedCounter.inc();
            }
        }
    }
    return allowTx;
}

TransactionQueue::~TransactionQueue()
{
    // empty destructor needed here due to the dependency on TxQueueLimiter
}

// returns true, if a transaction can be replaced by another
// `minFee` is set when returning false, and is the smallest _full_ fee
// that would allow replace by fee to succeed in this situation
// Note that replace-by-fee logic is done on _inclusion_ fee
static bool
canReplaceByFee(TransactionFrameBasePtr tx, TransactionFrameBasePtr oldTx,
                int64_t& minFee)
{
    int64_t newFee = tx->getInclusionFee();
    uint32_t newNumOps = std::max<uint32_t>(1, tx->getNumOperations());
    int64_t oldFee = oldTx->getInclusionFee();
    uint32_t oldNumOps = std::max<uint32_t>(1, oldTx->getNumOperations());

    // newFee / newNumOps >= FEE_MULTIPLIER * oldFee / oldNumOps
    // is equivalent to
    // newFee * oldNumOps >= FEE_MULTIPLIER * oldFee * newNumOps
    //
    // FEE_MULTIPLIER * oldTotalFee does not overflow uint128_t because fees
    // are bounded by INT64_MAX, while number of operations and
    // FEE_MULTIPLIER are small.
    uint128_t oldTotalFee = bigMultiply(oldFee, newNumOps);
    uint128_t minFeeN = oldTotalFee * TransactionQueue::FEE_MULTIPLIER;

    bool res = newFee >= 0 && bigMultiply(newFee, oldNumOps) >= minFeeN;
    if (!res)
    {
        if (!bigDivide128(minFee, minFeeN, int64_t(oldNumOps),
                          Rounding::ROUND_UP))
        {
            minFee = INT64_MAX;
        }
        else
        {
            // Add the potential flat component to the resulting min fee.
            minFee += tx->getFullFee() - tx->getInclusionFee();
        }
    }
    return res;
}

static bool
isDuplicateTx(TransactionFrameBasePtr oldTx, TransactionFrameBasePtr newTx)
{
    auto const& oldEnv = oldTx->getEnvelope();
    auto const& newEnv = newTx->getEnvelope();

    if (oldEnv.type() == newEnv.type())
    {
        return oldTx->getFullHash() == newTx->getFullHash();
    }
    else if (oldEnv.type() == ENVELOPE_TYPE_TX_FEE_BUMP)
    {
        std::shared_ptr<FeeBumpTransactionFrame const> feeBumpPtr{};
#ifdef BUILD_TESTS
        if (oldTx->isTestTx())
        {
            auto testFrame =
                std::static_pointer_cast<TransactionTestFrame const>(oldTx);
            feeBumpPtr =
                std::static_pointer_cast<FeeBumpTransactionFrame const>(
                    testFrame->getTxFramePtr());
        }
        else
#endif
            feeBumpPtr =
                std::static_pointer_cast<FeeBumpTransactionFrame const>(oldTx);
        return feeBumpPtr->getInnerFullHash() == newTx->getFullHash();
    }
    return false;
}

bool
TransactionQueue::sourceAccountPending(AccountID const& accountID) const
{
    return mAccountStates.find(accountID) != mAccountStates.end();
}

bool
validateSorobanMemo(TransactionFrameBasePtr tx)
{
    if (tx->getEnvelope().type() != ENVELOPE_TYPE_TX)
    {
        return true;
    }

    auto const& txEnv = tx->getEnvelope().v1();
    if (txEnv.tx.operations.size() != 1)
    {
        return true;
    }
    auto const& op = txEnv.tx.operations.at(0);
    if (op.body.type() != INVOKE_HOST_FUNCTION)
    {
        return true;
    }

    bool isSourceAccountAuthOnly = true;

    auto const& auth = op.body.invokeHostFunctionOp().auth;
    for (auto const& authEntry : auth)
    {
        if (authEntry.credentials.type() !=
            SorobanCredentialsType::SOROBAN_CREDENTIALS_SOURCE_ACCOUNT)
        {
            isSourceAccountAuthOnly = false;
            break;
        }
    }

    if (isSourceAccountAuthOnly)
    {
        return true;
    }

    // If tx has a memo or the source account is muxed
    if (txEnv.tx.memo.type() != MemoType::MEMO_NONE ||
        txEnv.tx.sourceAccount.type() == CryptoKeyType::KEY_TYPE_MUXED_ED25519)
    {
        return false;
    }

    // If op source account is muxed
    if (op.sourceAccount &&
        op.sourceAccount->type() == CryptoKeyType::KEY_TYPE_MUXED_ED25519)
    {
        return false;
    }

    return true;
}

TransactionQueue::AddResult
TransactionQueue::canAdd(
    TransactionFrameBasePtr tx, AccountStates::iterator& stateIter,
    std::vector<std::pair<TransactionFrameBasePtr, bool>>& txsToEvict)
{
    ZoneScoped;
    if (isBanned(tx->getFullHash()))
    {
        return AddResult(
            TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
    }
    if (isFiltered(tx))
    {
        return AddResult(TransactionQueue::AddResultCode::ADD_STATUS_FILTERED);
    }

    int64_t newFullFee = tx->getFullFee();
    if (newFullFee < 0 || tx->getInclusionFee() < 0)
    {
        return AddResult(TransactionQueue::AddResultCode::ADD_STATUS_ERROR, tx,
                         txMALFORMED);
    }

    stateIter = mAccountStates.find(tx->getSourceID());
    TransactionFrameBasePtr currentTx;
    if (stateIter != mAccountStates.end())
    {
        auto const& transaction = stateIter->second.mTransaction;

        if (transaction)
        {
            currentTx = transaction->mTx;

            // Check if the tx is a duplicate
            if (isDuplicateTx(currentTx, tx))
            {
                return AddResult(
                    TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE);
            }

            // Any transaction older than the current one is invalid
            if (tx->getSeqNum() < currentTx->getSeqNum())
            {
                // If the transaction is older than the one in the queue, we
                // reject it
                return AddResult(
                    TransactionQueue::AddResultCode::ADD_STATUS_ERROR, tx,
                    txBAD_SEQ);
            }

            // Before rejecting Soroban transactions due to source account
            // limit, check validity of its declared resources, and return an
            // appropriate error message
            if (tx->isSoroban())
            {
                auto txResult = tx->createSuccessResult();
                if (!tx->checkSorobanResourceAndSetError(
                        mApp.getAppConnector(),
                        mApp.getLedgerManager()
                            .getSorobanNetworkConfigReadOnly(),
                        mApp.getLedgerManager()
                            .getLastClosedLedgerHeader()
                            .header.ledgerVersion,
                        txResult))
                {
                    return AddResult(AddResultCode::ADD_STATUS_ERROR, txResult);
                }
            }

            if (tx->getEnvelope().type() != ENVELOPE_TYPE_TX_FEE_BUMP)
            {
                // If there's already a transaction in the queue, we reject
                // any new transaction
                return AddResult(TransactionQueue::AddResultCode::
                                     ADD_STATUS_TRY_AGAIN_LATER);
            }
            else
            {
                if (tx->getSeqNum() != currentTx->getSeqNum())
                {
                    // New fee-bump transaction is rejected
                    return AddResult(TransactionQueue::AddResultCode::
                                         ADD_STATUS_TRY_AGAIN_LATER);
                }

                int64_t minFee;
                if (!canReplaceByFee(tx, currentTx, minFee))
                {
                    AddResult result(
                        TransactionQueue::AddResultCode::ADD_STATUS_ERROR, tx,
                        txINSUFFICIENT_FEE);
                    result.txResult->getResult().feeCharged = minFee;
                    return result;
                }

                if (currentTx->getFeeSourceID() == tx->getFeeSourceID())
                {
                    newFullFee -= currentTx->getFullFee();
                }
            }
        }
    }

    LedgerSnapshot ls(mApp);
    uint32_t ledgerVersion = ls.getLedgerHeader().current().ledgerVersion;
    // Subtle: transactions are rejected based on the source account limit
    // prior to this point. This is safe because we can't evict transactions
    // from the same source account, so a newer transaction won't replace an
    // old one.
    auto canAddRes =
        mTxQueueLimiter->canAddTx(tx, currentTx, txsToEvict, ledgerVersion);
    if (!canAddRes.first)
    {
        ban({tx});
        if (canAddRes.second != 0)
        {
            AddResult result(TransactionQueue::AddResultCode::ADD_STATUS_ERROR,
                             tx, txINSUFFICIENT_FEE);
            result.txResult->getResult().feeCharged = canAddRes.second;
            return result;
        }
        return AddResult(
            TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
    }

    auto closeTime = mApp.getLedgerManager()
                         .getLastClosedLedgerHeader()
                         .header.scpValue.closeTime;
    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_19))
    {
        // This is done so minSeqLedgerGap is validated against the next
        // ledgerSeq, which is what will be used at apply time
        ls.getLedgerHeader().currentToModify().ledgerSeq =
            mApp.getLedgerManager().getLastClosedLedgerNum() + 1;
    }

    auto txResult =
        tx->checkValid(mApp.getAppConnector(), ls, 0, 0,
                       getUpperBoundCloseTimeOffset(mApp, closeTime));
    if (!txResult->isSuccess())
    {
        return AddResult(TransactionQueue::AddResultCode::ADD_STATUS_ERROR,
                         txResult);
    }

    // Note: stateIter corresponds to getSourceID() which is not necessarily
    // the same as getFeeSourceID()
    auto const feeSource = ls.getAccount(tx->getFeeSourceID());
    auto feeStateIter = mAccountStates.find(tx->getFeeSourceID());
    int64_t totalFees = feeStateIter == mAccountStates.end()
                            ? 0
                            : feeStateIter->second.mTotalFees;
    if (getAvailableBalance(ls.getLedgerHeader().current(),
                            feeSource.current()) -
            newFullFee <
        totalFees)
    {
        txResult->setResultCode(txINSUFFICIENT_BALANCE);
        return AddResult(TransactionQueue::AddResultCode::ADD_STATUS_ERROR,
                         txResult);
    }

    if (!validateSorobanMemo(tx))
    {
        txResult->setInnermostResultCode(txSOROBAN_INVALID);

        auto sorobanTxData = txResult->getSorobanData();
        releaseAssertOrThrow(sorobanTxData);

        sorobanTxData->pushValidationTimeDiagnosticError(
            mApp.getConfig(), SCE_CONTEXT, SCEC_INVALID_INPUT,
            "non-source auth Soroban tx uses memo or muxed source account");

        return AddResult(TransactionQueue::AddResultCode::ADD_STATUS_ERROR,
                         txResult);
    }

    return AddResult(TransactionQueue::AddResultCode::ADD_STATUS_PENDING,
                     txResult);
}

void
TransactionQueue::releaseFeeMaybeEraseAccountState(TransactionFrameBasePtr tx)
{
    auto iter = mAccountStates.find(tx->getFeeSourceID());
    releaseAssert(iter != mAccountStates.end() &&
                  iter->second.mTotalFees >= tx->getFullFee());

    iter->second.mTotalFees -= tx->getFullFee();
    if (!iter->second.mTransaction && iter->second.mTotalFees == 0)
    {
        mAccountStates.erase(iter);
    }
}

void
TransactionQueue::prepareDropTransaction(AccountState& as)
{
    releaseAssert(as.mTransaction);
    mTxQueueLimiter->removeTransaction(as.mTransaction->mTx);
    mKnownTxHashes.erase(as.mTransaction->mTx->getFullHash());
    CLOG_DEBUG(Tx, "Dropping {} transaction",
               hexAbbrev(as.mTransaction->mTx->getFullHash()));
    releaseFeeMaybeEraseAccountState(as.mTransaction->mTx);
}

// Heuristic: an "arbitrage transaction" as identified by this function as
// any tx that has 1 or more path payments in it that collectively form a
// payment _loop_. That is: a tx that performs a sequence of order-book
// conversions of at least some quantity of some asset _back_ to itself via
// some number of intermediate steps. Typically these are only a single
// path-payment op, but for thoroughness sake we're also going to cover
// cases where there's any atomic _sequence_ of path payment ops that cause
// a conversion-loop.
//
// Such transactions are not going to be outright banned, note: just damped
// so that they do not overload the network. Currently people are submitting
// thousands of such txs per second in an attempt to win races for
// arbitrage, and we just want to make those races a behave more like
// bidding wars than pure resource-wasting races.
//
// This function doesn't catch all forms of arbitrage -- there are an
// unlimited number of types, many of which involve holding assets,
// interacting with real-world actors, etc. and are indistinguishable from
// "real" traffic -- but it does cover the case of zero-risk (fee-only)
// instantaneous-arbitrage attempts, which users are (at the time of
// writing) flooding the network with.
std::vector<AssetPair>
TransactionQueue::findAllAssetPairsInvolvedInPaymentLoops(
    TransactionFrameBasePtr tx)
{
    std::map<Asset, size_t> assetToNum;
    std::vector<Asset> numToAsset;
    std::vector<BitSet> graph;

    auto internAsset = [&](Asset const& a) -> size_t {
        size_t n = numToAsset.size();
        auto pair = assetToNum.emplace(a, n);
        if (pair.second)
        {
            numToAsset.emplace_back(a);
            graph.emplace_back(BitSet());
        }
        return pair.first->second;
    };

    auto internEdge = [&](Asset const& src, Asset const& dst) {
        auto si = internAsset(src);
        auto di = internAsset(dst);
        graph.at(si).set(di);
    };

    auto internSegment = [&](Asset const& src, Asset const& dst,
                             std::vector<Asset> const& path) {
        Asset const* prev = &src;
        for (auto const& a : path)
        {
            internEdge(*prev, a);
            prev = &a;
        }
        internEdge(*prev, dst);
    };

    for (auto const& op : tx->getRawOperations())
    {
        switch (op.body.type())
        {
        case PATH_PAYMENT_STRICT_RECEIVE:
        {
            auto const& pop = op.body.pathPaymentStrictReceiveOp();
            internSegment(pop.sendAsset, pop.destAsset, pop.path);
        }
        break;
        case PATH_PAYMENT_STRICT_SEND:
        {
            auto const& pop = op.body.pathPaymentStrictSendOp();
            internSegment(pop.sendAsset, pop.destAsset, pop.path);
        }
        break;
        default:
            continue;
        }
    }

    // We build a TarjanSCCCalculator for the graph of all the edges we've
    // seen, and return the set of edges that participate in nontrivial SCCs
    // (which are loops). This is O(|v| + |e|) and just operations on a
    // vector of pairs of integers.

    TarjanSCCCalculator tsc;
    tsc.calculateSCCs(graph.size(), [&graph](size_t i) -> BitSet const& {
        // NB: this closure must be written with the explicit const&
        // returning type signature, otherwise it infers wrong and
        // winds up returning a dangling reference at its site of use.
        return graph.at(i);
    });

    std::vector<AssetPair> ret;
    for (BitSet const& scc : tsc.mSCCs)
    {
        if (scc.count() > 1)
        {
            for (size_t src = 0; scc.nextSet(src); ++src)
            {
                BitSet edgesFromSrcInSCC = graph.at(src);
                edgesFromSrcInSCC.inplaceIntersection(scc);
                for (size_t dst = 0; edgesFromSrcInSCC.nextSet(dst); ++dst)
                {
                    ret.emplace_back(
                        AssetPair{numToAsset.at(src), numToAsset.at(dst)});
                }
            }
        }
    }
    return ret;
}

TransactionQueue::AddResult
TransactionQueue::tryAdd(TransactionFrameBasePtr tx, bool submittedFromSelf)
{
    ZoneScoped;

    auto c1 =
        tx->getEnvelope().type() == ENVELOPE_TYPE_TX_FEE_BUMP &&
        tx->getEnvelope().feeBump().tx.innerTx.type() == ENVELOPE_TYPE_TX &&
        tx->getEnvelope().feeBump().tx.innerTx.v1().tx.ext.v() == 1;
    auto c2 = tx->getEnvelope().type() == ENVELOPE_TYPE_TX &&
              tx->getEnvelope().v1().tx.ext.v() == 1;
    // Check basic structure validity _before_ any fee-related computation
    // fast fail when Soroban tx is malformed
    if ((tx->isSoroban() != (c1 || c2)) || !tx->XDRProvidesValidFee())
    {
        return AddResult(TransactionQueue::AddResultCode::ADD_STATUS_ERROR, tx,
                         txMALFORMED);
    }

    AccountStates::iterator stateIter;

    std::vector<std::pair<TransactionFrameBasePtr, bool>> txsToEvict;
    auto const res = canAdd(tx, stateIter, txsToEvict);
    if (res.code != TransactionQueue::AddResultCode::ADD_STATUS_PENDING)
    {
        return res;
    }

    // only evict if successful
    if (stateIter == mAccountStates.end())
    {
        stateIter =
            mAccountStates.emplace(tx->getSourceID(), AccountState{}).first;
    }

    auto& oldTx = stateIter->second.mTransaction;

    if (oldTx)
    {
        // Drop current transaction associated with this account, replace
        // with `tx`
        prepareDropTransaction(stateIter->second);
        *oldTx = {tx, false, mApp.getClock().now(), submittedFromSelf};
    }
    else
    {
        // New transaction for this account, insert it and update age
        stateIter->second.mTransaction = {tx, false, mApp.getClock().now(),
                                          submittedFromSelf};
        mQueueMetrics->mSizeByAge[stateIter->second.mAge]->inc();
    }

    // Update fee accounting
    auto& thisAccountState = mAccountStates[tx->getFeeSourceID()];
    thisAccountState.mTotalFees += tx->getFullFee();

    // make space so that we can add this transaction
    // this will succeed as `canAdd` ensures that this is the case
    mTxQueueLimiter->evictTransactions(
        txsToEvict, *tx,
        [&](TransactionFrameBasePtr const& txToEvict) { ban({txToEvict}); });
    mTxQueueLimiter->addTransaction(tx);
    mKnownTxHashes[tx->getFullHash()] = tx;

    broadcast(false);

    return res;
}

void
TransactionQueue::dropTransaction(AccountStates::iterator stateIter)
{
    ZoneScoped;
    // Remove fees and update queue size for each transaction to be dropped.
    // Note prepareDropTransaction may erase other iterators from
    // mAccountStates, but it will not erase stateIter because it has at
    // least one transaction (otherwise we couldn't reach that line).
    releaseAssert(stateIter->second.mTransaction);

    prepareDropTransaction(stateIter->second);

    // Actually erase the transaction to be dropped.
    stateIter->second.mTransaction.reset();

    // If the queue for stateIter is now empty, then (1) erase it if it is
    // not the fee-source for some other transaction or (2) reset the age
    // otherwise.
    if (stateIter->second.mTotalFees == 0)
    {
        mAccountStates.erase(stateIter);
    }
    else
    {
        stateIter->second.mAge = 0;
    }
}

void
TransactionQueue::removeApplied(Transactions const& appliedTxs)
{
    ZoneScoped;

    auto now = mApp.getClock().now();
    for (auto const& appliedTx : appliedTxs)
    {
        // If the source account is not in mAccountStates, then it has no
        // transactions in the queue so there is nothing to do
        auto stateIter = mAccountStates.find(appliedTx->getSourceID());
        if (stateIter != mAccountStates.end())
        {
            // If there are no transactions in the queue for this source
            // account, then there is nothing to do
            auto const& transaction = stateIter->second.mTransaction;
            if (transaction)
            {
                // We care about matching the sequence number rather than
                // the hash, because any transaction with a sequence number
                // less-than-or-equal to the highest applied sequence number
                // for this source account has either (1) been applied, or
                // (2) become invalid.
                if (transaction->mTx->getSeqNum() <= appliedTx->getSeqNum())
                {
                    auto& age = stateIter->second.mAge;
                    mQueueMetrics->mSizeByAge[age]->dec();
                    age = 0;

                    // update the metric for the time spent for applied
                    // transactions using exact match
                    if (transaction->mTx->getFullHash() ==
                        appliedTx->getFullHash())
                    {
                        auto elapsed = now - transaction->mInsertionTime;
                        mQueueMetrics->mTransactionsDelay.Update(elapsed);
                        if (transaction->mSubmittedFromSelf)
                        {
                            mQueueMetrics->mTransactionsSelfDelay.Update(
                                elapsed);
                        }
                    }

                    // WARNING: stateIter and everything that references it
                    // may be invalid from this point onward and should not
                    // be used.
                    dropTransaction(stateIter);
                }
            }
        }

        // Ban applied tx
        auto& bannedFront = mBannedTransactions.front();
        bannedFront.emplace(appliedTx->getFullHash());
        CLOG_DEBUG(Tx, "Ban applied transaction {}",
                   hexAbbrev(appliedTx->getFullHash()));

        // do not mark metric for banning as this is the result of normal
        // flow of operations
    }
}

void
TransactionQueue::ban(Transactions const& banTxs)
{
    ZoneScoped;
    auto& bannedFront = mBannedTransactions.front();

    // Group the transactions by source account and ban all the transactions
    // that are explicitly listed
    std::map<AccountID, TransactionFrameBasePtr> transactionsByAccount;
    for (auto const& tx : banTxs)
    {
        // Must be a new transaction for this account
        releaseAssert(
            transactionsByAccount.emplace(tx->getSourceID(), tx).second);
        CLOG_DEBUG(Tx, "Ban transaction {}", hexAbbrev(tx->getFullHash()));
        if (bannedFront.emplace(tx->getFullHash()).second)
        {
            mQueueMetrics->mBannedTransactionsCounter.inc();
        }
    }

    for (auto const& kv : transactionsByAccount)
    {
        // If the source account is not in mAccountStates, then it has no
        // transactions in the queue so there is nothing to do
        auto stateIter = mAccountStates.find(kv.first);
        if (stateIter != mAccountStates.end())
        {
            auto const& transaction = stateIter->second.mTransaction;
            // Only ban transactions that are actually present in the queue.
            // Transactions with higher sequence numbers than banned
            // transactions remain in the queue.
            if (transaction &&
                transaction->mTx->getFullHash() == kv.second->getFullHash())
            {
                mQueueMetrics->mSizeByAge[stateIter->second.mAge]->dec();
                // WARNING: stateIter and everything that references it may
                // be invalid from this point onward and should not be used.
                dropTransaction(stateIter);
            }
        }
    }
}

#ifdef BUILD_TESTS
TransactionQueue::AccountState
TransactionQueue::getAccountTransactionQueueInfo(
    AccountID const& accountID) const
{
    auto i = mAccountStates.find(accountID);
    if (i == std::end(mAccountStates))
    {
        return AccountState{};
    }
    return i->second;
}

size_t
TransactionQueue::countBanned(int index) const
{
    return mBannedTransactions[index].size();
}
#endif

void
TransactionQueue::shift()
{
    ZoneScoped;
    mBannedTransactions.pop_back();
    mBannedTransactions.emplace_front();
    mArbitrageFloodDamping.clear();

    auto sizes = std::vector<int64_t>{};
    sizes.resize(mPendingDepth);

    auto& bannedFront = mBannedTransactions.front();
    auto end = std::end(mAccountStates);
    auto it = std::begin(mAccountStates);
    while (it != end)
    {
        // If mTransactions is empty then mAge is always 0. This can occur
        // if an account is the fee-source for at least one transaction but
        // not the sequence-number-source for any transaction in the
        // TransactionQueue.
        if (it->second.mTransaction)
        {
            ++it->second.mAge;
        }

        if (mPendingDepth == it->second.mAge)
        {
            if (it->second.mTransaction)
            {
                // This never invalidates it because
                //     it->second.mTransaction
                // otherwise we couldn't have reached this line.
                prepareDropTransaction(it->second);
                CLOG_DEBUG(
                    Tx, "Ban transaction {}",
                    hexAbbrev(it->second.mTransaction->mTx->getFullHash()));
                bannedFront.insert(it->second.mTransaction->mTx->getFullHash());
                mQueueMetrics->mBannedTransactionsCounter.inc();
                it->second.mTransaction.reset();
            }
            if (it->second.mTotalFees == 0)
            {
                it = mAccountStates.erase(it);
            }
            else
            {
                it->second.mAge = 0;
            }
        }
        else
        {
            sizes[it->second.mAge] +=
                static_cast<int>(it->second.mTransaction.has_value());
            ++it;
        }
    }

    for (size_t i = 0; i < sizes.size(); i++)
    {
        mQueueMetrics->mSizeByAge[i]->set_count(sizes[i]);
    }
    mTxQueueLimiter->resetEvictionState();
    // pick a new randomizing seed for tie breaking
    mBroadcastSeed =
        rand_uniform<uint64>(0, std::numeric_limits<uint64>::max());
}

bool
TransactionQueue::isBanned(Hash const& hash) const
{
    return std::any_of(
        std::begin(mBannedTransactions), std::end(mBannedTransactions),
        [&](UnorderedSet<Hash> const& transactions) {
            return transactions.find(hash) != std::end(transactions);
        });
}

TxFrameList
TransactionQueue::getTransactions(LedgerHeader const& lcl) const
{
    ZoneScoped;
    TxFrameList txs;

    uint32_t const nextLedgerSeq = lcl.ledgerSeq + 1;
    int64_t const startingSeq = getStartingSequenceNumber(nextLedgerSeq);
    for (auto const& m : mAccountStates)
    {
        if (m.second.mTransaction &&
            m.second.mTransaction->mTx->getSeqNum() != startingSeq)
        {
            txs.emplace_back(m.second.mTransaction->mTx);
        }
    }

    return txs;
}

TransactionFrameBaseConstPtr
TransactionQueue::getTx(Hash const& hash) const
{
    ZoneScoped;
    auto it = mKnownTxHashes.find(hash);
    if (it != mKnownTxHashes.end())
    {
        return it->second;
    }
    else
    {
        return nullptr;
    }
}

std::pair<Resource, std::optional<Resource>>
ClassicTransactionQueue::getMaxResourcesToFloodThisPeriod() const
{
    auto& cfg = mApp.getConfig();
    double opRatePerLedger = cfg.FLOOD_OP_RATE_PER_LEDGER;

    auto maxOps = mApp.getLedgerManager().getLastMaxTxSetSizeOps();
    double opsToFloodLedgerDbl = opRatePerLedger * maxOps;
    releaseAssertOrThrow(opsToFloodLedgerDbl >= 0.0);
    releaseAssertOrThrow(isRepresentableAsInt64(opsToFloodLedgerDbl));
    int64_t opsToFloodLedger = static_cast<int64_t>(opsToFloodLedgerDbl);

    auto opsToFlood =
        mBroadcastOpCarryover[SurgePricingPriorityQueue::GENERIC_LANE] +
        Resource(
            bigDivideOrThrow(opsToFloodLedger, getFloodPeriod(),
                             cfg.getExpectedLedgerCloseTime().count() * 1000,
                             Rounding::ROUND_UP));
    releaseAssertOrThrow(Resource(0) <= opsToFlood &&
                         opsToFlood <=
                             Resource(std::numeric_limits<uint32_t>::max()));

    auto maxDexOps = cfg.MAX_DEX_TX_OPERATIONS_IN_TX_SET;
    std::optional<Resource> dexOpsToFlood;
    if (maxDexOps)
    {
        *maxDexOps = std::min(maxOps, *maxDexOps);
        uint32_t dexOpsToFloodLedger =
            static_cast<uint32_t>(*maxDexOps * opRatePerLedger);
        auto dexOpsCarryover =
            mBroadcastOpCarryover.size() > DexLimitingLaneConfig::DEX_LANE
                ? mBroadcastOpCarryover[DexLimitingLaneConfig::DEX_LANE]
                : 0;
        auto dexOpsToFloodUint =
            dexOpsCarryover +
            static_cast<uint32>(bigDivideOrThrow(
                dexOpsToFloodLedger, getFloodPeriod(),
                cfg.getExpectedLedgerCloseTime().count() * 1000ll,
                Rounding::ROUND_UP));
        dexOpsToFlood = dexOpsToFloodUint;
    }
    return std::make_pair(opsToFlood, dexOpsToFlood);
}

TransactionQueue::BroadcastStatus
TransactionQueue::broadcastTx(TimestampedTx& tx)
{
    if (tx.mBroadcasted)
    {
        return BroadcastStatus::BROADCAST_STATUS_ALREADY;
    }

    bool allowTx = allowTxBroadcast(tx);

#ifdef BUILD_TESTS
    if (mTxBroadcastedEvent)
    {
        mTxBroadcastedEvent(tx.mTx);
    }
#endif

    // Mark the tx as effectively "broadcast" and update the per-account
    // queue to count it as consumption from that balance, for proper
    // overall queue accounting (whether or not we will actually broadcast
    // it).
    tx.mBroadcasted = true;

    if (!allowTx)
    {
        // If we decide not to broadcast for real (due to damping) we return
        // false to our caller so that they will not count this tx against
        // the per-timeslice counters -- we want to allow the caller to try
        // useful work from other sources.
        return BroadcastStatus::BROADCAST_STATUS_SKIPPED;
    }
    return mApp.getOverlayManager().broadcastMessage(
               tx.mTx->toStellarMessage(),
               std::make_optional<Hash>(tx.mTx->getFullHash()))
               ? BroadcastStatus::BROADCAST_STATUS_SUCCESS
               : BroadcastStatus::BROADCAST_STATUS_ALREADY;
}

SorobanTransactionQueue::SorobanTransactionQueue(Application& app,
                                                 uint32 pendingDepth,
                                                 uint32 banDepth,
                                                 uint32 poolLedgerMultiplier)
    : TransactionQueue(app, pendingDepth, banDepth, poolLedgerMultiplier, true)
{

    std::vector<medida::Counter*> sizeByAge;
    for (uint32 i = 0; i < mPendingDepth; i++)
    {
        sizeByAge.emplace_back(&app.getMetrics().NewCounter(
            {"herder", "pending-soroban-txs",
             fmt::format(FMT_STRING("age{:d}"), i)}));
    }
    mQueueMetrics = std::make_unique<QueueMetrics>(
        sizeByAge,
        app.getMetrics().NewCounter(
            {"herder", "pending-soroban-txs", "banned"}),
        app.getMetrics().NewTimer({"herder", "pending-soroban-txs", "delay"}),
        app.getMetrics().NewTimer(
            {"herder", "pending-soroban-txs", "self-delay"}));
    mBroadcastOpCarryover.resize(1, Resource::makeEmptySoroban());
}

std::pair<Resource, std::optional<Resource>>
SorobanTransactionQueue::getMaxResourcesToFloodThisPeriod() const
{
    auto const& cfg = mApp.getConfig();
    double ratePerLedger = cfg.FLOOD_SOROBAN_RATE_PER_LEDGER;

    auto sorRes = mApp.getLedgerManager().maxLedgerResources(true);

    auto totalFloodPerLedger = multiplyByDouble(sorRes, ratePerLedger);

    Resource resToFlood =
        mBroadcastOpCarryover[SurgePricingPriorityQueue::GENERIC_LANE] +
        bigDivideOrThrow(totalFloodPerLedger, getFloodPeriod(),
                         cfg.getExpectedLedgerCloseTime().count() * 1000,
                         Rounding::ROUND_UP);
    return std::make_pair(resToFlood, std::nullopt);
}

bool
SorobanTransactionQueue::broadcastSome()
{
    // broadcast transactions in surge pricing order:
    // loop over transactions by picking from the account queue with the
    // highest base fee not broadcasted so far.
    // This broadcasts from account queues in order as to maximize chances
    // of propagation.
    auto resToFlood = getMaxResourcesToFloodThisPeriod().first;

    auto totalResToFlood = Resource::makeEmptySoroban();
    std::vector<TransactionFrameBasePtr> txsToBroadcast;
    std::unordered_map<TransactionFrameBasePtr, AccountState*> txToAccountState;
    for (auto& [_, accountState] : mAccountStates)
    {
        if (accountState.mTransaction &&
            !accountState.mTransaction->mBroadcasted)
        {
            auto tx = accountState.mTransaction->mTx;
            txsToBroadcast.emplace_back(tx);
            totalResToFlood += tx->getResources(
                /* useByteLimitInClassic */ false);
            txToAccountState[tx] = &accountState;
        }
    }

    auto visitor = [this, &totalResToFlood,
                    &txToAccountState](TransactionFrameBasePtr const& tx) {
        auto& accState = *txToAccountState.at(tx);
        // look at the next candidate transaction for that account
        auto& cur = *accState.mTransaction;
        // by construction, cur points to non broadcasted transactions
        releaseAssert(!cur.mBroadcasted);
        auto bStatus = broadcastTx(cur);
        // Skipped does not apply to Soroban
        releaseAssert(bStatus != BroadcastStatus::BROADCAST_STATUS_SKIPPED);
        if (bStatus == BroadcastStatus::BROADCAST_STATUS_SUCCESS)
        {
            totalResToFlood -=
                tx->getResources(/* useByteLimitInClassic */ false);
            return SurgePricingPriorityQueue::VisitTxResult::PROCESSED;
        }
        else
        {
            // Already broadcasted, skip the transaction and don't count it
            // towards the total resources to flood.
            return SurgePricingPriorityQueue::VisitTxResult::SKIPPED;
        }
    };

    SurgePricingPriorityQueue queue(
        /* isHighestPriority */ true,
        std::make_shared<SorobanGenericLaneConfig>(resToFlood), mBroadcastSeed);
    queue.visitTopTxs(txsToBroadcast, visitor, mBroadcastOpCarryover);

    Resource maxPerTx =
        mApp.getLedgerManager().maxSorobanTransactionResources();
    for (auto& resLeft : mBroadcastOpCarryover)
    {
        // Limit carry-over to 1 maximum resource transaction
        resLeft = limitTo(resLeft, maxPerTx);
    }
    return !totalResToFlood.isZero();
}

size_t
SorobanTransactionQueue::getMaxQueueSizeOps() const
{
    if (protocolVersionStartsFrom(mApp.getLedgerManager()
                                      .getLastClosedLedgerHeader()
                                      .header.ledgerVersion,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        auto res = mTxQueueLimiter->maxScaledLedgerResources(true);
        releaseAssert(res.size() == NUM_SOROBAN_TX_RESOURCES);
        return res.getVal(Resource::Type::OPERATIONS);
    }
    else
    {
        return 0;
    }
}

bool
ClassicTransactionQueue::broadcastSome()
{
    // broadcast transactions in surge pricing order:
    // loop over transactions by picking from the account queue with the
    // highest base fee not broadcasted so far.
    // This broadcasts from account queues in order as to maximize chances
    // of propagation.
    auto [opsToFlood, dexOpsToFlood] = getMaxResourcesToFloodThisPeriod();
    releaseAssert(opsToFlood.size() == NUM_CLASSIC_TX_RESOURCES);
    if (dexOpsToFlood)
    {
        releaseAssert(dexOpsToFlood->size() == NUM_CLASSIC_TX_RESOURCES);
    }

    auto totalToFlood = Resource::makeEmpty(NUM_CLASSIC_TX_RESOURCES);
    std::vector<TransactionFrameBasePtr> txsToBroadcast;
    std::unordered_map<TransactionFrameBasePtr, AccountState*> txToAccountState;
    for (auto& [_, accountState] : mAccountStates)
    {
        if (accountState.mTransaction &&
            !accountState.mTransaction->mBroadcasted)
        {
            auto tx = accountState.mTransaction->mTx;
            txsToBroadcast.emplace_back(tx);
            totalToFlood += Resource(tx->getNumOperations());
            txToAccountState[tx] = &accountState;
        }
    }

    std::vector<TransactionFrameBasePtr> banningTxs;
    auto visitor = [this, &totalToFlood, &banningTxs,
                    &txToAccountState](TransactionFrameBasePtr const& tx) {
        auto const& curTracker = txToAccountState.at(tx);
        // look at the next candidate transaction for that account
        auto& cur = *curTracker->mTransaction;
        // by construction, cur points to non broadcasted transactions
        releaseAssert(!cur.mBroadcasted);
        auto bStatus = broadcastTx(cur);
        if (bStatus == BroadcastStatus::BROADCAST_STATUS_SUCCESS)
        {
            totalToFlood -= tx->getResources(/* useByteLimitInClassic */ false);
            return SurgePricingPriorityQueue::VisitTxResult::PROCESSED;
        }
        else if (bStatus == BroadcastStatus::BROADCAST_STATUS_SKIPPED)
        {
            // When skipping, we ban the transaction and skip its resources.
            banningTxs.emplace_back(tx);
            return SurgePricingPriorityQueue::VisitTxResult::SKIPPED;
        }
        else
        {
            // Already broadcasted, skip the transaction and don't count it
            // towards the total resources to flood.
            return SurgePricingPriorityQueue::VisitTxResult::SKIPPED;
        }
    };

    SurgePricingPriorityQueue queue(
        /* isHighestPriority */ true,
        std::make_shared<DexLimitingLaneConfig>(opsToFlood, dexOpsToFlood),
        mBroadcastSeed);
    queue.visitTopTxs(txsToBroadcast, visitor, mBroadcastOpCarryover);
    ban(banningTxs);
    // carry over remainder, up to MAX_OPS_PER_TX ops
    // reason is that if we add 1 next round, we can flood a "worst case fee
    // bump" tx
    for (auto& opsLeft : mBroadcastOpCarryover)
    {
        releaseAssert(opsLeft.size() == NUM_CLASSIC_TX_RESOURCES);
        opsLeft = limitTo(opsLeft, Resource(MAX_OPS_PER_TX + 1));
    }
    return !totalToFlood.isZero();
}

void
TransactionQueue::broadcast(bool fromCallback)
{
    if (mShutdown || (!fromCallback && mWaiting))
    {
        return;
    }
    mWaiting = false;

    bool needsMore = false;
    if (!fromCallback)
    {
        // don't do anything right away, wait for the timer
        needsMore = true;
    }
    else
    {
        needsMore = broadcastSome();
    }

    if (needsMore)
    {
        mWaiting = true;
        mBroadcastTimer.expires_from_now(
            std::chrono::milliseconds(getFloodPeriod()));
        mBroadcastTimer.async_wait([&]() { broadcast(true); },
                                   &VirtualTimer::onFailureNoop);
    }
}

void
TransactionQueue::rebroadcast()
{
    // force to rebroadcast everything
    for (auto& m : mAccountStates)
    {
        auto& as = m.second;
        if (as.mTransaction)
        {
            as.mTransaction->mBroadcasted = false;
        }
    }
    broadcast(false);
}

void
TransactionQueue::shutdown()
{
    mShutdown = true;
    mBroadcastTimer.cancel();
}

static bool
containsFilteredOperation(std::vector<Operation> const& ops,
                          UnorderedSet<OperationType> const& filteredTypes)
{
    return std::any_of(ops.begin(), ops.end(), [&](auto const& op) {
        return filteredTypes.find(op.body.type()) != filteredTypes.end();
    });
}

bool
TransactionQueue::isFiltered(TransactionFrameBasePtr tx) const
{
    // Avoid cost of checking if filtering is not in use
    if (mFilteredTypes.empty())
    {
        return false;
    }

    switch (tx->getEnvelope().type())
    {
    case ENVELOPE_TYPE_TX_V0:
        return containsFilteredOperation(tx->getEnvelope().v0().tx.operations,
                                         mFilteredTypes);
    case ENVELOPE_TYPE_TX:
        return containsFilteredOperation(tx->getEnvelope().v1().tx.operations,
                                         mFilteredTypes);
    case ENVELOPE_TYPE_TX_FEE_BUMP:
    {
        auto const& envelope = tx->getEnvelope().feeBump().tx.innerTx.v1();
        return containsFilteredOperation(envelope.tx.operations,
                                         mFilteredTypes);
    }
    default:
        abort();
    }
}

#ifdef BUILD_TESTS
size_t
TransactionQueue::getQueueSizeOps() const
{
    return mTxQueueLimiter->size();
}

std::optional<int64_t>
TransactionQueue::getInQueueSeqNum(AccountID const& account) const
{
    auto stateIter = mAccountStates.find(account);
    if (stateIter == mAccountStates.end())
    {
        return std::nullopt;
    }
    if (stateIter->second.mTransaction)
    {
        return stateIter->second.mTransaction->mTx->getSeqNum();
    }
    return std::nullopt;
}
#endif

size_t
ClassicTransactionQueue::getMaxQueueSizeOps() const
{
    auto res = mTxQueueLimiter->maxScaledLedgerResources(false);
    releaseAssert(res.size() == NUM_CLASSIC_TX_RESOURCES);
    return res.getVal(Resource::Type::OPERATIONS);
}
}
