// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/TxTests.h"
#include "crypto/ByteSlice.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "herder/simulation/TxSimTxSetFrame.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/test/LedgerTestUtils.h"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/OperationFrame.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionSQL.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include "xdrpp/autocheck.h"

#include <lib/catch.hpp>

using namespace stellar;
using namespace stellar::txtest;

namespace stellar
{
namespace txtest
{

ExpectedOpResult::ExpectedOpResult(OperationResultCode code)
{
    mOperationResult.code(code);
}

ExpectedOpResult::ExpectedOpResult(CreateAccountResultCode createAccountCode)
{
    mOperationResult.code(opINNER);
    mOperationResult.tr().type(CREATE_ACCOUNT);
    mOperationResult.tr().createAccountResult().code(createAccountCode);
}

ExpectedOpResult::ExpectedOpResult(PaymentResultCode paymentCode)
{
    mOperationResult.code(opINNER);
    mOperationResult.tr().type(PAYMENT);
    mOperationResult.tr().paymentResult().code(paymentCode);
}

ExpectedOpResult::ExpectedOpResult(AccountMergeResultCode accountMergeCode)
{
    mOperationResult.code(opINNER);
    mOperationResult.tr().type(ACCOUNT_MERGE);
    mOperationResult.tr().accountMergeResult().code(accountMergeCode);
}

ExpectedOpResult::ExpectedOpResult(AccountMergeResultCode accountMergeCode,
                                   int64_t sourceAccountBalance)
{
    if (accountMergeCode != ACCOUNT_MERGE_SUCCESS)
    {
        throw std::logic_error("accountMergeCode must be ACCOUNT_MERGE_SUCCESS "
                               "when sourceAccountBalance is passed");
    }

    mOperationResult.code(opINNER);
    mOperationResult.tr().type(ACCOUNT_MERGE);
    mOperationResult.tr().accountMergeResult().code(ACCOUNT_MERGE_SUCCESS);
    mOperationResult.tr().accountMergeResult().sourceAccountBalance() =
        sourceAccountBalance;
}

ExpectedOpResult::ExpectedOpResult(SetOptionsResultCode setOptionsResultCode)
{
    mOperationResult.code(opINNER);
    mOperationResult.tr().type(SET_OPTIONS);
    mOperationResult.tr().setOptionsResult().code(setOptionsResultCode);
}

TransactionResult
expectedResult(int64_t fee, size_t opsCount, TransactionResultCode code,
               std::vector<ExpectedOpResult> ops)
{
    auto result = TransactionResult{};
    result.feeCharged = fee;
    result.result.code(code);

    if (code != txSUCCESS && code != txFAILED)
    {
        return result;
    }

    if (ops.empty())
    {
        std::fill_n(std::back_inserter(ops), opsCount, PAYMENT_SUCCESS);
    }

    for (auto const& op : ops)
    {
        result.result.results().push_back(op.mOperationResult);
    }

    return result;
}

bool
applyCheck(TransactionFramePtr tx, Application& app, bool checkSeqNum)
{
    // Close the ledger here to advance ledgerSeq
    closeLedger(app);

    LedgerTxn ltx(app.getLedgerTxnRoot());

    auto ledgerVersion = ltx.loadHeader().current().ledgerVersion;

    bool check = false;
    TransactionResult checkResult;
    TransactionResultCode code;
    AccountEntry srcAccountBefore;

    auto checkedTx = TransactionFrameBase::makeTransactionFromWire(
        app.getNetworkID(), tx->getEnvelope());
    bool checkedTxApplyRes = false;
    {
        LedgerTxn ltxFeeProc(ltx);
        // use checkedTx here for validity check as to keep tx untouched
        check = checkedTx->checkValid(app, ltxFeeProc, 0, 0, 0);
        checkResult = checkedTx->getResult();
        REQUIRE((!check || checkResult.result.code() == txSUCCESS));

        // now, check what happens when simulating what happens during a ledger
        // close and reconcile it with the return value of "apply" with the one
        // from checkValid:
        // * an invalid (as per isValid) tx is still invalid during apply (and
        // the same way)
        // * a valid tx can fail later
        code = checkResult.result.code();

        // compute the same changes in parallel on checkedTx
        {
            LedgerTxn ltxCleanTx(ltxFeeProc);
            auto baseFee = ltxCleanTx.loadHeader().current().baseFee;
            if (code != txNO_ACCOUNT)
            {
                checkedTx->processFeeSeqNum(ltxCleanTx, baseFee);
            }
            // else, leave feeCharged as per checkValid
            try
            {
                TransactionMetaFrame cleanTm(
                    ltxCleanTx.loadHeader().current().ledgerVersion);
                checkedTxApplyRes = checkedTx->apply(app, ltxCleanTx, cleanTm);
            }
            catch (...)
            {
                checkedTx->getResult().result.code(txINTERNAL_ERROR);
            }
            // do not commit this one
        }

        if (code != txNO_ACCOUNT)
        {
            srcAccountBefore = loadAccount(ltxFeeProc, tx->getSourceID(), true)
                                   .current()
                                   .data.account();

            // no account -> can't process the fee
            auto baseFee = ltxFeeProc.loadHeader().current().baseFee;
            tx->processFeeSeqNum(ltxFeeProc, baseFee);
            // check that the recommended fee is correct, ignore the difference
            // for later
            if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_11))
            {
                REQUIRE(checkResult.feeCharged >= tx->getResult().feeCharged);
            }
            // this is to ignore potential changes in feeCharged
            // as `checkValid` returns an estimate
            checkResult.feeCharged = tx->getResult().feeCharged;

            // verify that the fee got processed
            auto ltxDelta = ltxFeeProc.getDelta();
            REQUIRE(ltxDelta.entry.size() == 1);
            auto current = ltxDelta.entry.begin()->second.current;
            REQUIRE(current);
            REQUIRE(current->type() == InternalLedgerEntryType::LEDGER_ENTRY);
            auto previous = ltxDelta.entry.begin()->second.previous;
            REQUIRE(previous);
            REQUIRE(previous->type() == InternalLedgerEntryType::LEDGER_ENTRY);
            auto currAcc = current->ledgerEntry().data.account();
            auto prevAcc = previous->ledgerEntry().data.account();
            REQUIRE(prevAcc == srcAccountBefore);
            REQUIRE(currAcc.accountID == tx->getSourceID());
            REQUIRE(currAcc.balance < prevAcc.balance);
            currAcc.balance = prevAcc.balance;
            if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_10))
            {
                // v9 and below, we also need to verify that the sequence number
                // also got processed at this time
                REQUIRE(currAcc.seqNum == prevAcc.seqNum + 1);
                currAcc.seqNum = prevAcc.seqNum;
            }
            REQUIRE(currAcc == prevAcc);
        }
        else
        {
            // this basically makes it that we ignore that field when we
            // don't have an account
            tx->getResult().feeCharged = checkResult.feeCharged;
        }
        ltxFeeProc.commit();
    }

    bool res = false;
    {
        LedgerTxn ltxTx(ltx);
        TransactionMetaFrame tm(ltxTx.loadHeader().current().ledgerVersion);
        try
        {
            res = tx->apply(app, ltxTx, tm);
        }
        catch (...)
        {
            tx->getResult().result.code(txINTERNAL_ERROR);
        }

        // check that both tx and cleanTx behave the same
        REQUIRE(res == checkedTxApplyRes);
        REQUIRE(tx->getEnvelope() == checkedTx->getEnvelope());
        REQUIRE(tx->getResult() == checkedTx->getResult());

        REQUIRE((!res || tx->getResultCode() == txSUCCESS));

        if (!res || tx->getResultCode() != txSUCCESS)
        {
            REQUIRE(tm.getNumOperations() == 0);
        }
        // checks that the failure is the same if pre checks failed
        if (!check)
        {
            if (tx->getResultCode() != txFAILED)
            {
                REQUIRE(checkResult == tx->getResult());
            }
            else
            {
                auto const& txResults = tx->getResult().result.results();
                auto const& checkResults = checkResult.result.results();
                for (auto i = 0u; i < txResults.size(); i++)
                {
                    REQUIRE(checkResults[i] == txResults[i]);
                    if (checkResults[i].code() == opBAD_AUTH)
                    {
                        // results may not match after first opBAD_AUTH
                        break;
                    }
                }
            }
        }

        if (code != txNO_ACCOUNT)
        {
            auto srcAccountAfter =
                loadAccount(ltxTx, srcAccountBefore.accountID, false);
            if (srcAccountAfter)
            {
                bool earlyFailure =
                    (code == txMISSING_OPERATION || code == txTOO_EARLY ||
                     code == txTOO_LATE || code == txINSUFFICIENT_FEE ||
                     code == txBAD_SEQ || code == txMALFORMED);
                // verify that the sequence number changed (v10+)
                // do not perform the check if there was a failure before
                // or during the sequence number processing
                auto header = ltxTx.loadHeader();
                if (checkSeqNum &&
                    protocolVersionStartsFrom(ledgerVersion,
                                              ProtocolVersion::V_10) &&
                    !earlyFailure)
                {
                    REQUIRE(srcAccountAfter.current().data.account().seqNum ==
                            (srcAccountBefore.seqNum + 1));
                }
                // on failure, no other changes should have been made
                if (!res)
                {
                    bool noChangeOnEarlyFailure =
                        earlyFailure &&
                        protocolVersionIsBefore(ledgerVersion,
                                                ProtocolVersion::V_13);
                    if (noChangeOnEarlyFailure ||
                        protocolVersionIsBefore(ledgerVersion,
                                                ProtocolVersion::V_10))
                    {
                        // no changes during an early failure
                        REQUIRE(ltxTx.getDelta().entry.empty());
                    }
                    else
                    {
                        auto ltxDelta = ltxTx.getDelta();
                        for (auto const& kvp : ltxDelta.entry)
                        {
                            auto current = kvp.second.current;
                            REQUIRE(current);
                            REQUIRE(current->type() ==
                                    InternalLedgerEntryType::LEDGER_ENTRY);
                            auto previous = kvp.second.previous;
                            REQUIRE(previous);
                            REQUIRE(previous->type() ==
                                    InternalLedgerEntryType::LEDGER_ENTRY);

                            // From V13, it's possible to remove one-time
                            // signers on early failures
                            if (protocolVersionStartsFrom(
                                    ledgerVersion, ProtocolVersion::V_13) &&
                                earlyFailure)
                            {
                                auto currAcc =
                                    current->ledgerEntry().data.account();
                                auto prevAcc =
                                    previous->ledgerEntry().data.account();
                                REQUIRE(currAcc.signers.size() + 1 ==
                                        prevAcc.signers.size());
                                REQUIRE(hasAccountEntryExtV2(currAcc) ==
                                        hasAccountEntryExtV2(prevAcc));

                                // signers should be the only change so this
                                // should make the accounts equivalent
                                if (hasAccountEntryExtV2(currAcc))
                                {
                                    auto& currSignerSponsoringIDs =
                                        getAccountEntryExtensionV2(currAcc)
                                            .signerSponsoringIDs;
                                    auto const& prevSignerSponsoringIDs =
                                        getAccountEntryExtensionV2(prevAcc)
                                            .signerSponsoringIDs;

                                    REQUIRE(currSignerSponsoringIDs.size() +
                                                1 ==
                                            prevSignerSponsoringIDs.size());
                                    currSignerSponsoringIDs =
                                        prevSignerSponsoringIDs;
                                }
                                currAcc.signers = prevAcc.signers;
                                currAcc.numSubEntries = prevAcc.numSubEntries;
                                REQUIRE(currAcc == prevAcc);
                            }
                        }
                        // could check more here if needed
                    }
                }
            }
        }
        ltxTx.commit();
        recordOrCheckGlobalTestTxMetadata(tm.getXDR());
    }

    ltx.commit();

    return res;
}

void
checkTransaction(TransactionFrame& txFrame, Application& app)
{
    REQUIRE(txFrame.getResult().feeCharged ==
            app.getLedgerManager().getLastTxFee());
    REQUIRE((txFrame.getResultCode() == txSUCCESS ||
             txFrame.getResultCode() == txFAILED));
}

void
applyTx(TransactionFramePtr const& tx, Application& app, bool checkSeqNum)
{
    applyCheck(tx, app, checkSeqNum);
    throwIf(tx->getResult());
    checkTransaction(*tx, app);

    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto account = stellar::loadAccount(ltx, tx->getSourceID());
    if (protocolVersionStartsFrom(ltx.loadHeader().current().ledgerVersion,
                                  ProtocolVersion::V_19) &&
        account)
    {
        auto const& v3 =
            getAccountEntryExtensionV3(account.current().data.account());
        REQUIRE(v3.seqLedger == ltx.loadHeader().current().ledgerSeq);
        REQUIRE(v3.seqTime == ltx.loadHeader().current().scpValue.closeTime);
    }
}

void
validateTxResults(TransactionFramePtr const& tx, Application& app,
                  ValidationResult validationResult,
                  TransactionResult const& applyResult)
{
    auto shouldValidateOk = validationResult.code == txSUCCESS;

    auto checkedTx = TransactionFrameBase::makeTransactionFromWire(
        app.getNetworkID(), tx->getEnvelope());
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        REQUIRE(checkedTx->checkValid(app, ltx, 0, 0, 0) == shouldValidateOk);
    }
    REQUIRE(checkedTx->getResult().result.code() == validationResult.code);
    REQUIRE(checkedTx->getResult().feeCharged == validationResult.fee);

    // do not try to apply if checkValid returned false
    if (!shouldValidateOk)
    {
        REQUIRE(applyResult == TransactionResult{});
        return;
    }

    switch (applyResult.result.code())
    {
    case txBAD_AUTH_EXTRA:
    case txBAD_SEQ:
        return;
    default:
        break;
    }

    auto shouldApplyOk = applyResult.result.code() == txSUCCESS;
    auto applyOk = applyCheck(tx, app);
    REQUIRE(tx->getResult() == applyResult);
    REQUIRE(applyOk == shouldApplyOk);
};

void
checkLiquidityPool(Application& app, PoolID const& poolID, int64_t reserveA,
                   int64_t reserveB, int64_t totalPoolShares,
                   int64_t poolSharesTrustLineCount)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto lp = loadLiquidityPool(ltx, poolID);
    REQUIRE(lp);
    auto const& cp = lp.current().data.liquidityPool().body.constantProduct();
    REQUIRE(cp.reserveA == reserveA);
    REQUIRE(cp.reserveB == reserveB);
    REQUIRE(cp.totalPoolShares == totalPoolShares);
    REQUIRE(cp.poolSharesTrustLineCount == poolSharesTrustLineCount);
}

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, int day, int month, int year,
              std::vector<TransactionFrameBasePtr> const& txs, bool strictOrder)
{
    return closeLedgerOn(app, ledgerSeq, getTestDate(day, month, year), txs,
                         strictOrder);
}

TxSetResultMeta
closeLedgerOn(Application& app, int day, int month, int year,
              std::vector<TransactionFrameBasePtr> const& txs, bool strictOrder)
{
    auto nextLedgerSeq = app.getLedgerManager().getLastClosedLedgerNum() + 1;
    return closeLedgerOn(app, nextLedgerSeq, getTestDate(day, month, year), txs,
                         strictOrder);
}

TxSetResultMeta
closeLedger(Application& app, std::vector<TransactionFrameBasePtr> const& txs,
            bool strictOrder)
{
    auto lastCloseTime = app.getLedgerManager()
                             .getLastClosedLedgerHeader()
                             .header.scpValue.closeTime;

    auto nextLedgerSeq = app.getLedgerManager().getLastClosedLedgerNum() + 1;

    return closeLedgerOn(app, nextLedgerSeq, lastCloseTime, txs, strictOrder);
}

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, TimePoint closeTime,
              std::vector<TransactionFrameBasePtr> const& txs, bool strictOrder)
{
    auto lastCloseTime = app.getLedgerManager()
                             .getLastClosedLedgerHeader()
                             .header.scpValue.closeTime;
    if (closeTime < lastCloseTime)
    {
        closeTime = lastCloseTime;
    }

    TxSetFrameConstPtr txSet;
    if (strictOrder)
    {
        txSet = std::make_shared<txsimulation::SimApplyOrderTxSetFrame const>(
            app.getLedgerManager().getLastClosedLedgerHeader(), txs);
    }
    else
    {
        txSet = TxSetFrame::makeFromTransactions(txs, app, 0, 0);
    }
    if (!strictOrder)
    {
        // `strictOrder` means the txs in the txSet will be applied in the exact
        // same order as they were constructed. It could also imply the txs
        // themselves maybe intentionally invalid for testing purpose.
        REQUIRE(txSet->checkValid(app, 0, 0));
    }

    app.getHerder().externalizeValue(txSet, ledgerSeq, closeTime,
                                     emptyUpgradeSteps);

    auto z1 = getTransactionHistoryResults(app.getDatabase(), ledgerSeq);
    auto z2 = getTransactionFeeMeta(app.getDatabase(), ledgerSeq);

    REQUIRE(app.getLedgerManager().getLastClosedLedgerNum() == ledgerSeq);

    TxSetResultMeta res;
    std::transform(
        z1.results.begin(), z1.results.end(), z2.begin(),
        std::back_inserter(res),
        [](TransactionResultPair const& r1, LedgerEntryChanges const& r2) {
            return std::make_pair(r1, r2);
        });

    return res;
}

TxSetResultMeta
closeLedger(Application& app, TxSetFrameConstPtr txSet)
{
    auto lastCloseTime = app.getLedgerManager()
                             .getLastClosedLedgerHeader()
                             .header.scpValue.closeTime;
    auto nextLedgerSeq = app.getLedgerManager().getLastClosedLedgerNum() + 1;
    return closeLedgerOn(app, nextLedgerSeq, lastCloseTime, txSet);
}

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, time_t closeTime,
              TxSetFrameConstPtr txSet)
{
    app.getHerder().externalizeValue(txSet, ledgerSeq, closeTime,
                                     emptyUpgradeSteps);

    auto z1 = getTransactionHistoryResults(app.getDatabase(), ledgerSeq);
    auto z2 = getTransactionFeeMeta(app.getDatabase(), ledgerSeq);

    REQUIRE(app.getLedgerManager().getLastClosedLedgerNum() == ledgerSeq);

    TxSetResultMeta res;
    std::transform(
        z1.results.begin(), z1.results.end(), z2.begin(),
        std::back_inserter(res),
        [](TransactionResultPair const& r1, LedgerEntryChanges const& r2) {
            return std::make_pair(r1, r2);
        });

    return res;
}

SecretKey
getRoot(Hash const& networkID)
{
    return SecretKey::fromSeed(networkID);
}

SecretKey
getAccount(std::string const& n)
{
    // stretch seed to 32 bytes
    std::string seed(n);
    while (seed.size() < 32)
        seed += '.';
    return SecretKey::fromSeed(seed);
}

Signer
makeSigner(SecretKey key, int weight)
{
    return Signer{KeyUtils::convertKey<SignerKey>(key.getPublicKey()), weight};
}

ConstLedgerTxnEntry
loadAccount(AbstractLedgerTxn& ltx, PublicKey const& k, bool mustExist)
{
    auto res = stellar::loadAccountWithoutRecord(ltx, k);
    if (mustExist)
    {
        REQUIRE(res);
    }
    return res;
}

bool
doesAccountExist(Application& app, PublicKey const& k)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    return (bool)stellar::loadAccountWithoutRecord(ltx, k);
}

xdr::xvector<Signer, 20>
getAccountSigners(PublicKey const& k, Application& app)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto account = stellar::loadAccount(ltx, k);
    return account.current().data.account().signers;
}

TransactionFramePtr
transactionFromOperationsV0(Application& app, SecretKey const& from,
                            SequenceNumber seq,
                            const std::vector<Operation>& ops, uint32_t fee)
{
    TransactionEnvelope e(ENVELOPE_TYPE_TX_V0);
    e.v0().tx.sourceAccountEd25519 = from.getPublicKey().ed25519();
    e.v0().tx.fee =
        fee != 0 ? fee
                 : static_cast<uint32_t>(
                       (ops.size() * app.getLedgerManager().getLastTxFee()) &
                       UINT32_MAX);
    e.v0().tx.seqNum = seq;
    std::copy(std::begin(ops), std::end(ops),
              std::back_inserter(e.v0().tx.operations));

    auto res = std::static_pointer_cast<TransactionFrame>(
        TransactionFrameBase::makeTransactionFromWire(app.getNetworkID(), e));
    res->addSignature(from);
    return res;
}

TransactionFramePtr
transactionFromOperationsV1(Application& app, SecretKey const& from,
                            SequenceNumber seq,
                            const std::vector<Operation>& ops, uint32_t fee,
                            std::optional<PreconditionsV2> cond)
{
    TransactionEnvelope e(ENVELOPE_TYPE_TX);
    e.v1().tx.sourceAccount = toMuxedAccount(from.getPublicKey());
    e.v1().tx.fee =
        fee != 0 ? fee
                 : static_cast<uint32_t>(
                       (ops.size() * app.getLedgerManager().getLastTxFee()) &
                       UINT32_MAX);
    e.v1().tx.seqNum = seq;
    std::copy(std::begin(ops), std::end(ops),
              std::back_inserter(e.v1().tx.operations));

    if (cond)
    {
        e.v1().tx.cond.type(PRECOND_V2);
        e.v1().tx.cond.v2() = *cond;
    }

    auto res = std::static_pointer_cast<TransactionFrame>(
        TransactionFrameBase::makeTransactionFromWire(app.getNetworkID(), e));
    res->addSignature(from);
    return res;
}

TransactionFramePtr
transactionFromOperations(Application& app, SecretKey const& from,
                          SequenceNumber seq, const std::vector<Operation>& ops,
                          uint32_t fee)
{
    uint32_t ledgerVersion;
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    }
    if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_13))
    {
        return transactionFromOperationsV0(app, from, seq, ops, fee);
    }
    return transactionFromOperationsV1(app, from, seq, ops, fee);
}

TransactionFramePtr
transactionWithV2Precondition(Application& app, TestAccount& account,
                              int64_t sequenceDelta, uint32_t fee,
                              PreconditionsV2 const& cond)
{
    return transactionFromOperationsV1(
        app, account, account.getLastSequenceNumber() + sequenceDelta,
        {payment(account.getPublicKey(), 1)}, fee, cond);
}

TransactionFrameBasePtr
feeBump(Application& app, TestAccount& feeSource, TransactionFrameBasePtr tx,
        int64_t fee)
{
    REQUIRE(tx->getEnvelope().type() == ENVELOPE_TYPE_TX);
    TransactionEnvelope fb(ENVELOPE_TYPE_TX_FEE_BUMP);
    fb.feeBump().tx.feeSource = toMuxedAccount(feeSource);
    fb.feeBump().tx.fee = fee;
    fb.feeBump().tx.innerTx.type(ENVELOPE_TYPE_TX);
    fb.feeBump().tx.innerTx.v1() = tx->getEnvelope().v1();

    auto hash = sha256(xdr::xdr_to_opaque(
        app.getNetworkID(), ENVELOPE_TYPE_TX_FEE_BUMP, fb.feeBump().tx));
    fb.feeBump().signatures.emplace_back(SignatureUtils::sign(feeSource, hash));
    return TransactionFrameBase::makeTransactionFromWire(app.getNetworkID(),
                                                         fb);
}

Operation
changeTrust(Asset const& asset, int64_t limit)
{
    return changeTrust(assetToChangeTrustAsset(asset), limit);
}

Operation
changeTrust(ChangeTrustAsset const& asset, int64_t limit)
{
    Operation op;

    op.body.type(CHANGE_TRUST);
    op.body.changeTrustOp().limit = limit;
    op.body.changeTrustOp().line = asset;

    return op;
}

Operation
allowTrust(PublicKey const& trustor, Asset const& asset, uint32_t authorize)
{
    Operation op;

    op.body.type(ALLOW_TRUST);
    op.body.allowTrustOp().trustor = trustor;
    op.body.allowTrustOp().asset.type(asset.type());

    if (asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        op.body.allowTrustOp().asset.assetCode4() = asset.alphaNum4().assetCode;
    }
    else
    {
        op.body.allowTrustOp().asset.assetCode12() =
            asset.alphaNum12().assetCode;
    }

    op.body.allowTrustOp().authorize = authorize;

    return op;
}

Operation
createAccount(PublicKey const& dest, int64_t amount)
{
    Operation op;
    op.body.type(CREATE_ACCOUNT);
    op.body.createAccountOp().startingBalance = amount;
    op.body.createAccountOp().destination = dest;
    return op;
}

Operation
payment(PublicKey const& to, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().destination = toMuxedAccount(to);
    op.body.paymentOp().asset.type(ASSET_TYPE_NATIVE);
    return op;
}

Operation
payment(PublicKey const& to, Asset const& asset, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().destination = toMuxedAccount(to);
    op.body.paymentOp().asset = asset;
    return op;
}

TransactionFramePtr
createPaymentTx(Application& app, SecretKey const& from, PublicKey const& to,
                SequenceNumber seq, int64_t amount)
{
    return transactionFromOperations(app, from, seq, {payment(to, amount)});
}

TransactionFramePtr
createCreditPaymentTx(Application& app, SecretKey const& from,
                      PublicKey const& to, Asset const& asset,
                      SequenceNumber seq, int64_t amount)
{
    auto op = payment(to, asset, amount);
    return transactionFromOperations(app, from, seq, {op});
}

TransactionFramePtr
createSimpleDexTx(Application& app, TestAccount& account, uint32 nbOps,
                  uint32_t fee)
{
    std::vector<Operation> ops;
    Asset asset1(ASSET_TYPE_NATIVE);
    Asset asset2(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(asset2.alphaNum4().assetCode, "USD");
    REQUIRE(nbOps > 0);
    uint32 nonDexOps = autocheck::generator<uint32>()(nbOps - 1);
    for (uint32 i = 0; i < nbOps - nonDexOps; ++i)
    {
        ops.emplace_back(
            manageBuyOffer(i + 1, asset1, asset2, Price{2, 5}, 10));
    }
    for (uint32 i = nbOps - nonDexOps; i < nbOps; ++i)
    {
        ops.emplace_back(payment(account.getPublicKey(), 1000));
    }
    stellar::shuffle(ops.begin(), ops.end(), autocheck::rng());
    return transactionFromOperations(app, account, account.nextSequenceNumber(),
                                     ops, fee);
}

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
TransactionFramePtr
createSimpleDeployContractTx(Application& app, TestAccount& account,
                             uint32_t fee, uint32_t refundableFee,
                             SorobanResources resources,
                             std::optional<std::string> memo)
{
    Operation deployOp;
    deployOp.body.type(INVOKE_HOST_FUNCTION);
    auto& uploadHF = deployOp.body.invokeHostFunctionOp().hostFunction;
    uploadHF.type(HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);
    uploadHF.wasm().resize(1000);
    auto byteDistr = uniform_int_distribution<uint8_t>();
    std::generate(uploadHF.wasm().begin(), uploadHF.wasm().end(),
                  [&byteDistr]() { return byteDistr(gRandomEngine); });

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = xdrSha256(uploadHF.wasm());

    resources.footprint.readWrite = {contractCodeLedgerKey};

    auto tx =
        sorobanTransactionFrameFromOps(app.getNetworkID(), account, {deployOp},
                                       {}, resources, fee, refundableFee, memo);
    return std::dynamic_pointer_cast<TransactionFrame>(tx);
}
#endif

Asset
makeNativeAsset()
{
    Asset asset;
    asset.type(ASSET_TYPE_NATIVE);
    return asset;
}

Asset
makeInvalidAsset()
{
    Asset asset;
    asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    return asset;
}

Asset
makeAsset(SecretKey const& issuer, std::string const& code)
{
    Asset asset;
    asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    asset.alphaNum4().issuer = issuer.getPublicKey();
    strToAssetCode(asset.alphaNum4().assetCode, code);
    return asset;
}

Asset
makeAssetAlphanum12(SecretKey const& issuer, std::string const& code)
{
    Asset asset;
    asset.type(ASSET_TYPE_CREDIT_ALPHANUM12);
    asset.alphaNum12().issuer = issuer.getPublicKey();
    strToAssetCode(asset.alphaNum12().assetCode, code);
    return asset;
}

ChangeTrustAsset
makeChangeTrustAssetPoolShare(Asset const& assetA, Asset const& assetB,
                              int32_t fee)
{
    REQUIRE(assetA < assetB);
    ChangeTrustAsset poolAsset;
    poolAsset.type(ASSET_TYPE_POOL_SHARE);
    poolAsset.liquidityPool().constantProduct().assetA = assetA;
    poolAsset.liquidityPool().constantProduct().assetB = assetB;
    poolAsset.liquidityPool().constantProduct().fee = fee;
    return poolAsset;
}

Operation
pathPayment(PublicKey const& to, Asset const& sendCur, int64_t sendMax,
            Asset const& destCur, int64_t destAmount,
            std::vector<Asset> const& path)
{
    Operation op;
    op.body.type(PATH_PAYMENT_STRICT_RECEIVE);
    PathPaymentStrictReceiveOp& ppop = op.body.pathPaymentStrictReceiveOp();
    ppop.sendAsset = sendCur;
    ppop.sendMax = sendMax;
    ppop.destAsset = destCur;
    ppop.destAmount = destAmount;
    ppop.destination = toMuxedAccount(to);
    std::copy(std::begin(path), std::end(path), std::back_inserter(ppop.path));

    return op;
}

Operation
pathPaymentStrictSend(PublicKey const& to, Asset const& sendCur,
                      int64_t sendAmount, Asset const& destCur, int64_t destMin,
                      std::vector<Asset> const& path)
{
    Operation op;
    op.body.type(PATH_PAYMENT_STRICT_SEND);
    PathPaymentStrictSendOp& ppop = op.body.pathPaymentStrictSendOp();
    ppop.sendAsset = sendCur;
    ppop.sendAmount = sendAmount;
    ppop.destAsset = destCur;
    ppop.destMin = destMin;
    ppop.destination = toMuxedAccount(to);
    std::copy(std::begin(path), std::end(path), std::back_inserter(ppop.path));

    return op;
}

Operation
createPassiveOffer(Asset const& selling, Asset const& buying,
                   Price const& price, int64_t amount)
{
    Operation op;
    op.body.type(CREATE_PASSIVE_SELL_OFFER);
    op.body.createPassiveSellOfferOp().amount = amount;
    op.body.createPassiveSellOfferOp().selling = selling;
    op.body.createPassiveSellOfferOp().buying = buying;
    op.body.createPassiveSellOfferOp().price = price;

    return op;
}

Operation
manageOffer(int64 offerId, Asset const& selling, Asset const& buying,
            Price const& price, int64_t amount)
{
    Operation op;
    op.body.type(MANAGE_SELL_OFFER);
    op.body.manageSellOfferOp().amount = amount;
    op.body.manageSellOfferOp().selling = selling;
    op.body.manageSellOfferOp().buying = buying;
    op.body.manageSellOfferOp().offerID = offerId;
    op.body.manageSellOfferOp().price = price;

    return op;
}

Operation
manageBuyOffer(int64 offerId, Asset const& selling, Asset const& buying,
               Price const& price, int64_t amount)
{
    Operation op;
    op.body.type(MANAGE_BUY_OFFER);
    op.body.manageBuyOfferOp().buyAmount = amount;
    op.body.manageBuyOfferOp().selling = selling;
    op.body.manageBuyOfferOp().buying = buying;
    op.body.manageBuyOfferOp().offerID = offerId;
    op.body.manageBuyOfferOp().price = price;

    return op;
}

static ManageSellOfferResult
applyCreateOfferHelper(Application& app, int64 offerId, SecretKey const& source,
                       Asset const& selling, Asset const& buying,
                       Price const& price, int64_t amount, SequenceNumber seq)
{
    auto getIdPool = [&]() {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        return ltx.loadHeader().current().idPool;
    };
    auto lastGeneratedID = getIdPool();
    auto expectedOfferID = lastGeneratedID + 1;
    if (offerId != 0)
    {
        expectedOfferID = offerId;
    }

    auto op = manageOffer(offerId, selling, buying, price, amount);
    auto tx = transactionFromOperations(app, source, seq, {op});

    try
    {
        applyTx(tx, app);
    }
    catch (...)
    {
        REQUIRE(getIdPool() == lastGeneratedID);
        throw;
    }

    auto& results = tx->getResult().result.results();

    REQUIRE(results.size() == 1);

    auto& manageSellOfferResult = results[0].tr().manageSellOfferResult();

    auto& offerResult = manageSellOfferResult.success().offer;

    switch (offerResult.effect())
    {
    case MANAGE_OFFER_CREATED:
    case MANAGE_OFFER_UPDATED:
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        auto offer =
            stellar::loadOffer(ltx, source.getPublicKey(), expectedOfferID);
        REQUIRE(offer);
        auto& offerEntry = offer.current().data.offer();
        REQUIRE(offerEntry == offerResult.offer());
        REQUIRE(offerEntry.price == price);
        REQUIRE(offerEntry.selling == selling);
        REQUIRE(offerEntry.buying == buying);
    }
    break;
    case MANAGE_OFFER_DELETED:
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        REQUIRE(
            !stellar::loadOffer(ltx, source.getPublicKey(), expectedOfferID));
    }
    break;
    default:
        abort();
    }

    return manageSellOfferResult;
}

int64_t
applyManageOffer(Application& app, int64 offerId, SecretKey const& source,
                 Asset const& selling, Asset const& buying, Price const& price,
                 int64_t amount, SequenceNumber seq,
                 ManageOfferEffect expectedEffect)
{
    ManageSellOfferResult const& createOfferRes = applyCreateOfferHelper(
        app, offerId, source, selling, buying, price, amount, seq);

    auto& success = createOfferRes.success().offer;
    REQUIRE(success.effect() == expectedEffect);
    return success.effect() != MANAGE_OFFER_DELETED ? success.offer().offerID
                                                    : 0;
}

int64_t
applyManageBuyOffer(Application& app, int64 offerId, SecretKey const& source,
                    Asset const& selling, Asset const& buying,
                    Price const& price, int64_t amount, SequenceNumber seq,
                    ManageOfferEffect expectedEffect)
{
    auto getIdPool = [&]() {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        return ltx.loadHeader().current().idPool;
    };
    auto lastGeneratedID = getIdPool();
    auto expectedOfferID = lastGeneratedID + 1;
    if (offerId != 0)
    {
        expectedOfferID = offerId;
    }

    auto op = manageBuyOffer(offerId, selling, buying, price, amount);
    auto tx = transactionFromOperations(app, source, seq, {op});

    try
    {
        applyTx(tx, app);
    }
    catch (...)
    {
        REQUIRE(getIdPool() == lastGeneratedID);
        throw;
    }

    auto& results = tx->getResult().result.results();
    REQUIRE(results.size() == 1);
    auto& manageBuyOfferResult = results[0].tr().manageBuyOfferResult();
    auto& success = manageBuyOfferResult.success().offer;

    REQUIRE(success.effect() == expectedEffect);
    switch (success.effect())
    {
    case MANAGE_OFFER_CREATED:
    case MANAGE_OFFER_UPDATED:
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        auto offer =
            stellar::loadOffer(ltx, source.getPublicKey(), expectedOfferID);
        REQUIRE(offer);
        auto& offerEntry = offer.current().data.offer();
        REQUIRE(offerEntry == success.offer());
        REQUIRE(offerEntry.price == Price{price.d, price.n});
        REQUIRE(offerEntry.selling == selling);
        REQUIRE(offerEntry.buying == buying);
    }
    break;
    case MANAGE_OFFER_DELETED:
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        REQUIRE(
            !stellar::loadOffer(ltx, source.getPublicKey(), expectedOfferID));
    }
    break;
    default:
        abort();
    }

    return success.effect() != MANAGE_OFFER_DELETED ? success.offer().offerID
                                                    : 0;
}

int64_t
applyCreatePassiveOffer(Application& app, SecretKey const& source,
                        Asset const& selling, Asset const& buying,
                        Price const& price, int64_t amount, SequenceNumber seq,
                        ManageOfferEffect expectedEffect)
{
    auto getIdPool = [&]() {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        return ltx.loadHeader().current().idPool;
    };
    auto lastGeneratedID = getIdPool();
    auto expectedOfferID = lastGeneratedID + 1;

    auto op = createPassiveOffer(selling, buying, price, amount);
    auto tx = transactionFromOperations(app, source, seq, {op});

    try
    {
        applyTx(tx, app);
    }
    catch (...)
    {
        REQUIRE(getIdPool() == lastGeneratedID);
        throw;
    }

    auto& results = tx->getResult().result.results();

    REQUIRE(results.size() == 1);

    auto& createPassiveSellOfferResult =
        results[0].tr().manageSellOfferResult();

    if (createPassiveSellOfferResult.code() == MANAGE_SELL_OFFER_SUCCESS)
    {
        auto& offerResult = createPassiveSellOfferResult.success().offer;

        switch (offerResult.effect())
        {
        case MANAGE_OFFER_CREATED:
        case MANAGE_OFFER_UPDATED:
        {
            LedgerTxn ltx(app.getLedgerTxnRoot());
            auto offer =
                stellar::loadOffer(ltx, source.getPublicKey(), expectedOfferID);
            REQUIRE(offer);
            auto& offerEntry = offer.current().data.offer();
            REQUIRE(offerEntry == offerResult.offer());
            REQUIRE(offerEntry.price == price);
            REQUIRE(offerEntry.selling == selling);
            REQUIRE(offerEntry.buying == buying);
            REQUIRE((offerEntry.flags & PASSIVE_FLAG) != 0);
        }
        break;
        case MANAGE_OFFER_DELETED:
        {
            LedgerTxn ltx(app.getLedgerTxnRoot());
            REQUIRE(!stellar::loadOffer(ltx, source.getPublicKey(),
                                        expectedOfferID));
        }
        break;
        default:
            abort();
        }
    }

    auto& success = createPassiveSellOfferResult.success().offer;

    REQUIRE(success.effect() == expectedEffect);

    return success.effect() == MANAGE_OFFER_CREATED ? success.offer().offerID
                                                    : 0;
}

SetOptionsArguments
operator|(SetOptionsArguments const& x, SetOptionsArguments const& y)
{
    auto result = SetOptionsArguments{};
    result.masterWeight = y.masterWeight ? y.masterWeight : x.masterWeight;
    result.lowThreshold = y.lowThreshold ? y.lowThreshold : x.lowThreshold;
    result.medThreshold = y.medThreshold ? y.medThreshold : x.medThreshold;
    result.highThreshold = y.highThreshold ? y.highThreshold : x.highThreshold;
    result.signer = y.signer ? y.signer : x.signer;
    result.setFlags = y.setFlags ? y.setFlags : x.setFlags;
    result.clearFlags = y.clearFlags ? y.clearFlags : x.clearFlags;
    result.inflationDest = y.inflationDest ? y.inflationDest : x.inflationDest;
    result.homeDomain = y.homeDomain ? y.homeDomain : x.homeDomain;
    return result;
}

Operation
setOptions(SetOptionsArguments const& arguments)
{
    Operation op;
    op.body.type(SET_OPTIONS);

    SetOptionsOp& setOp = op.body.setOptionsOp();

    if (arguments.inflationDest)
    {
        setOp.inflationDest.activate() = *arguments.inflationDest;
    }

    if (arguments.setFlags)
    {
        setOp.setFlags.activate() = *arguments.setFlags;
    }

    if (arguments.clearFlags)
    {
        setOp.clearFlags.activate() = *arguments.clearFlags;
    }

    if (arguments.masterWeight)
    {
        setOp.masterWeight.activate() = *arguments.masterWeight;
    }
    if (arguments.lowThreshold)
    {
        setOp.lowThreshold.activate() = *arguments.lowThreshold;
    }
    if (arguments.medThreshold)
    {
        setOp.medThreshold.activate() = *arguments.medThreshold;
    }
    if (arguments.highThreshold)
    {
        setOp.highThreshold.activate() = *arguments.highThreshold;
    }

    if (arguments.signer)
    {
        setOp.signer.activate() = *arguments.signer;
    }

    if (arguments.homeDomain)
    {
        setOp.homeDomain.activate() = *arguments.homeDomain;
    }

    return op;
}

SetOptionsArguments
setMasterWeight(int master)
{
    SetOptionsArguments result;
    result.masterWeight = std::make_optional<int>(master);
    return result;
}

SetOptionsArguments
setLowThreshold(int low)
{
    SetOptionsArguments result;
    result.lowThreshold = std::make_optional<int>(low);
    return result;
}

SetOptionsArguments
setMedThreshold(int med)
{
    SetOptionsArguments result;
    result.medThreshold = std::make_optional<int>(med);
    return result;
}

SetOptionsArguments
setHighThreshold(int high)
{
    SetOptionsArguments result;
    result.highThreshold = std::make_optional<int>(high);
    return result;
}

SetOptionsArguments
setSigner(Signer signer)
{
    SetOptionsArguments result;
    result.signer = std::make_optional<Signer>(signer);
    return result;
}

SetOptionsArguments
setFlags(uint32_t setFlags)
{
    SetOptionsArguments result;
    result.setFlags = std::make_optional<uint32_t>(setFlags);
    return result;
}

SetOptionsArguments
clearFlags(uint32_t clearFlags)
{
    SetOptionsArguments result;
    result.clearFlags = std::make_optional<uint32_t>(clearFlags);
    return result;
}

SetOptionsArguments
setInflationDestination(AccountID inflationDest)
{
    SetOptionsArguments result;
    result.inflationDest = std::make_optional<AccountID>(inflationDest);
    return result;
}

SetOptionsArguments
setHomeDomain(std::string const& homeDomain)
{
    SetOptionsArguments result;
    result.homeDomain = std::make_optional<std::string>(homeDomain);
    return result;
}

SetTrustLineFlagsArguments
operator|(SetTrustLineFlagsArguments const& x,
          SetTrustLineFlagsArguments const& y)
{
    auto result = SetTrustLineFlagsArguments{};
    result.setFlags = y.setFlags | x.setFlags;
    result.clearFlags = y.clearFlags | x.clearFlags;
    return result;
}

Operation
setTrustLineFlags(PublicKey const& trustor, Asset const& asset,
                  SetTrustLineFlagsArguments const& arguments)
{
    Operation op;
    op.body.type(SET_TRUST_LINE_FLAGS);

    SetTrustLineFlagsOp& setOp = op.body.setTrustLineFlagsOp();
    setOp.trustor = trustor;
    setOp.asset = asset;
    setOp.setFlags = arguments.setFlags;
    setOp.clearFlags = arguments.clearFlags;

    return op;
}

SetTrustLineFlagsArguments
setTrustLineFlags(uint32_t setFlags)
{
    SetTrustLineFlagsArguments result;
    result.setFlags = setFlags;
    return result;
}

SetTrustLineFlagsArguments
clearTrustLineFlags(uint32_t clearFlags)
{
    SetTrustLineFlagsArguments result;
    result.clearFlags = clearFlags;
    return result;
}

Operation
inflation()
{
    Operation op;
    op.body.type(INFLATION);

    return op;
}

Operation
accountMerge(PublicKey const& dest)
{
    Operation op;
    op.body.type(ACCOUNT_MERGE);
    op.body.destination() = toMuxedAccount(dest);
    return op;
}

Operation
manageData(std::string const& name, DataValue* value)
{
    Operation op;
    op.body.type(MANAGE_DATA);
    op.body.manageDataOp().dataName = name;
    if (value)
        op.body.manageDataOp().dataValue.activate() = *value;

    return op;
}

Operation
bumpSequence(SequenceNumber to)
{
    Operation op;
    op.body.type(BUMP_SEQUENCE);
    op.body.bumpSequenceOp().bumpTo = to;
    return op;
}

Operation
createClaimableBalance(Asset const& asset, int64_t amount,
                       xdr::xvector<Claimant, 10> const& claimants)
{
    Operation op;
    op.body.type(CREATE_CLAIMABLE_BALANCE);
    op.body.createClaimableBalanceOp().asset = asset;
    op.body.createClaimableBalanceOp().amount = amount;
    op.body.createClaimableBalanceOp().claimants = claimants;
    return op;
}

Operation
claimClaimableBalance(ClaimableBalanceID const& balanceID)
{
    Operation op;
    op.body.type(CLAIM_CLAIMABLE_BALANCE);
    op.body.claimClaimableBalanceOp().balanceID = balanceID;
    return op;
}

Operation
beginSponsoringFutureReserves(PublicKey const& sponsoredID)
{
    Operation op;
    op.body.type(BEGIN_SPONSORING_FUTURE_RESERVES);
    op.body.beginSponsoringFutureReservesOp().sponsoredID = sponsoredID;
    return op;
}

Operation
endSponsoringFutureReserves()
{
    Operation op;
    op.body.type(END_SPONSORING_FUTURE_RESERVES);
    return op;
}

Operation
revokeSponsorship(LedgerKey const& key)
{
    Operation op;
    op.body.type(REVOKE_SPONSORSHIP);
    op.body.revokeSponsorshipOp().type(REVOKE_SPONSORSHIP_LEDGER_ENTRY);
    op.body.revokeSponsorshipOp().ledgerKey() = key;
    return op;
}

Operation
revokeSponsorship(AccountID const& accID, SignerKey const& key)
{
    Operation op;
    op.body.type(REVOKE_SPONSORSHIP);
    op.body.revokeSponsorshipOp().type(REVOKE_SPONSORSHIP_SIGNER);
    op.body.revokeSponsorshipOp().signer().accountID = accID;
    op.body.revokeSponsorshipOp().signer().signerKey = key;
    return op;
}

Operation
clawback(AccountID const& from, Asset const& asset, int64_t amount)
{
    Operation op;
    op.body.type(CLAWBACK);
    op.body.clawbackOp().from = toMuxedAccount(from);
    op.body.clawbackOp().amount = amount;
    op.body.clawbackOp().asset = asset;

    return op;
}

Operation
clawbackClaimableBalance(ClaimableBalanceID const& balanceID)
{
    Operation op;
    op.body.type(CLAWBACK_CLAIMABLE_BALANCE);
    op.body.clawbackClaimableBalanceOp().balanceID = balanceID;
    return op;
}

Operation
liquidityPoolDeposit(PoolID const& poolID, int64_t maxAmountA,
                     int64_t maxAmountB, Price const& minPrice,
                     Price const& maxPrice)
{
    Operation op;
    op.body.type(LIQUIDITY_POOL_DEPOSIT);
    op.body.liquidityPoolDepositOp().liquidityPoolID = poolID;
    op.body.liquidityPoolDepositOp().maxAmountA = maxAmountA;
    op.body.liquidityPoolDepositOp().maxAmountB = maxAmountB;
    op.body.liquidityPoolDepositOp().minPrice = minPrice;
    op.body.liquidityPoolDepositOp().maxPrice = maxPrice;
    return op;
}

Operation
liquidityPoolWithdraw(PoolID const& poolID, int64_t amount, int64_t minAmountA,
                      int64_t minAmountB)
{
    Operation op;
    op.body.type(LIQUIDITY_POOL_WITHDRAW);
    op.body.liquidityPoolWithdrawOp().liquidityPoolID = poolID;
    op.body.liquidityPoolWithdrawOp().amount = amount;
    op.body.liquidityPoolWithdrawOp().minAmountA = minAmountA;
    op.body.liquidityPoolWithdrawOp().minAmountB = minAmountB;
    return op;
}

OperationFrame const&
getFirstOperationFrame(TransactionFrame const& tx)
{
    return *(tx.getOperations()[0]);
}

OperationResult const&
getFirstResult(TransactionFrame const& tx)
{
    return getFirstOperationFrame(tx).getResult();
}

OperationResultCode
getFirstResultCode(TransactionFrame const& tx)
{
    return getFirstOperationFrame(tx).getResultCode();
}

void
checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected)
{
    REQUIRE(r[index].first.result.result.code() == expected);
};

void
checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected,
        OperationResultCode code)
{
    checkTx(index, r, expected);
    REQUIRE(r[index].first.result.result.results()[0].code() == code);
};

static void
sign(Hash const& networkID, SecretKey key, TransactionV1Envelope& env)
{
    env.signatures.emplace_back(SignatureUtils::sign(
        key, sha256(xdr::xdr_to_opaque(networkID, ENVELOPE_TYPE_TX, env.tx))));
}

static TransactionEnvelope
envelopeFromOps(Hash const& networkID, TestAccount& source,
                std::vector<Operation> const& ops,
                std::vector<SecretKey> const& opKeys,
                std::optional<PreconditionsV2> cond = std::nullopt)
{
    TransactionEnvelope tx(ENVELOPE_TYPE_TX);
    tx.v1().tx.sourceAccount = toMuxedAccount(source);
    tx.v1().tx.fee = uint32_t(100) * uint32_t(ops.size());
    tx.v1().tx.seqNum = source.nextSequenceNumber();
    std::copy(ops.begin(), ops.end(),
              std::back_inserter(tx.v1().tx.operations));

    if (cond)
    {
        tx.v1().tx.cond.type(PRECOND_V2);
        tx.v1().tx.cond.v2() = *cond;
    }
    sign(networkID, source, tx.v1());
    for (auto const& opKey : opKeys)
    {
        sign(networkID, opKey, tx.v1());
    }
    return tx;
}

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
static TransactionEnvelope
sorobanEnvelopeFromOps(Hash const& networkID, TestAccount& source,
                       std::vector<Operation> const& ops,
                       std::vector<SecretKey> const& opKeys,
                       SorobanResources const& resources, uint32_t fee,
                       uint32_t refundableFee, std::optional<std::string> memo)
{
    TransactionEnvelope tx(ENVELOPE_TYPE_TX);
    tx.v1().tx.sourceAccount = toMuxedAccount(source);
    tx.v1().tx.fee = fee;
    tx.v1().tx.seqNum = source.nextSequenceNumber();
    tx.v1().tx.ext.v(1);
    tx.v1().tx.ext.sorobanData().resources = resources;
    tx.v1().tx.ext.sorobanData().refundableFee = refundableFee;
    if (memo)
    {
        Memo textMemo(MEMO_TEXT);
        textMemo.text() = *memo;
        tx.v1().tx.memo = textMemo;
    }
    std::copy(ops.begin(), ops.end(),
              std::back_inserter(tx.v1().tx.operations));

    sign(networkID, source, tx.v1());
    for (auto const& opKey : opKeys)
    {
        sign(networkID, opKey, tx.v1());
    }
    return tx;
}
#endif

TransactionFrameBasePtr
transactionFrameFromOps(Hash const& networkID, TestAccount& source,
                        std::vector<Operation> const& ops,
                        std::vector<SecretKey> const& opKeys,
                        std::optional<PreconditionsV2> cond)
{
    return TransactionFrameBase::makeTransactionFromWire(
        networkID, envelopeFromOps(networkID, source, ops, opKeys, cond));
}

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
TransactionFrameBasePtr
sorobanTransactionFrameFromOps(Hash const& networkID, TestAccount& source,
                               std::vector<Operation> const& ops,
                               std::vector<SecretKey> const& opKeys,
                               SorobanResources const& resources, uint32_t fee,
                               uint32_t refundableFee,
                               std::optional<std::string> memo)
{
    return TransactionFrameBase::makeTransactionFromWire(
        networkID, sorobanEnvelopeFromOps(networkID, source, ops, opKeys,
                                          resources, fee, refundableFee, memo));
}
#endif

LedgerUpgrade
makeBaseReserveUpgrade(int baseReserve)
{
    auto result = LedgerUpgrade{LEDGER_UPGRADE_BASE_RESERVE};
    result.newBaseReserve() = baseReserve;
    return result;
}

LedgerHeader
executeUpgrades(Application& app, xdr::xvector<UpgradeType, 6> const& upgrades,
                bool upgradesIgnored)
{
    auto& lm = app.getLedgerManager();
    auto currLh = app.getLedgerManager().getLastClosedLedgerHeader().header;

    auto const& lcl = lm.getLastClosedLedgerHeader();
    auto txSet = TxSetFrame::makeEmpty(lcl);
    auto lastCloseTime = lcl.header.scpValue.closeTime;
    app.getHerder().externalizeValue(txSet, lcl.header.ledgerSeq + 1,
                                     lastCloseTime, upgrades);
    if (upgradesIgnored)
    {
        auto const& newHeader = lm.getLastClosedLedgerHeader().header;
        REQUIRE(currLh.baseFee == newHeader.baseFee);
        REQUIRE(currLh.baseReserve == newHeader.baseReserve);
        REQUIRE(currLh.ledgerVersion == newHeader.ledgerVersion);
        REQUIRE(currLh.maxTxSetSize == newHeader.maxTxSetSize);
        REQUIRE(currLh.ext.v() == newHeader.ext.v());
        if (currLh.ext.v() == 1)
        {
            REQUIRE(currLh.ext.v1().flags == newHeader.ext.v1().flags);
        }
    }
    return lm.getLastClosedLedgerHeader().header;
};

LedgerHeader
executeUpgrade(Application& app, LedgerUpgrade const& lupgrade,
               bool upgradeIgnored)
{
    return executeUpgrades(app, {LedgerTestUtils::toUpgradeType(lupgrade)},
                           upgradeIgnored);
};

// trades is a vector of pairs, where the bool indicates if assetA or assetB is
// sent in the payment, and the int64_t is the amount
void
depositTradeWithdrawTest(Application& app, TestAccount& root, int depositSize,
                         std::vector<std::pair<bool, int64_t>> const& trades)
{
    struct Deposit
    {
        TestAccount acc;
        int64_t numPoolShares;
    };
    std::vector<Deposit> deposits;

    int64_t total = 0;

    auto cur1 = makeAsset(root, "CUR1");
    auto cur2 = makeAsset(root, "CUR2");
    auto share12 =
        makeChangeTrustAssetPoolShare(cur1, cur2, LIQUIDITY_POOL_FEE_V18);
    auto pool12 = xdrSha256(share12.liquidityPool());

    auto deposit = [&](int64_t accNum, int64_t size) {
        auto acc = root.create(fmt::format("account{}", accNum),
                               app.getLedgerManager().getLastMinBalance(10));
        acc.changeTrust(cur1, INT64_MAX);
        acc.changeTrust(cur2, INT64_MAX);
        acc.changeTrust(share12, INT64_MAX);

        root.pay(acc, cur1, size);
        root.pay(acc, cur2, size);

        acc.liquidityPoolDeposit(pool12, size, size, Price{1, INT32_MAX},
                                 Price{INT32_MAX, 1});

        total += size;

        checkLiquidityPool(app, pool12, total, total, total, accNum + 1);

        deposits.emplace_back(Deposit{acc, size});
    };

    // deposit
    deposit(0, depositSize);
    deposit(1, depositSize);

    for (auto const& trade : trades)
    {
        auto const& sendAsset = trade.first ? cur1 : cur2;
        auto const& recvAsset = trade.first ? cur2 : cur1;
        root.pay(root, sendAsset, INT64_MAX, recvAsset, trade.second, {});
    }

    // withdraw in reverse order
    for (auto rit = deposits.rbegin(); rit != deposits.rend(); ++rit)
    {
        auto& d = *rit;
        d.acc.liquidityPoolWithdraw(pool12, d.numPoolShares, 0, 0);

        total -= d.numPoolShares;
    }

    checkLiquidityPool(app, pool12, 0, 0, 0, 2);

    // delete trustlines
    int i = 2;
    for (auto& dep : deposits)
    {
        dep.acc.changeTrust(share12, 0);

        if (--i > 0)
        {
            checkLiquidityPool(app, pool12, 0, 0, 0, i);
        }
    }

    LedgerTxn ltx(app.getLedgerTxnRoot());
    REQUIRE(!loadLiquidityPool(ltx, pool12));
}

}
}
