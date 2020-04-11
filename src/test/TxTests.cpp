// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/TxTests.h"
#include "crypto/ByteSlice.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/OperationFrame.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionSQL.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/types.h"

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
    LedgerTxn ltx(app.getLedgerTxnRoot());
    // Increment ledgerSeq to simulate the behavior of closeLedger, which begins
    // by advancing the ledgerSeq.
    ++ltx.loadHeader().current().ledgerSeq;

    bool check = false;
    TransactionResult checkResult;
    TransactionResultCode code;
    AccountEntry srcAccountBefore;
    {
        LedgerTxn ltxFeeProc(ltx);
        check = tx->checkValid(ltxFeeProc, 0);
        checkResult = tx->getResult();
        REQUIRE((!check || checkResult.result.code() == txSUCCESS));

        // now, check what happens when simulating what happens during a ledger
        // close and reconcile it with the return value of "apply" with the one
        // from checkValid:
        // * an invalid (as per isValid) tx is still invalid during apply (and
        // the same way)
        // * a valid tx can fail later
        code = checkResult.result.code();
        if (code != txNO_ACCOUNT)
        {
            srcAccountBefore = loadAccount(ltxFeeProc, tx->getSourceID(), true)
                                   .current()
                                   .data.account();

            // no account -> can't process the fee
            auto baseFee = ltxFeeProc.loadHeader().current().baseFee;
            tx->processFeeSeqNum(ltxFeeProc, baseFee);
            uint32_t ledgerVersion =
                ltxFeeProc.loadHeader().current().ledgerVersion;

            // verify that the fee got processed
            auto ltxDelta = ltxFeeProc.getDelta();
            REQUIRE(ltxDelta.entry.size() == 1);
            auto current = ltxDelta.entry.begin()->second.current;
            REQUIRE(current);
            auto previous = ltxDelta.entry.begin()->second.previous;
            REQUIRE(previous);
            auto currAcc = current->data.account();
            auto prevAcc = previous->data.account();
            REQUIRE(prevAcc == srcAccountBefore);
            REQUIRE(currAcc.accountID == tx->getSourceID());
            REQUIRE(currAcc.balance < prevAcc.balance);
            currAcc.balance = prevAcc.balance;
            if (ledgerVersion <= 9)
            {
                // v9 and below, we also need to verify that the sequence number
                // also got processed at this time
                REQUIRE(currAcc.seqNum == prevAcc.seqNum + 1);
                currAcc.seqNum = prevAcc.seqNum;
            }
            REQUIRE(currAcc == prevAcc);
        }
        ltxFeeProc.commit();
    }

    bool res = false;
    {
        LedgerTxn ltxTx(ltx);
        TransactionMeta tm(2);
        try
        {
            res = tx->apply(app, ltxTx, tm);
        }
        catch (...)
        {
            tx->getResult().result.code(txINTERNAL_ERROR);
        }
        REQUIRE((!res || tx->getResultCode() == txSUCCESS));

        if (!res || tx->getResultCode() != txSUCCESS)
        {
            REQUIRE(tm.v2().operations.size() == 0);
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
                     code == txBAD_SEQ);
                // verify that the sequence number changed (v10+)
                // do not perform the check if there was a failure before
                // or during the sequence number processing
                auto header = ltxTx.loadHeader();
                auto ledgerVersion = header.current().ledgerVersion;
                if (checkSeqNum && ledgerVersion >= 10 && !earlyFailure)
                {
                    REQUIRE(srcAccountAfter.current().data.account().seqNum ==
                            (srcAccountBefore.seqNum + 1));
                }
                // on failure, no other changes should have been made
                if (!res)
                {
                    bool noChangeOnEarlyFailure =
                        earlyFailure && ledgerVersion < 13;
                    if (noChangeOnEarlyFailure || ledgerVersion <= 9)
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
                            auto previous = kvp.second.previous;
                            REQUIRE(previous);

                            // From V13, it's possible to remove one-time
                            // signers on early failures
                            if (ledgerVersion >= 13 && earlyFailure)
                            {
                                auto currAcc = current->data.account();
                                auto prevAcc = previous->data.account();
                                REQUIRE(currAcc.signers.size() + 1 ==
                                        prevAcc.signers.size());
                                // signers should be the only change so this
                                // should make the accounts equivalent
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
    }

    // Undo the increment from the beginning of this function. Note that if this
    // function exits without reaching this point, then ltx will not be
    // committed and the increment will be rolled back anyway.
    --ltx.loadHeader().current().ledgerSeq;
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
}

void
validateTxResults(TransactionFramePtr const& tx, Application& app,
                  ValidationResult validationResult,
                  TransactionResult const& applyResult)
{
    auto shouldValidateOk = validationResult.code == txSUCCESS;
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        REQUIRE(tx->checkValid(ltx, 0) == shouldValidateOk);
    }
    REQUIRE(tx->getResult().result.code() == validationResult.code);
    REQUIRE(tx->getResult().feeCharged == validationResult.fee);

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

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, int day, int month, int year,
              std::vector<TransactionFrameBasePtr> const& txs)
{
    auto txSet = std::make_shared<TxSetFrame>(
        app.getLedgerManager().getLastClosedLedgerHeader().hash);

    for (auto const& tx : txs)
    {
        txSet->add(tx);
    }

    txSet->sortForHash();
    REQUIRE(txSet->checkValid(app));

    StellarValue sv(txSet->getContentsHash(), getTestDate(day, month, year),
                    emptyUpgradeSteps, STELLAR_VALUE_BASIC);
    LedgerCloseData ledgerData(ledgerSeq, txSet, sv);
    app.getLedgerManager().closeLedger(ledgerData);

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
                            const std::vector<Operation>& ops, int fee)
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
                            const std::vector<Operation>& ops, int fee)
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

    auto res = std::static_pointer_cast<TransactionFrame>(
        TransactionFrameBase::makeTransactionFromWire(app.getNetworkID(), e));
    res->addSignature(from);
    return res;
}

TransactionFramePtr
transactionFromOperations(Application& app, SecretKey const& from,
                          SequenceNumber seq, const std::vector<Operation>& ops,
                          int fee)
{
    uint32_t ledgerVersion;
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    }
    if (ledgerVersion < 13)
    {
        return transactionFromOperationsV0(app, from, seq, ops, fee);
    }
    return transactionFromOperationsV1(app, from, seq, ops, fee);
}

Operation
changeTrust(Asset const& asset, int64_t limit)
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
    op.body.allowTrustOp().asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    op.body.allowTrustOp().asset.assetCode4() = asset.alphaNum4().assetCode;
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
    result.masterWeight = make_optional<int>(master);
    return result;
}

SetOptionsArguments
setLowThreshold(int low)
{
    SetOptionsArguments result;
    result.lowThreshold = make_optional<int>(low);
    return result;
}

SetOptionsArguments
setMedThreshold(int med)
{
    SetOptionsArguments result;
    result.medThreshold = make_optional<int>(med);
    return result;
}

SetOptionsArguments
setHighThreshold(int high)
{
    SetOptionsArguments result;
    result.highThreshold = make_optional<int>(high);
    return result;
}

SetOptionsArguments
setSigner(Signer signer)
{
    SetOptionsArguments result;
    result.signer = make_optional<Signer>(signer);
    return result;
}

SetOptionsArguments
setFlags(uint32_t setFlags)
{
    SetOptionsArguments result;
    result.setFlags = make_optional<uint32_t>(setFlags);
    return result;
}

SetOptionsArguments
clearFlags(uint32_t clearFlags)
{
    SetOptionsArguments result;
    result.clearFlags = make_optional<uint32_t>(clearFlags);
    return result;
}

SetOptionsArguments
setInflationDestination(AccountID inflationDest)
{
    SetOptionsArguments result;
    result.inflationDest = make_optional<AccountID>(inflationDest);
    return result;
}

SetOptionsArguments
setHomeDomain(std::string const& homeDomain)
{
    SetOptionsArguments result;
    result.homeDomain = make_optional<std::string>(homeDomain);
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
}
}
