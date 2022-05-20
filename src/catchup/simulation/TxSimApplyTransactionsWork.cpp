// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/simulation/TxSimApplyTransactionsWork.h"
#include "catchup/ApplyLedgerWork.h"
#include "crypto/Hex.h"
#include "crypto/SignerKey.h"
#include "herder/LedgerCloseData.h"
#include "herder/simulation/TxSimTxSetFrame.h"
#include "history/HistoryArchiveManager.h"
#include "ledger/LedgerManagerImpl.h"
#include "ledger/LedgerRange.h"
#include "ledger/LedgerTxn.h"
#include "src/transactions/simulation/TxSimUtils.h"
#include "transactions/FeeBumpTransactionFrame.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionSQL.h"
#include "transactions/TransactionUtils.h"
#include "util/HashOfHash.h"
#include "util/XDRCereal.h"
#include <fmt/format.h>

namespace stellar
{
namespace txsimulation
{
constexpr uint32_t const CLEAR_METRICS_AFTER_NUM_LEDGERS = 100;

TxSimApplyTransactionsWork::TxSimApplyTransactionsWork(
    Application& app, TmpDir const& downloadDir, LedgerRange const& range,
    std::string const& networkPassphrase, uint32_t desiredOperations,
    bool upgrade, uint32_t multiplier, bool verifyResults)
    : BasicWork(app, "apply-transactions", RETRY_NEVER)
    , mDownloadDir(downloadDir)
    , mRange(range)
    , mNetworkID(sha256(networkPassphrase))
    , mTransactionHistory{}
    , mTransactionIter(mTransactionHistory.cend())
    , mResultHistory{}
    , mResultIter(mResultHistory.txResultSet.results.cend())
    , mMaxOperations(desiredOperations)
    , mUpgradeProtocol(upgrade)
    , mMultiplier(multiplier)
    , mVerifyResults(verifyResults)
{
    if (mMultiplier == 0)
    {
        throw std::runtime_error("Invalid multiplier!");
    }

    auto const& lcl = mApp.getLedgerManager().getLastClosedLedgerHeader();
    if (mUpgradeProtocol && lcl.header.ledgerVersion + 1 !=
                                mApp.getConfig().LEDGER_PROTOCOL_VERSION)
    {
        throw std::runtime_error("Invalid ledger version: can only force "
                                 "upgrade for consecutive versions");
    }
}

static void
checkOperationResults(xdr::xvector<OperationResult> const& expected,
                      xdr::xvector<OperationResult> const& actual)
{
    assert(expected.size() == actual.size());
    for (size_t i = 0; i < expected.size(); i++)
    {
        if (expected[i].code() != actual[i].code())
        {
            CLOG_ERROR(History, "Expected {} but got {}",
                       xdr_to_string(expected[i].code(), "OperationResultCode"),
                       xdr_to_string(actual[i].code(), "OperationResultCode"));
            continue;
        }

        if (expected[i].code() != opINNER)
        {
            continue;
        }

        auto const& expectedOpRes = expected[i].tr();
        auto const& actualOpRes = actual[i].tr();

        assert(expectedOpRes.type() == actualOpRes.type());

        auto check = [&](int expectedCode, int actualCode) {
            auto success = expectedCode >= 0 && actualCode >= 0;
            auto fail = expectedCode < 0 && actualCode < 0;
            return success || fail;
        };

        bool match = false;
        switch (expectedOpRes.type())
        {
        case CREATE_ACCOUNT:
            match = check(actualOpRes.createAccountResult().code(),
                          expectedOpRes.createAccountResult().code());
            break;
        case PAYMENT:
            match = check(actualOpRes.paymentResult().code(),
                          expectedOpRes.paymentResult().code());
            break;
        case PATH_PAYMENT_STRICT_RECEIVE:
            match =
                check(actualOpRes.pathPaymentStrictReceiveResult().code(),
                      expectedOpRes.pathPaymentStrictReceiveResult().code());
            break;
        case PATH_PAYMENT_STRICT_SEND:
            match = check(actualOpRes.pathPaymentStrictSendResult().code(),
                          expectedOpRes.pathPaymentStrictSendResult().code());
            break;
        case MANAGE_SELL_OFFER:
            match = check(actualOpRes.manageSellOfferResult().code(),
                          expectedOpRes.manageSellOfferResult().code());
            break;
        case MANAGE_BUY_OFFER:
            match = check(actualOpRes.manageBuyOfferResult().code(),
                          expectedOpRes.manageBuyOfferResult().code());
            break;
        case CREATE_PASSIVE_SELL_OFFER:
            match = check(actualOpRes.createPassiveSellOfferResult().code(),
                          expectedOpRes.createPassiveSellOfferResult().code());
            break;
        case SET_OPTIONS:
            match = check(actualOpRes.setOptionsResult().code(),
                          expectedOpRes.setOptionsResult().code());
            break;
        case CHANGE_TRUST:
            match = check(actualOpRes.changeTrustResult().code(),
                          expectedOpRes.changeTrustResult().code());
            break;
        case ALLOW_TRUST:
            match = check(actualOpRes.allowTrustResult().code(),
                          expectedOpRes.allowTrustResult().code());
            break;
        case ACCOUNT_MERGE:
            match = check(actualOpRes.accountMergeResult().code(),
                          expectedOpRes.accountMergeResult().code());
            break;
        case MANAGE_DATA:
            match = check(actualOpRes.manageDataResult().code(),
                          expectedOpRes.manageDataResult().code());
            break;
        case INFLATION:
            match = check(actualOpRes.inflationResult().code(),
                          expectedOpRes.inflationResult().code());
            break;
        case BUMP_SEQUENCE:
            match = check(actualOpRes.bumpSeqResult().code(),
                          expectedOpRes.bumpSeqResult().code());
            break;
        case CREATE_CLAIMABLE_BALANCE:
            match = check(actualOpRes.createClaimableBalanceResult().code(),
                          expectedOpRes.createClaimableBalanceResult().code());
            break;
        case CLAIM_CLAIMABLE_BALANCE:
            match = check(actualOpRes.claimClaimableBalanceResult().code(),
                          expectedOpRes.claimClaimableBalanceResult().code());
            break;
        case BEGIN_SPONSORING_FUTURE_RESERVES:
            match = check(
                actualOpRes.beginSponsoringFutureReservesResult().code(),
                expectedOpRes.beginSponsoringFutureReservesResult().code());
            break;
        case END_SPONSORING_FUTURE_RESERVES:
            match =
                check(actualOpRes.endSponsoringFutureReservesResult().code(),
                      expectedOpRes.endSponsoringFutureReservesResult().code());
            break;
        case REVOKE_SPONSORSHIP:
            match = check(actualOpRes.revokeSponsorshipResult().code(),
                          expectedOpRes.revokeSponsorshipResult().code());
            break;
        case CLAWBACK:
            match = check(actualOpRes.clawbackResult().code(),
                          expectedOpRes.clawbackResult().code());
            break;
        case CLAWBACK_CLAIMABLE_BALANCE:
            match =
                check(actualOpRes.clawbackClaimableBalanceResult().code(),
                      expectedOpRes.clawbackClaimableBalanceResult().code());
            break;
        case SET_TRUST_LINE_FLAGS:
            match = check(actualOpRes.setTrustLineFlagsResult().code(),
                          expectedOpRes.setTrustLineFlagsResult().code());
            break;
        case LIQUIDITY_POOL_DEPOSIT:
            match = check(actualOpRes.liquidityPoolDepositResult().code(),
                          expectedOpRes.liquidityPoolDepositResult().code());
            break;
        case LIQUIDITY_POOL_WITHDRAW:
            match = check(actualOpRes.liquidityPoolWithdrawResult().code(),
                          expectedOpRes.liquidityPoolWithdrawResult().code());
            break;
        default:
            throw std::runtime_error("Unknown operation type");
        }

        if (!match)
        {
            CLOG_ERROR(History, "Expected {}",
                       xdr_to_string(expectedOpRes, "OperationResult"));
            CLOG_ERROR(History, "Actual {}",
                       xdr_to_string(actualOpRes, "OperationResult"));
        }
    }
}

static void
checkResults(Application& app, uint32_t ledger,
             std::vector<TransactionResultPair> const& results)
{
    auto resSet = getTransactionHistoryResults(app.getDatabase(), ledger);

    assert(resSet.results.size() == results.size());
    for (size_t i = 0; i < results.size(); i++)
    {
        assert(results[i].transactionHash == resSet.results[i].transactionHash);

        auto const& dbRes = resSet.results[i].result.result;
        auto const& archiveRes = results[i].result.result;

        if (dbRes.code() != archiveRes.code())
        {
            CLOG_ERROR(
                History, "Expected {} does not agree with {} for tx {}",
                xdr_to_string(archiveRes.code(), "TransactionResultCode"),
                xdr_to_string(dbRes.code(), "TransactionResultCode"),
                binToHex(results[i].transactionHash));
        }
        else if (dbRes.code() == txFEE_BUMP_INNER_FAILED ||
                 dbRes.code() == txFEE_BUMP_INNER_SUCCESS)
        {

            if (dbRes.innerResultPair().result.result.code() !=
                archiveRes.innerResultPair().result.result.code())
            {
                CLOG_ERROR(
                    History,
                    "Expected {} does not agree with {} for "
                    "fee-bump inner tx {}",
                    xdr_to_string(
                        archiveRes.innerResultPair().result.result.code(),
                        "TransactionResultCode"),
                    xdr_to_string(dbRes.innerResultPair().result.result.code(),
                                  "TransactionResultCode"),
                    binToHex(archiveRes.innerResultPair().transactionHash));
            }
            else if (dbRes.innerResultPair().result.result.code() == txFAILED ||
                     dbRes.innerResultPair().result.result.code() == txSUCCESS)
            {
                checkOperationResults(
                    archiveRes.innerResultPair().result.result.results(),
                    dbRes.innerResultPair().result.result.results());
            }
        }
        else if (dbRes.code() == txFAILED || dbRes.code() == txSUCCESS)
        {
            checkOperationResults(archiveRes.results(), dbRes.results());
        }
    }
}

static bool
hasSig(PublicKey const& account,
       xdr::xvector<DecoratedSignature, 20> const& sigs, Hash const& hash)
{
    // Is the signature of this account present in the envelope we're
    // simulating?
    return std::any_of(sigs.begin(), sigs.end(),
                       [&](DecoratedSignature const& sig) {
                           return SignatureUtils::verify(sig, account, hash);
                       });
}

void
TxSimApplyTransactionsWork::addSignerKeys(
    AccountID const& acc, AbstractLedgerTxn& ltx, std::set<SecretKey>& keys,
    xdr::xvector<DecoratedSignature, 20> const& sigs, Hash const& txHash,
    uint32_t partition)
{
    if (hasSig(acc, sigs, txHash))
    {
        keys.emplace(generateScaledSecret(acc, partition));
    }

    auto account = stellar::loadAccount(ltx, acc);
    if (!account)
    {
        return;
    }

    auto maybeAddKey = [&](SignerKey const& signer) {
        if (signer.type() == SIGNER_KEY_TYPE_ED25519)
        {
            auto pubKey = KeyUtils::convertKey<PublicKey>(signer);
            if (hasSig(pubKey, sigs, txHash))
            {
                keys.emplace(generateScaledSecret(pubKey, partition));
            }
        }
    };

    for (auto const& signer : account.current().data.account().signers)
    {
        maybeAddKey(signer.key);
    }

    auto const& env = mUpgradeProtocol
                          ? txbridge::convertForV13(*mTransactionIter)
                          : *mTransactionIter;
    if (env.type() == ENVELOPE_TYPE_TX && env.v1().tx.cond.type() == PRECOND_V2)
    {
        for (auto const& signerKey : env.v1().tx.cond.v2().extraSigners)
        {
            maybeAddKey(signerKey);
        }
    }
}

Hash const&
TxSimApplyTransactionsWork::getInnerTxHash()
{
    bool isFeeBump =
        mResultIter->result.result.code() == txFEE_BUMP_INNER_SUCCESS ||
        mResultIter->result.result.code() == txFEE_BUMP_INNER_FAILED;
    return isFeeBump
               ? mResultIter->result.result.innerResultPair().transactionHash
               : mResultIter->transactionHash;
}

void
TxSimApplyTransactionsWork::addSignerKeys(
    MuxedAccount const& acc, AbstractLedgerTxn& ltx, std::set<SecretKey>& keys,
    xdr::xvector<DecoratedSignature, 20> const& sigs, Hash const& txHash,
    uint32_t partition)
{
    addSignerKeys(toAccountID(acc), ltx, keys, sigs, txHash, partition);
}

void
TxSimApplyTransactionsWork::mutateTxSourceAccounts(TransactionEnvelope& env,
                                                   AbstractLedgerTxn& ltx,
                                                   std::set<SecretKey>& keys,
                                                   uint32_t partition)
{
    auto const& sigs = txbridge::getSignaturesInner(env);
    auto addSignerAndReplaceID = [&](MuxedAccount& acc) {
        addSignerKeys(acc, ltx, keys, sigs, getInnerTxHash(), partition);
        mutateScaledAccountID(acc, partition);
    };

    // Depending on the envelope type, update sourceAccount and maybe feeSource
    AccountID acc;
    switch (env.type())
    {
    case ENVELOPE_TYPE_TX_V0:
        // Wrap raw Ed25519 key in an AccountID
        acc.type(PUBLIC_KEY_TYPE_ED25519);
        acc.ed25519() = env.v0().tx.sourceAccountEd25519;
        addSignerKeys(acc, ltx, keys, sigs, getInnerTxHash(), partition);
        env.v0().tx.sourceAccountEd25519 =
            generateScaledSecret(acc, partition).getPublicKey().ed25519();
        break;
    case ENVELOPE_TYPE_TX:
        addSignerAndReplaceID(env.v1().tx.sourceAccount);
        break;
    case ENVELOPE_TYPE_TX_FEE_BUMP:
        // Note: handle inner transaction only, outer signatures will be handled
        // separately
        assert(env.feeBump().tx.innerTx.type() == ENVELOPE_TYPE_TX);
        addSignerAndReplaceID(env.feeBump().tx.innerTx.v1().tx.sourceAccount);
        break;
    default:
        throw std::runtime_error("Unknown envelope type");
    }
}

void
TxSimApplyTransactionsWork::mutateOperations(TransactionEnvelope& env,
                                             AbstractLedgerTxn& ltx,
                                             std::set<SecretKey>& keys,
                                             uint32_t partition)
{
    // Maps PoolID from ChangeTrustOp to the LiquidityPoolParameters so we can
    // scale poolIds for pools created and deposited into in the same ledger
    UnorderedMap<PoolID, LiquidityPoolParameters> ctPoolIdToParam;

    auto& ops = txbridge::getOperations(env);
    auto const& sigs = txbridge::getSignaturesInner(env);

    for (auto& op : ops)
    {
        // Add signer keys where needed before simulating the operation
        if (op.sourceAccount)
        {
            addSignerKeys(*op.sourceAccount, ltx, keys, sigs, getInnerTxHash(),
                          partition);
        }
        mutateScaledOperation(op, ltx, ctPoolIdToParam, partition);
    }
}

size_t
TxSimApplyTransactionsWork::scaleLedger(
    std::vector<TransactionEnvelope>& transactions,
    std::vector<TransactionResultPair>& results,
    std::vector<UpgradeType>& upgrades, uint32_t partition)
{
    assert(mTransactionIter != mTransactionHistory.cend());
    assert(mResultIter != mResultHistory.txResultSet.results.cend());

    auto const& env = mUpgradeProtocol
                          ? txbridge::convertForV13(*mTransactionIter)
                          : *mTransactionIter;
    TransactionEnvelope newEnv = env;

    // No mutation needed, simply return existing transactions and results
    if (partition == 0)
    {
        transactions.emplace_back(newEnv);
        results.emplace_back(*mResultIter);
        return txbridge::getOperations(newEnv).size();
    }

    // Keep track of accounts that need to sign
    std::set<SecretKey> keys;

    // First, update transaction source accounts
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    mutateTxSourceAccounts(newEnv, ltx, keys, partition);

    mutateOperations(newEnv, ltx, keys, partition);

    auto simulateSigs = [&](xdr::xvector<DecoratedSignature, 20>& sigs,
                            std::set<SecretKey> const& keys,
                            bool useInnerHash) {
        Hash hash;
        if (newEnv.type() == ENVELOPE_TYPE_TX_FEE_BUMP)
        {
            auto txFrame = std::make_shared<FeeBumpTransactionFrame>(
                mApp.getNetworkID(), newEnv);
            if (useInnerHash)
            {
                auto innerTxFrame = std::make_shared<TransactionFrame>(
                    mApp.getNetworkID(), txFrame->convertInnerTxToV1(newEnv));
                hash = innerTxFrame->getContentsHash();
            }
            else
            {
                hash = txFrame->getContentsHash();
            }
        }
        else
        {
            auto txFrame = TransactionFrameBase::makeTransactionFromWire(
                mApp.getNetworkID(), newEnv);
            hash = txFrame->getContentsHash();
        }

        sigs.clear();
        std::transform(
            keys.begin(), keys.end(), std::back_inserter(sigs),
            [&](SecretKey const& k) { return SignatureUtils::sign(k, hash); });
        return hash;
    };

    // Handle v0 and v1 tx signatures, or fee-bump inner tx
    // Note: for fee-bump transactions, set inner tx signatures first
    // to ensure the right hash
    auto newTxHash =
        simulateSigs(txbridge::getSignaturesInner(newEnv), keys, true);

    // Second, if fee-bump tx, handle outer tx signatures
    if (newEnv.type() == ENVELOPE_TYPE_TX_FEE_BUMP)
    {
        std::set<SecretKey> outerTxKeys;
        auto& outerSigs = newEnv.feeBump().signatures;
        addSignerKeys(
            newEnv.feeBump().tx.feeSource, ltx, outerTxKeys, outerSigs,
            mResultIter->result.result.innerResultPair().transactionHash,
            partition);
        mutateScaledAccountID(newEnv.feeBump().tx.feeSource, partition);
        newTxHash = simulateSigs(outerSigs, outerTxKeys, false);
    }
    else if (env.type() == ENVELOPE_TYPE_TX &&
             env.v1().tx.cond.type() == PRECOND_V2)
    {
        newEnv.v1().tx.cond.v2().extraSigners.clear();
        for (auto const& signerKey : env.v1().tx.cond.v2().extraSigners)
        {
            if (signerKey.type() == SIGNER_KEY_TYPE_ED25519)
            {
                newEnv.v1().tx.cond.v2().extraSigners.emplace_back(
                    generateScaledEd25519Signer(signerKey, partition));
            }
        }
    }

    // These are not exactly accurate, but sufficient to check result codes
    auto newRes = *mResultIter;
    newRes.transactionHash = newTxHash;

    results.emplace_back(newRes);
    transactions.emplace_back(newEnv);

    return txbridge::getOperations(newEnv).size();
}

bool
TxSimApplyTransactionsWork::getNextLedgerFromHistoryArchive()
{
    TransactionHistoryEntry txHistoryEntry;
    if (mStream->getNextLedger(mHeaderHistory, txHistoryEntry, mResultHistory))
    {
        // Derive transaction apply order from the results
        UnorderedMap<Hash, TransactionEnvelope const*> transactions;
        TxSetFrameConstPtr txSetFrame;
        if (txHistoryEntry.ext.v() == 1)
        {
            txSetFrame = TxSetFrame::makeFromWire(
                mNetworkID, txHistoryEntry.ext.generalizedTxSet());
        }
        else
        {
            txSetFrame =
                TxSetFrame::makeFromWire(mNetworkID, txHistoryEntry.txSet);
        }
        for (auto const& txFrame : txSetFrame->getTxsInHashOrder())
        {
            transactions[txFrame->getContentsHash()] = &txFrame->getEnvelope();
        }

        mTransactionHistory.clear();
        for (auto const& result : mResultHistory.txResultSet.results)
        {
            auto it = transactions.find(result.transactionHash);
            assert(it != transactions.end());
            mTransactionHistory.emplace_back(*it->second);
        }
        mTransactionIter = mTransactionHistory.cbegin();
        mResultIter = mResultHistory.txResultSet.results.cbegin();
        return true;
    }
    return false;
}

bool
TxSimApplyTransactionsWork::getNextLedger(
    std::vector<TransactionEnvelope>& transactions,
    std::vector<TransactionResultPair>& results,
    std::vector<UpgradeType>& upgrades)
{
    transactions.clear();
    results.clear();
    upgrades.clear();

    if (mTransactionIter == mTransactionHistory.cend())
    {
        if (!getNextLedgerFromHistoryArchive())
        {
            return false;
        }
    }

    size_t nOps = 0;
    while (true)
    {
        // sustained: mMaxOperations > 0,
        // scaled ledger: avoid checking nOps < mMaxOperations, mMaxOperations
        // = 0
        while (mTransactionIter != mTransactionHistory.cend() &&
               (mMaxOperations == 0 || (nOps < mMaxOperations)))
        {
            for (uint32_t partition = 0; partition < mMultiplier; partition++)
            {
                nOps += scaleLedger(transactions, results, upgrades, partition);
            }

            ++mTransactionIter;
            ++mResultIter;
        }

        if (mTransactionIter != mTransactionHistory.cend() ||
            mMaxOperations == 0)
        {
            return true;
        }

        if (!getNextLedgerFromHistoryArchive())
        {
            return true;
        }

        upgrades = mHeaderHistory.header.scpValue.upgrades;
        upgrades.erase(
            std::remove_if(upgrades.begin(), upgrades.end(),
                           [](auto const& opaqueUpgrade) {
                               LedgerUpgrade upgrade;
                               xdr::xdr_from_opaque(opaqueUpgrade, upgrade);
                               return (upgrade.type() ==
                                       LEDGER_UPGRADE_MAX_TX_SET_SIZE);
                           }),
            upgrades.end());
        if (!upgrades.empty())
        {
            return true;
        }
    }
}

void
TxSimApplyTransactionsWork::onReset()
{
    // Upgrade max transaction set size if necessary
    auto& lm = mApp.getLedgerManager();
    auto const& lclHeader = lm.getLastClosedLedgerHeader();
    auto const& header = lclHeader.header;

    // If ledgerVersion < 11 then we need to support at least mMaxOperations
    // transactions to guarantee we can support mMaxOperations operations no
    // matter how they are distributed (worst case one per transaction).
    //
    // If ledgerVersion >= 11 then we need to support at least mMaxOperations
    // operations.
    //
    // So we can do the same upgrade in both cases.
    if (header.maxTxSetSize < mMaxOperations || mUpgradeProtocol)
    {
        StellarValue sv;
        if (header.maxTxSetSize < mMaxOperations)
        {
            LedgerUpgrade upgrade(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
            upgrade.newMaxTxSetSize() = mMaxOperations;
            auto opaqueUpgrade = xdr::xdr_to_opaque(upgrade);
            sv.upgrades.emplace_back(opaqueUpgrade.begin(),
                                     opaqueUpgrade.end());
        }
        if (mUpgradeProtocol)
        {
            LedgerUpgrade upgrade(LEDGER_UPGRADE_VERSION);
            upgrade.newLedgerVersion() =
                mApp.getConfig().LEDGER_PROTOCOL_VERSION;
            auto opaqueUpgrade = xdr::xdr_to_opaque(upgrade);
            sv.upgrades.emplace_back(opaqueUpgrade.begin(),
                                     opaqueUpgrade.end());
        }

        TxSetFrameConstPtr txSet = TxSetFrame::makeEmpty(lclHeader);

        sv.txSetHash = txSet->getContentsHash();
        sv.closeTime = mHeaderHistory.header.scpValue.closeTime;

        LedgerCloseData closeData(header.ledgerSeq + 1, txSet, sv);
        lm.closeLedger(closeData);
    }

    // Prepare the HistoryArchiveStream
    mStream = std::make_unique<HistoryArchiveStream>(mDownloadDir, mRange,
                                                     mApp.getHistoryManager());
    mApplyLedgerWork.reset();
    mResults.clear();
}

BasicWork::State
TxSimApplyTransactionsWork::onRun()
{
    auto& lm = mApp.getLedgerManager();
    auto const& lclHeader = lm.getLastClosedLedgerHeader();
    auto const& header = lclHeader.header;

    if (mApplyLedgerWork)
    {
        mApplyLedgerWork->crankWork();
        if (mApplyLedgerWork->getState() != State::WORK_SUCCESS)
        {
            return mApplyLedgerWork->getState();
        }
        else
        {
            if (mVerifyResults)
            {
                checkResults(mApp, header.ledgerSeq, mResults);
            }
            auto applied = lm.getLastClosedLedgerNum() - mRange.mFirst + 1;
            if (applied == CLEAR_METRICS_AFTER_NUM_LEDGERS)
            {
                std::string domain;
                mApp.clearMetrics(domain);
            }
        }
    }

    std::vector<TransactionEnvelope> transactions;
    std::vector<UpgradeType> upgrades;
    mResults.clear();

    try
    {
        if (!getNextLedger(transactions, mResults, upgrades))
        {
            return State::WORK_SUCCESS;
        }
    }
    catch (xdr::xdr_bad_message_size&)
    {
        CLOG_ERROR(History,
                   "Failed to read xdr. Most likely a version mismatch");
        return State::WORK_FAILURE;
    }

    // When creating SimulationTxSetFrame, we only want to use mMultiplier when
    // generating transactions to handle offer creation (mapping created offer
    // id to a simulated one). When simulating pre-generated transactions, we
    // already have relevant offer ids in transaction results
    auto txSet = makeSimTxSetFrame(mNetworkID, lclHeader, transactions,
                                   mResults, mMultiplier);

    StellarValue sv;
    sv.txSetHash = txSet->getContentsHash();
    sv.closeTime = mHeaderHistory.header.scpValue.closeTime;
    sv.upgrades.insert(sv.upgrades.begin(), upgrades.begin(), upgrades.end());

    LedgerCloseData closeData(header.ledgerSeq + 1, txSet, sv);
    auto applyLedger = std::make_shared<ApplyLedgerWork>(mApp, closeData);

    bool waitForPublish = false;
    auto condition = [waitForPublish](Application& app) mutable {
        auto const& lm = app.getLedgerManager();
        auto const& ham = app.getHistoryArchiveManager();
        auto const& hm = app.getHistoryManager();
        auto proceed = true;
        if (ham.hasAnyWritableHistoryArchive())
        {
            auto lcl = lm.getLastClosedLedgerNum();
            if (hm.isFirstLedgerInCheckpoint(lcl))
            {
                auto queueLen = hm.publishQueueLength();
                if (queueLen <= CatchupWork::PUBLISH_QUEUE_UNBLOCK_APPLICATION)
                {
                    waitForPublish = false;
                }
                else if (queueLen > CatchupWork::PUBLISH_QUEUE_MAX_SIZE)
                {
                    waitForPublish = true;
                }
                proceed = !waitForPublish;
            }
        }
        return proceed;
    };

    mApplyLedgerWork = std::make_shared<ConditionalWork>(
        mApp,
        fmt::format(FMT_STRING("simulation-apply-ledger-{:d}"),
                    closeData.getLedgerSeq()),
        condition, applyLedger);

    mApplyLedgerWork->startWork(wakeSelfUpCallback());

    return State::WORK_RUNNING;
}

void
TxSimApplyTransactionsWork::shutdown()
{
    if (mApplyLedgerWork)
    {
        mApplyLedgerWork->shutdown();
    }
    BasicWork::shutdown();
}

bool
TxSimApplyTransactionsWork::onAbort()
{
    if (mApplyLedgerWork && !mApplyLedgerWork->isDone())
    {
        mApplyLedgerWork->crankWork();
        return false;
    }
    return true;
}
}
}
