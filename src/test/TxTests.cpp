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
#include "transactions/AllowTrustOpFrame.h"
#include "transactions/BumpSequenceOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/InflationOpFrame.h"
#include "transactions/ManageDataOpFrame.h"
#include "transactions/ManageOfferOpFrame.h"
#include "transactions/MergeOpFrame.h"
#include "transactions/PathPaymentOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/types.h"

#include <lib/catch.hpp>

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;
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
        check = tx->checkValid(app, ltxFeeProc, 0);
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
            tx->processFeeSeqNum(ltxFeeProc);
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
        try
        {
            res = tx->apply(app, ltxTx);
        }
        catch (...)
        {
            tx->getResult().result.code(txINTERNAL_ERROR);
        }
        REQUIRE((!res || tx->getResultCode() == txSUCCESS));

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
                if (checkSeqNum && header.current().ledgerVersion >= 10 &&
                    !earlyFailure)
                {
                    REQUIRE(srcAccountAfter.current().data.account().seqNum ==
                            (srcAccountBefore.seqNum + 1));
                }
                // on failure, no other changes should have been made
                if (!res)
                {
                    if (earlyFailure || header.current().ledgerVersion <= 9)
                    {
                        // no changes during an early failure
                        REQUIRE(ltxTx.getDelta().entry.empty());
                    }
                    else
                    {
                        auto ltxDelta = ltxTx.getDelta();
                        REQUIRE(ltxDelta.entry.size() == 1);
                        auto current = ltxDelta.entry.begin()->second.current;
                        REQUIRE(current);
                        auto previous = ltxDelta.entry.begin()->second.previous;
                        REQUIRE(previous);
                        auto currAcc = current->data.account();
                        REQUIRE(currAcc.accountID ==
                                srcAccountBefore.accountID);
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
        REQUIRE(tx->checkValid(app, ltx, 0) == shouldValidateOk);
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
              std::vector<TransactionFramePtr> const& txs)
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
                    emptyUpgradeSteps, 0);
    LedgerCloseData ledgerData(ledgerSeq, txSet, sv);
    app.getLedgerManager().closeLedger(ledgerData);

    auto z1 = TransactionFrame::getTransactionHistoryResults(app.getDatabase(),
                                                             ledgerSeq);
    auto z2 =
        TransactionFrame::getTransactionFeeMeta(app.getDatabase(), ledgerSeq);

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
getAccount(const char* n)
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
transactionFromOperations(Application& app, SecretKey const& from,
                          SequenceNumber seq, const std::vector<Operation>& ops)
{
    auto e = TransactionEnvelope{};
    e.tx.sourceAccount = from.getPublicKey();
    e.tx.fee = static_cast<uint32_t>(
        (ops.size() * app.getLedgerManager().getLastTxFee()) & UINT32_MAX);
    e.tx.seqNum = seq;
    std::copy(std::begin(ops), std::end(ops),
              std::back_inserter(e.tx.operations));

    auto res = TransactionFrame::makeTransactionFromWire(app.getNetworkID(), e);
    res->addSignature(from);
    return res;
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
allowTrust(PublicKey const& trustor, Asset const& asset, bool authorize)
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
    op.body.paymentOp().destination = to;
    op.body.paymentOp().asset.type(ASSET_TYPE_NATIVE);
    return op;
}

Operation
payment(PublicKey const& to, Asset const& asset, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().destination = to;
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
    op.body.type(PATH_PAYMENT);
    PathPaymentOp& ppop = op.body.pathPaymentOp();
    ppop.sendAsset = sendCur;
    ppop.sendMax = sendMax;
    ppop.destAsset = destCur;
    ppop.destAmount = destAmount;
    ppop.destination = to;
    std::copy(std::begin(path), std::end(path), std::back_inserter(ppop.path));

    return op;
}

Operation
createPassiveOffer(Asset const& selling, Asset const& buying,
                   Price const& price, int64_t amount)
{
    Operation op;
    op.body.type(CREATE_PASSIVE_OFFER);
    op.body.createPassiveOfferOp().amount = amount;
    op.body.createPassiveOfferOp().selling = selling;
    op.body.createPassiveOfferOp().buying = buying;
    op.body.createPassiveOfferOp().price = price;

    return op;
}

Operation
manageOffer(uint64 offerId, Asset const& selling, Asset const& buying,
            Price const& price, int64_t amount)
{
    Operation op;
    op.body.type(MANAGE_OFFER);
    op.body.manageOfferOp().amount = amount;
    op.body.manageOfferOp().selling = selling;
    op.body.manageOfferOp().buying = buying;
    op.body.manageOfferOp().offerID = offerId;
    op.body.manageOfferOp().price = price;

    return op;
}

static ManageOfferResult
applyCreateOfferHelper(Application& app, uint64 offerId,
                       SecretKey const& source, Asset const& selling,
                       Asset const& buying, Price const& price, int64_t amount,
                       SequenceNumber seq)
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

    auto& manageOfferResult = results[0].tr().manageOfferResult();

    auto& offerResult = manageOfferResult.success().offer;

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

    return manageOfferResult;
}

uint64_t
applyManageOffer(Application& app, uint64 offerId, SecretKey const& source,
                 Asset const& selling, Asset const& buying, Price const& price,
                 int64_t amount, SequenceNumber seq,
                 ManageOfferEffect expectedEffect)
{
    ManageOfferResult const& createOfferRes = applyCreateOfferHelper(
        app, offerId, source, selling, buying, price, amount, seq);

    auto& success = createOfferRes.success().offer;
    REQUIRE(success.effect() == expectedEffect);
    return success.effect() != MANAGE_OFFER_DELETED ? success.offer().offerID
                                                    : 0;
}

uint64_t
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

    auto& createPassiveOfferResult = results[0].tr().manageOfferResult();

    if (createPassiveOfferResult.code() == MANAGE_OFFER_SUCCESS)
    {
        auto& offerResult = createPassiveOfferResult.success().offer;

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

    auto& success = createPassiveOfferResult.success().offer;

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
    op.body.destination() = dest;
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
