// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TxTests.h"

#include "crypto/ByteSlice.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "lib/catch.hpp"
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
#include "util/make_unique.h"
#include "util/types.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;
namespace stellar
{
using xdr::operator==;

namespace txtest
{

bool
applyCheck(TransactionFramePtr tx, Application& app, bool checkSeqNum)
{
    app.getDatabase().clearPreparedStatementCache();

    LedgerState lsCheck(app.getLedgerStateRoot());
    bool check = tx->checkValid(app, lsCheck, 0);
    TransactionResult checkResult = tx->getResult();
    REQUIRE((!check || checkResult.result.code() == txSUCCESS));
    lsCheck.rollback();

    LedgerState ls(app.getLedgerStateRoot());

    AccountEntry srcAccountBefore;

    // now, check what happens when simulating what happens during a ledger
    // close and reconcile it with the return value of "apply" with the one from
    // checkValid:
    // * an invalid (as per isValid) tx is still invalid during apply (and the
    // same way)
    // * a valid tx can fail later
    auto code = checkResult.result.code();
    if (code != txNO_ACCOUNT)
    {
        auto acnt = stellar::loadAccount(ls, tx->getSourceID());
        REQUIRE(acnt);
        srcAccountBefore = acnt.account();
        acnt.invalidate();

        // no account -> can't process the fee
        tx->processFeeSeqNum(ls);

        // verify that the fee got processed
        REQUIRE(++ls.begin() == ls.end());
        {
            auto const& val = *ls.begin();

            REQUIRE(val.previousEntry());
            auto const& prevAccount = val.previousEntry()->data.account();
            REQUIRE(prevAccount == srcAccountBefore);

            REQUIRE(val.entry());
            auto curAccount = val.entry()->data.account();

            // the balance should have changed
            REQUIRE(curAccount.balance < prevAccount.balance);
            curAccount.balance = prevAccount.balance;
            
            auto header = ls.loadHeader();
            if (getCurrentLedgerVersion(header) <= 9)
            {
                // v9 and below, we also need to verify that the sequence number
                // also got processed at this time
                REQUIRE(curAccount.seqNum == (prevAccount.seqNum + 1));
                curAccount.seqNum = prevAccount.seqNum;
            }
            header->invalidate();
            REQUIRE(curAccount == prevAccount);
        }
    }

    bool res = false;
    {
        LedgerState lsApply(ls);
        try
        {
            res = tx->apply(lsApply, app);
        }
        catch (...)
        {
            tx->getResult().result.code(txINTERNAL_ERROR);
        }

        REQUIRE((!res || tx->getResultCode() == txSUCCESS));

        // checks that the failure is the same if pre checks failed
        if (!check)
        {
            REQUIRE(checkResult == tx->getResult());
        }

        if (code != txNO_ACCOUNT)
        {
            auto srcAccountAfter =
                stellar::loadAccount(lsApply, srcAccountBefore.accountID);
            if (srcAccountAfter)
            {
                bool earlyFailure =
                    (code == txMISSING_OPERATION || code == txTOO_EARLY ||
                     code == txTOO_LATE || code == txINSUFFICIENT_FEE ||
                     code == txBAD_SEQ);
                // verify that the sequence number changed (v10+)
                // do not perform the check if there was a failure before
                // or during the sequence number processing
                auto header = lsApply.loadHeader();
                if (checkSeqNum &&
                    getCurrentLedgerVersion(header) >= 10 &&
                    !earlyFailure)
                {
                    REQUIRE(srcAccountAfter.getSeqNum() ==
                            (srcAccountBefore.seqNum + 1));
                }
                // on failure, no other changes should have been made
                if (!res)
                {
                    if (earlyFailure || getCurrentLedgerVersion(header) <= 9)
                    {
                        // no changes during an early failure
                        REQUIRE(lsApply.begin() == lsApply.end());
                    }
                    else
                    {
                        REQUIRE(++lsApply.begin() == lsApply.end());
                        {
                            auto const& val = *lsApply.begin();
                            REQUIRE(val.previousEntry());
                            auto const& prevAccount = val.previousEntry()->data.account();
                            REQUIRE(prevAccount == srcAccountBefore);
                            // could check more here if needed
                        }
                    }
                }
            }
        }
        lsApply.commit();
    }

    // validates db state
    ls.commit();
    return res;
}

void
checkTransaction(TransactionFrame& txFrame, Application& app)
{
    REQUIRE(txFrame.getResult().feeCharged ==
            getCurrentTxFee(app.getLedgerStateRoot()));
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

    REQUIRE(getCurrentLedgerNum(app.getLedgerStateRoot()) == (ledgerSeq + 1));

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

bool
hasAccount(Application& app, PublicKey const& k)
{
    LedgerState ls(app.getLedgerStateRoot());
    return !!stellar::loadAccount(ls, k);
}

xdr::xvector<Signer, 20>
getAccountSigners(PublicKey const& k, Application& app)
{
    LedgerState ls(app.getLedgerStateRoot());
    auto account = stellar::loadAccount(ls, k);
    REQUIRE(account);
    return account.account().signers;
}

TransactionFramePtr
transactionFromOperations(Application& app, SecretKey const& from,
                          SequenceNumber seq, const std::vector<Operation>& ops)
{
    auto e = TransactionEnvelope{};
    e.tx.sourceAccount = from.getPublicKey();
    e.tx.fee = static_cast<uint32_t>(
        (ops.size() * getCurrentTxFee(app.getLedgerStateRoot())) & UINT32_MAX);
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
    LedgerState ls(app.getLedgerStateRoot());
    auto lastGeneratedID = ls.loadHeader()->header().idPool;
    ls.rollback();

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
        LedgerState ls(app.getLedgerStateRoot());
        REQUIRE(ls.loadHeader()->header().idPool == lastGeneratedID);
        ls.rollback();
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
        LedgerState ls(app.getLedgerStateRoot());
        auto offer = loadOffer(ls, source.getPublicKey(), expectedOfferID);
        REQUIRE(offer);
        auto const& offerEntry = offer->entry()->data.offer();
        REQUIRE(offerEntry == offerResult.offer());
        REQUIRE(offerEntry.price == price);
        REQUIRE(offerEntry.selling == selling);
        REQUIRE(offerEntry.buying == buying);
    }
    break;
    case MANAGE_OFFER_DELETED:
    {
        LedgerState ls(app.getLedgerStateRoot());
        REQUIRE(!loadOffer(ls, source.getPublicKey(), expectedOfferID));
        break;
    }
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
    LedgerState ls(app.getLedgerStateRoot());
    auto lastGeneratedID = ls.loadHeader()->header().idPool;
    ls.rollback();

    auto expectedOfferID = lastGeneratedID + 1;

    auto op = createPassiveOffer(selling, buying, price, amount);
    auto tx = transactionFromOperations(app, source, seq, {op});

    try
    {
        applyTx(tx, app);
    }
    catch (...)
    {
        LedgerState ls(app.getLedgerStateRoot());
        REQUIRE(ls.loadHeader()->header().idPool == lastGeneratedID);
        ls.rollback();
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
            LedgerState ls(app.getLedgerStateRoot());
            auto offer = loadOffer(ls, source.getPublicKey(), expectedOfferID);
            REQUIRE(offer);
            auto const& offerEntry = offer->entry()->data.offer();
            REQUIRE(offerEntry == offerResult.offer());
            REQUIRE(offerEntry.price == price);
            REQUIRE(offerEntry.selling == selling);
            REQUIRE(offerEntry.buying == buying);
            REQUIRE((offerEntry.flags & PASSIVE_FLAG) != 0);
        }
        break;
        case MANAGE_OFFER_DELETED:
        {
            LedgerState ls(app.getLedgerStateRoot());
            REQUIRE(!loadOffer(ls, source.getPublicKey(), expectedOfferID));
            break;
        }
        default:
            abort();
        }
    }

    auto& success = createPassiveOfferResult.success().offer;

    REQUIRE(success.effect() == expectedEffect);

    return success.effect() == MANAGE_OFFER_CREATED ? success.offer().offerID
                                                    : 0;
}

Operation
setOptions(AccountID* inflationDest, uint32_t* setFlags, uint32_t* clearFlags,
           ThresholdSetter* thrs, Signer* signer, std::string* homeDomain)
{
    Operation op;
    op.body.type(SET_OPTIONS);

    SetOptionsOp& setOp = op.body.setOptionsOp();

    if (inflationDest)
    {
        setOp.inflationDest.activate() = *inflationDest;
    }

    if (setFlags)
    {
        setOp.setFlags.activate() = *setFlags;
    }

    if (clearFlags)
    {
        setOp.clearFlags.activate() = *clearFlags;
    }

    if (thrs)
    {
        if (thrs->masterWeight)
        {
            setOp.masterWeight.activate() = *thrs->masterWeight;
        }
        if (thrs->lowThreshold)
        {
            setOp.lowThreshold.activate() = *thrs->lowThreshold;
        }
        if (thrs->medThreshold)
        {
            setOp.medThreshold.activate() = *thrs->medThreshold;
        }
        if (thrs->highThreshold)
        {
            setOp.highThreshold.activate() = *thrs->highThreshold;
        }
    }

    if (signer)
    {
        setOp.signer.activate() = *signer;
    }

    if (homeDomain)
    {
        setOp.homeDomain.activate() = *homeDomain;
    }

    return op;
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
