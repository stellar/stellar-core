// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TxTests.h"

#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/ByteSlice.h"
#include "util/types.h"
#include "transactions/TransactionFrame.h"
#include "ledger/LedgerDelta.h"
#include "ledger/DataFrame.h"
#include "transactions/PathPaymentOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/ManageOfferOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "transactions/AllowTrustOpFrame.h"
#include "transactions/InflationOpFrame.h"
#include "transactions/MergeOpFrame.h"
#include "transactions/ManageDataOpFrame.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;
namespace stellar
{
using xdr::operator==;

namespace txtest
{

bool
applyCheck(TransactionFramePtr tx, LedgerDelta& delta, Application& app)
{
    bool check = tx->checkValid(app, 0);
    TransactionResult checkResult = tx->getResult();

    REQUIRE((!check || checkResult.result.code() == txSUCCESS));

    bool doApply;
    // valid transaction sets ensure that
    {
        auto code = checkResult.result.code();
        if (code != txNO_ACCOUNT && code != txBAD_SEQ)
        {
            tx->processFeeSeqNum(delta, app.getLedgerManager());
        }
        doApply = (code != txBAD_SEQ);
    }

    bool res;

    if (doApply)
    {
        res = tx->apply(delta, app);

        REQUIRE((!res || tx->getResultCode() == txSUCCESS));

        if (!check)
        {
            REQUIRE(checkResult == tx->getResult());
        }
    }
    else
    {
        res = check;
    }

    // verify modified accounts invariants
    auto const& changes = delta.getChanges();
    for (auto const& c : changes)
    {
        switch (c.type())
        {
        case LEDGER_ENTRY_CREATED:
            checkEntry(c.created(), app);
            break;
        case LEDGER_ENTRY_UPDATED:
            checkEntry(c.updated(), app);
            break;
        default:
            break;
        }
    }

    // validates db state
    app.getLedgerManager().checkDbState();

    return res;
}

void
checkEntry(LedgerEntry const& le, Application& app)
{
    auto& d = le.data;
    switch (d.type())
    {
    case ACCOUNT:
        checkAccount(d.account().accountID, app);
        break;
    case TRUSTLINE:
        checkAccount(d.trustLine().accountID, app);
        break;
    case OFFER:
        checkAccount(d.offer().sellerID, app);
        break;
    case DATA:
        checkAccount(d.data().accountID, app);
        break;
    default:
        break;
    }
}

void
checkAccount(AccountID const& id, Application& app)
{
    AccountFrame::pointer res =
        AccountFrame::loadAccount(id, app.getDatabase());
    REQUIRE(!!res);
    std::vector<TrustFrame::pointer> retLines;
    TrustFrame::loadLines(id, retLines, app.getDatabase());

    std::vector<OfferFrame::pointer> retOffers;
    OfferFrame::loadOffers(id, retOffers, app.getDatabase());

    std::vector<DataFrame::pointer> retDatas;
    DataFrame::loadAccountsData(id, retDatas, app.getDatabase());

    size_t actualSubEntries =
        res->getAccount().signers.size() + retLines.size() + retOffers.size() + retDatas.size();

    REQUIRE(res->getAccount().numSubEntries == (uint32)actualSubEntries);
}

time_t
getTestDate(int day, int month, int year)
{
    std::tm tm = {0};
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_mday = day;
    tm.tm_mon = month - 1; // 0 based
    tm.tm_year = year - 1900;

    VirtualClock::time_point tp = VirtualClock::tmToPoint(tm);
    time_t t = VirtualClock::to_time_t(tp);

    return t;
}

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, int day, int month, int year,
              TransactionFramePtr tx)
{
    TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
        app.getLedgerManager().getLastClosedLedgerHeader().hash);

    if (tx)
    {
        txSet->add(tx);
        txSet->sortForHash();
    }

    return closeLedgerOn(app, ledgerSeq, day, month, year, txSet);
}

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, int day, int month, int year,
              TxSetFramePtr txSet)
{

    StellarValue sv(txSet->getContentsHash(), getTestDate(day, month, year),
                    emptyUpgradeSteps, 0);
    LedgerCloseData ledgerData(ledgerSeq, txSet, sv);
    app.getLedgerManager().closeLedger(ledgerData);

    auto z1 = TransactionFrame::getTransactionHistoryResults(app.getDatabase(),
                                                             ledgerSeq);
    auto z2 =
        TransactionFrame::getTransactionFeeMeta(app.getDatabase(), ledgerSeq);

    REQUIRE(app.getLedgerManager().getLedgerNum() == (ledgerSeq + 1));

    TxSetResultMeta res;
    std::transform(z1.results.begin(), z1.results.end(), z2.begin(),
                   std::back_inserter(res), [](TransactionResultPair const& r1,
                                               LedgerEntryChanges const& r2)
                   {
                       return std::make_pair(r1, r2);
                   });

    return res;
}

void
upgradeToCurrentLedgerVersion(Application& app)
{
    auto const& lcl = app.getLedgerManager().getLastClosedLedgerHeader();
    auto const& lastHash = lcl.hash;
    TxSetFramePtr txSet = std::make_shared<TxSetFrame>(lastHash);

    LedgerUpgrade upgrade(LEDGER_UPGRADE_VERSION);
    upgrade.newLedgerVersion() = app.getConfig().LEDGER_PROTOCOL_VERSION;
    xdr::xvector<UpgradeType, 6> upgrades;
    Value v(xdr::xdr_to_opaque(upgrade));
    upgrades.emplace_back(v.begin(), v.end());

    StellarValue sv(txSet->getContentsHash(), 1, upgrades, 0);
    LedgerCloseData ledgerData(1, txSet, sv);
    app.getLedgerManager().closeLedger(ledgerData);
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

AccountFrame::pointer
loadAccount(SecretKey const& k, Application& app, bool mustExist)
{
    AccountFrame::pointer res =
        AccountFrame::loadAccount(k.getPublicKey(), app.getDatabase());
    if (mustExist)
    {
        REQUIRE(res);
    }
    return res;
}

void
requireNoAccount(SecretKey const& k, Application& app)
{
    AccountFrame::pointer res = loadAccount(k, app, false);
    REQUIRE(!res);
}

OfferFrame::pointer
loadOffer(SecretKey const& k, uint64 offerID, Application& app, bool mustExist)
{
    OfferFrame::pointer res =
        OfferFrame::loadOffer(k.getPublicKey(), offerID, app.getDatabase());
    if (mustExist)
    {
        REQUIRE(res);
    }
    return res;
}

TrustFrame::pointer
loadTrustLine(SecretKey const& k, Asset const& asset, Application& app,
              bool mustExist)
{
    TrustFrame::pointer res =
        TrustFrame::loadTrustLine(k.getPublicKey(), asset, app.getDatabase());
    if (mustExist)
    {
        REQUIRE(res);
    }
    return res;
}

SequenceNumber
getAccountSeqNum(SecretKey const& k, Application& app)
{
    AccountFrame::pointer account;
    account = loadAccount(k, app);
    return account->getSeqNum();
}

int64_t
getAccountBalance(SecretKey const& k, Application& app)
{
    AccountFrame::pointer account;
    account = loadAccount(k, app);
    return account->getBalance();
}

void
checkTransaction(TransactionFrame& txFrame)
{
    REQUIRE(txFrame.getResult().feeCharged == 100); // default fee
    REQUIRE((txFrame.getResultCode() == txSUCCESS ||
             txFrame.getResultCode() == txFAILED));
}

TransactionFramePtr
transactionFromOperation(Hash const& networkID, SecretKey& from,
                         SequenceNumber seq, Operation const& op)
{
    TransactionEnvelope e;

    e.tx.sourceAccount = from.getPublicKey();
    e.tx.fee = 100;
    e.tx.seqNum = seq;
    e.tx.operations.push_back(op);

    TransactionFramePtr res =
        TransactionFrame::makeTransactionFromWire(networkID, e);

    res->addSignature(from);

    return res;
}

TransactionFramePtr
createChangeTrust(Hash const& networkID, SecretKey& from, SecretKey& to,
                  SequenceNumber seq, std::string const& assetCode,
                  int64_t limit)
{
    Operation op;

    op.body.type(CHANGE_TRUST);
    op.body.changeTrustOp().limit = limit;
    op.body.changeTrustOp().line.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(op.body.changeTrustOp().line.alphaNum4().assetCode,
                   assetCode);
    op.body.changeTrustOp().line.alphaNum4().issuer = to.getPublicKey();

    return transactionFromOperation(networkID, from, seq, op);
}

TransactionFramePtr
createAllowTrust(Hash const& networkID, SecretKey& from, SecretKey& trustor,
                 SequenceNumber seq, std::string const& assetCode,
                 bool authorize)
{
    Operation op;

    op.body.type(ALLOW_TRUST);
    op.body.allowTrustOp().trustor = trustor.getPublicKey();
    op.body.allowTrustOp().asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(op.body.allowTrustOp().asset.assetCode4(), assetCode);
    op.body.allowTrustOp().authorize = authorize;

    return transactionFromOperation(networkID, from, seq, op);
}

void
applyAllowTrust(Application& app, SecretKey& from, SecretKey& trustor,
                SequenceNumber seq, std::string const& assetCode,
                bool authorize, AllowTrustResultCode result)
{
    TransactionFramePtr txFrame;
    txFrame = createAllowTrust(app.getNetworkID(), from, trustor, seq,
                               assetCode, authorize);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    REQUIRE(AllowTrustOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
}

TransactionFramePtr
createCreateAccountTx(Hash const& networkID, SecretKey& from, SecretKey& to,
                      SequenceNumber seq, int64_t amount)
{
    Operation op;
    op.body.type(CREATE_ACCOUNT);
    op.body.createAccountOp().startingBalance = amount;
    op.body.createAccountOp().destination = to.getPublicKey();

    return transactionFromOperation(networkID, from, seq, op);
}

void
applyCreateAccountTx(Application& app, SecretKey& from, SecretKey& to,
                     SequenceNumber seq, int64_t amount,
                     CreateAccountResultCode result)
{
    TransactionFramePtr txFrame;

    AccountFrame::pointer fromAccount, toAccount;
    toAccount = loadAccount(to, app, false);

    fromAccount = loadAccount(from, app);

    txFrame = createCreateAccountTx(app.getNetworkID(), from, to, seq, amount);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    auto txResult = txFrame->getResult();
    auto innerCode =
        CreateAccountOpFrame::getInnerCode(txResult.result.results()[0]);
    REQUIRE(innerCode == result);

    REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

    AccountFrame::pointer toAccountAfter;
    toAccountAfter = loadAccount(to, app, false);

    if (innerCode != CREATE_ACCOUNT_SUCCESS)
    {
        // check that the target account didn't change
        REQUIRE(!!toAccount == !!toAccountAfter);
        if (toAccount && toAccountAfter)
        {
            REQUIRE(toAccount->getAccount() == toAccountAfter->getAccount());
        }
    }
    else
    {
        REQUIRE(toAccountAfter);
    }
}

TransactionFramePtr
createPaymentTx(Hash const& networkID, SecretKey& from, SecretKey& to,
                SequenceNumber seq, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().destination = to.getPublicKey();
    op.body.paymentOp().asset.type(ASSET_TYPE_NATIVE);

    return transactionFromOperation(networkID, from, seq, op);
}

void
applyPaymentTx(Application& app, SecretKey& from, SecretKey& to,
               SequenceNumber seq, int64_t amount, PaymentResultCode result)
{
    TransactionFramePtr txFrame;

    AccountFrame::pointer fromAccount, toAccount;
    toAccount = loadAccount(to, app, false);

    fromAccount = loadAccount(from, app);

    txFrame = createPaymentTx(app.getNetworkID(), from, to, seq, amount);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    auto txResult = txFrame->getResult();
    auto innerCode = PaymentOpFrame::getInnerCode(txResult.result.results()[0]);
    REQUIRE(innerCode == result);

    REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

    AccountFrame::pointer toAccountAfter;
    toAccountAfter = loadAccount(to, app, false);

    if (innerCode != PAYMENT_SUCCESS)
    {
        // check that the target account didn't change
        REQUIRE(!!toAccount == !!toAccountAfter);
        if (toAccount && toAccountAfter)
        {
            REQUIRE(toAccount->getAccount() == toAccountAfter->getAccount());
        }
    }
    else
    {
        REQUIRE(toAccount);
        REQUIRE(toAccountAfter);
    }
}

void
applyChangeTrust(Application& app, SecretKey& from, SecretKey& to,
                 SequenceNumber seq, std::string const& assetCode,
                 int64_t limit, ChangeTrustResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame =
        createChangeTrust(app.getNetworkID(), from, to, seq, assetCode, limit);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    REQUIRE(ChangeTrustOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
}

TransactionFramePtr
createCreditPaymentTx(Hash const& networkID, SecretKey& from, SecretKey& to,
                      Asset& asset, SequenceNumber seq, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().asset = asset;
    op.body.paymentOp().destination = to.getPublicKey();

    return transactionFromOperation(networkID, from, seq, op);
}

Asset
makeAsset(SecretKey& issuer, std::string const& code)
{
    Asset asset;
    asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    asset.alphaNum4().issuer = issuer.getPublicKey();
    strToAssetCode(asset.alphaNum4().assetCode, code);
    return asset;
}

PaymentResult
applyCreditPaymentTx(Application& app, SecretKey& from, SecretKey& to,
                     Asset& ci, SequenceNumber seq, int64_t amount,
                     PaymentResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame =
        createCreditPaymentTx(app.getNetworkID(), from, to, ci, seq, amount);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);

    auto& firstResult = getFirstResult(*txFrame);

    PaymentResult res = firstResult.tr().paymentResult();
    auto resCode = res.code();
    REQUIRE(resCode == result);
    return res;
}

TransactionFramePtr
createPathPaymentTx(Hash const& networkID, SecretKey& from, SecretKey& to,
                    Asset const& sendCur, int64_t sendMax, Asset const& destCur,
                    int64_t destAmount, SequenceNumber seq,
                    std::vector<Asset>* path)
{
    Operation op;
    op.body.type(PATH_PAYMENT);
    PathPaymentOp& ppop = op.body.pathPaymentOp();
    ppop.sendAsset = sendCur;
    ppop.sendMax = sendMax;
    ppop.destAsset = destCur;
    ppop.destAmount = destAmount;
    ppop.destination = to.getPublicKey();
    if (path)
    {
        for (auto const& cur : *path)
        {
            ppop.path.push_back(cur);
        }
    }

    return transactionFromOperation(networkID, from, seq, op);
}

PathPaymentResult
applyPathPaymentTx(Application& app, SecretKey& from, SecretKey& to,
                   Asset const& sendCur, int64_t sendMax, Asset const& destCur,
                   int64_t destAmount, SequenceNumber seq,
                   PathPaymentResultCode result, std::vector<Asset>* path)
{
    TransactionFramePtr txFrame;

    txFrame = createPathPaymentTx(app.getNetworkID(), from, to, sendCur,
                                  sendMax, destCur, destAmount, seq, path);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);

    auto& firstResult = getFirstResult(*txFrame);

    PathPaymentResult res = firstResult.tr().pathPaymentResult();
    auto resCode = res.code();
    REQUIRE(resCode == result);
    return res;
}

TransactionFramePtr
createPassiveOfferOp(Hash const& networkID, SecretKey& source, Asset& selling,
                     Asset& buying, Price const& price, int64_t amount,
                     SequenceNumber seq)
{
    Operation op;
    op.body.type(CREATE_PASSIVE_OFFER);
    op.body.createPassiveOfferOp().amount = amount;
    op.body.createPassiveOfferOp().selling = selling;
    op.body.createPassiveOfferOp().buying = buying;
    op.body.createPassiveOfferOp().price = price;

    return transactionFromOperation(networkID, source, seq, op);
}

TransactionFramePtr
manageOfferOp(Hash const& networkID, uint64 offerId, SecretKey& source,
              Asset& selling, Asset& buying, Price const& price, int64_t amount,
              SequenceNumber seq)
{
    Operation op;
    op.body.type(MANAGE_OFFER);
    op.body.manageOfferOp().amount = amount;
    op.body.manageOfferOp().selling = selling;
    op.body.manageOfferOp().buying = buying;
    op.body.manageOfferOp().offerID = offerId;
    op.body.manageOfferOp().price = price;

    return transactionFromOperation(networkID, source, seq, op);
}

static ManageOfferResult
applyCreateOfferHelper(Application& app, LedgerDelta& delta, uint64 offerId,
                       SecretKey& source, Asset& selling, Asset& buying,
                       Price const& price, int64_t amount, SequenceNumber seq)
{
    uint64_t expectedOfferID = delta.getHeaderFrame().getLastGeneratedID() + 1;
    if (offerId != 0)
    {
        expectedOfferID = offerId;
    }

    TransactionFramePtr txFrame;

    txFrame = manageOfferOp(app.getNetworkID(), offerId, source, selling,
                            buying, price, amount, seq);

    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);

    auto& results = txFrame->getResult().result.results();

    REQUIRE(results.size() == 1);

    auto& manageOfferResult = results[0].tr().manageOfferResult();

    if (manageOfferResult.code() == MANAGE_OFFER_SUCCESS)
    {
        OfferFrame::pointer offer;

        auto& offerResult = manageOfferResult.success().offer;

        switch (offerResult.effect())
        {
        case MANAGE_OFFER_CREATED:
        case MANAGE_OFFER_UPDATED:
        {
            offer = loadOffer(source, expectedOfferID, app);
            auto& offerEntry = offer->getOffer();
            REQUIRE(offerEntry == offerResult.offer());
            REQUIRE(offerEntry.price == price);
            REQUIRE(offerEntry.selling == selling);
            REQUIRE(offerEntry.buying == buying);
        }
        break;
        case MANAGE_OFFER_DELETED:
            REQUIRE(!loadOffer(source, expectedOfferID, app, false));
            break;
        default:
            abort();
        }
    }

    return manageOfferResult;
}

uint64_t
applyCreateOffer(Application& app, LedgerDelta& delta, uint64 offerId,
                 SecretKey& source, Asset& selling, Asset& buying,
                 Price const& price, int64_t amount, SequenceNumber seq)
{
    ManageOfferResult const& createOfferRes = applyCreateOfferHelper(
        app, delta, offerId, source, selling, buying, price, amount, seq);

    REQUIRE(createOfferRes.code() == MANAGE_OFFER_SUCCESS);

    auto& success = createOfferRes.success().offer;

    REQUIRE(success.effect() == MANAGE_OFFER_CREATED);

    return success.offer().offerID;
}

ManageOfferResult
applyCreateOfferWithResult(Application& app, LedgerDelta& delta, uint64 offerId,
                           SecretKey& source, Asset& selling, Asset& buying,
                           Price const& price, int64_t amount,
                           SequenceNumber seq, ManageOfferResultCode result)
{
    ManageOfferResult const& manageOfferRes = applyCreateOfferHelper(
        app, delta, offerId, source, selling, buying, price, amount, seq);

    auto res = manageOfferRes.code();
    REQUIRE(res == result);

    return manageOfferRes;
}

TransactionFramePtr
createSetOptions(Hash const& networkID, SecretKey& source, SequenceNumber seq,
                 AccountID* inflationDest, uint32_t* setFlags,
                 uint32_t* clearFlags, ThresholdSetter* thrs, Signer* signer,
                 std::string* homeDomain)
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

    return transactionFromOperation(networkID, source, seq, op);
}

void
applySetOptions(Application& app, SecretKey& source, SequenceNumber seq,
                AccountID* inflationDest, uint32_t* setFlags,
                uint32_t* clearFlags, ThresholdSetter* thrs, Signer* signer,
                std::string* homeDomain, SetOptionsResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createSetOptions(app.getNetworkID(), source, seq, inflationDest,
                               setFlags, clearFlags, thrs, signer, homeDomain);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    REQUIRE(SetOptionsOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
}

TransactionFramePtr
createInflation(Hash const& networkID, SecretKey& from, SequenceNumber seq)
{
    Operation op;
    op.body.type(INFLATION);

    return transactionFromOperation(networkID, from, seq, op);
}

OperationResult
applyInflation(Application& app, SecretKey& from, SequenceNumber seq,
               InflationResultCode result)
{
    TransactionFramePtr txFrame =
        createInflation(app.getNetworkID(), from, seq);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    bool res = applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    REQUIRE(InflationOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
    if (res)
    {
        delta.commit();
    }
    return getFirstResult(*txFrame);
}

TransactionFramePtr
createAccountMerge(Hash const& networkID, SecretKey& source, SecretKey& dest,
                   SequenceNumber seq)
{
    Operation op;
    op.body.type(ACCOUNT_MERGE);
    op.body.destination() = dest.getPublicKey();

    return transactionFromOperation(networkID, source, seq, op);
}

void
applyAccountMerge(Application& app, SecretKey& source, SecretKey& dest,
                  SequenceNumber seq, AccountMergeResultCode targetResult)
{
    TransactionFramePtr txFrame =
        createAccountMerge(app.getNetworkID(), source, dest, seq);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    applyCheck(txFrame, delta, app);

    REQUIRE(MergeOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == targetResult);
}

TransactionFramePtr
createManageData(Hash const& networkID, SecretKey& source,
    std::string& name, DataValue* value, SequenceNumber seq)
{
    Operation op;
    op.body.type(MANAGE_DATA);
    op.body.manageDataOp().dataName = name;
    if(value)
        op.body.manageDataOp().dataValue.activate() = *value;

    return transactionFromOperation(networkID, source, seq, op);
}

void
applyManageData( Application& app,
    SecretKey& source, std::string& name, DataValue* value,
    SequenceNumber seq, ManageDataResultCode targetResult)
{
    TransactionFramePtr txFrame =
        createManageData(app.getNetworkID(), source, name, value, seq);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
        app.getDatabase());
    applyCheck(txFrame, delta, app);

    REQUIRE(ManageDataOpFrame::getInnerCode(
        txFrame->getResult().result.results()[0]) == targetResult);

    if(targetResult==MANAGE_DATA_SUCCESS)
    {
        auto dataFrame=DataFrame::loadData(source.getPublicKey(), name, app.getDatabase());
        if(value)
        {
            REQUIRE(dataFrame != nullptr);
            REQUIRE(dataFrame->getData().dataValue == *value);
        } else
        {
            REQUIRE(dataFrame == nullptr);
        }
    }
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

Operation&
getFirstOperation(TransactionFrame& tx)
{
    return tx.getEnvelope().tx.operations[0];
}

void
reSignTransaction(TransactionFrame& tx, SecretKey& source)
{
    tx.getEnvelope().signatures.clear();
    tx.addSignature(source);
}

void
checkAmounts(int64_t a, int64_t b, int64_t maxd)
{
    int64_t d = b - maxd;
    REQUIRE(a >= d);
    REQUIRE(a <= b);
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
