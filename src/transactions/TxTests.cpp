// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/Base58.h"
#include "transactions/TxTests.h"
#include "util/types.h"
#include "transactions/TransactionFrame.h"
#include "ledger/LedgerDelta.h"
#include "transactions/PathPaymentOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/ManageOfferOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "transactions/AllowTrustOpFrame.h"
#include "transactions/InflationOpFrame.h"
#include "transactions/MergeOpFrame.h"

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
    bool res = tx->apply(delta, app);

    if (!check)
    {
        REQUIRE(checkResult == tx->getResult());
    }

    return res;
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

void
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

    StellarValue sv(txSet->getContentsHash(), getTestDate(day, month, year),
                    emptyUpgradeSteps, 0);
    LedgerCloseData ledgerData(ledgerSeq, txSet, sv);
    app.getLedgerManager().closeLedger(ledgerData);

    REQUIRE(app.getLedgerManager().getLedgerNum() == (ledgerSeq + 1));
}

SecretKey
getRoot()
{
    std::string b58SeedStr =
        toBase58Check(B58_SEED_ED25519, "allmylifemyhearthasbeensearching");
    return SecretKey::fromBase58Seed(b58SeedStr);
}

SecretKey
getAccount(const char* n)
{
    // stretch name to 32 bytes
    std::string name(n);
    while (name.size() < 32)
        name += '.';
    std::string b58SeedStr = toBase58Check(B58_SEED_ED25519, name);
    return SecretKey::fromBase58Seed(b58SeedStr);
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
loadTrustLine(SecretKey const& k, Currency const& currency, Application& app,
              bool mustExist)
{
    TrustFrame::pointer res = TrustFrame::loadTrustLine(
        k.getPublicKey(), currency, app.getDatabase());
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

uint64_t
getAccountBalance(SecretKey const& k, Application& app)
{
    AccountFrame::pointer account;
    account = loadAccount(k, app);
    return account->getBalance();
}

void
checkTransaction(TransactionFrame& txFrame)
{
    REQUIRE(txFrame.getResult().feeCharged == 10); // default fee
    REQUIRE((txFrame.getResultCode() == txSUCCESS ||
             txFrame.getResultCode() == txFAILED));
}

TransactionFramePtr
transactionFromOperation(SecretKey& from, SequenceNumber seq,
                         Operation const& op)
{
    TransactionEnvelope e;

    e.tx.sourceAccount = from.getPublicKey();
    e.tx.fee = 10;
    e.tx.seqNum = seq;
    e.tx.operations.push_back(op);

    TransactionFramePtr res = TransactionFrame::makeTransactionFromWire(e);

    res->addSignature(from);

    return res;
}

TransactionFramePtr
createChangeTrust(SecretKey& from, SecretKey& to, SequenceNumber seq,
                  std::string const& currencyCode, int64_t limit)
{
    Operation op;

    op.body.type(CHANGE_TRUST);
    op.body.changeTrustOp().limit = limit;
    op.body.changeTrustOp().line.type(CURRENCY_TYPE_ALPHANUM);
    strToCurrencyCode(op.body.changeTrustOp().line.alphaNum().currencyCode,
                      currencyCode);
    op.body.changeTrustOp().line.alphaNum().issuer = to.getPublicKey();

    return transactionFromOperation(from, seq, op);
}

TransactionFramePtr
createAllowTrust(SecretKey& from, SecretKey& trustor, SequenceNumber seq,
                 std::string const& currencyCode, bool authorize)
{
    Operation op;

    op.body.type(ALLOW_TRUST);
    op.body.allowTrustOp().trustor = trustor.getPublicKey();
    op.body.allowTrustOp().currency.type(CURRENCY_TYPE_ALPHANUM);
    strToCurrencyCode(op.body.allowTrustOp().currency.currencyCode(),
                      currencyCode);
    op.body.allowTrustOp().authorize = authorize;

    return transactionFromOperation(from, seq, op);
}

void
applyAllowTrust(Application& app, SecretKey& from, SecretKey& trustor,
                SequenceNumber seq, std::string const& currencyCode,
                bool authorize, AllowTrustResultCode result)
{
    TransactionFramePtr txFrame;
    txFrame = createAllowTrust(from, trustor, seq, currencyCode, authorize);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    REQUIRE(AllowTrustOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
}

TransactionFramePtr
createCreateAccountTx(SecretKey& from, SecretKey& to, SequenceNumber seq,
                      int64_t amount)
{
    Operation op;
    op.body.type(CREATE_ACCOUNT);
    op.body.createAccountOp().startingBalance = amount;
    op.body.createAccountOp().destination = to.getPublicKey();

    return transactionFromOperation(from, seq, op);
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

    txFrame = createCreateAccountTx(from, to, seq, amount);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
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
createPaymentTx(SecretKey& from, SecretKey& to, SequenceNumber seq,
                int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().destination = to.getPublicKey();
    op.body.paymentOp().currency.type(CURRENCY_TYPE_NATIVE);

    return transactionFromOperation(from, seq, op);
}

void
applyPaymentTx(Application& app, SecretKey& from, SecretKey& to,
               SequenceNumber seq, int64_t amount, PaymentResultCode result)
{
    TransactionFramePtr txFrame;

    AccountFrame::pointer fromAccount, toAccount;
    toAccount = loadAccount(to, app, false);

    fromAccount = loadAccount(from, app);

    txFrame = createPaymentTx(from, to, seq, amount);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
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
                 SequenceNumber seq, std::string const& currencyCode,
                 int64_t limit, ChangeTrustResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createChangeTrust(from, to, seq, currencyCode, limit);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    REQUIRE(ChangeTrustOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
}

TransactionFramePtr
createCreditPaymentTx(SecretKey& from, SecretKey& to, Currency& ci,
                      SequenceNumber seq, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().currency = ci;
    op.body.paymentOp().destination = to.getPublicKey();

    return transactionFromOperation(from, seq, op);
}

Currency
makeCurrency(SecretKey& issuer, std::string const& code)
{
    Currency currency;
    currency.type(CURRENCY_TYPE_ALPHANUM);
    currency.alphaNum().issuer = issuer.getPublicKey();
    strToCurrencyCode(currency.alphaNum().currencyCode, code);
    return currency;
}

PaymentResult
applyCreditPaymentTx(Application& app, SecretKey& from, SecretKey& to,
                     Currency& ci, SequenceNumber seq, int64_t amount,
                     PaymentResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createCreditPaymentTx(from, to, ci, seq, amount);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);

    auto& firstResult = getFirstResult(*txFrame);

    PaymentResult res = firstResult.tr().paymentResult();
    auto resCode = res.code();
    REQUIRE(resCode == result);
    return res;
}

TransactionFramePtr
createPathPaymentTx(SecretKey& from, SecretKey& to, Currency const& sendCur,
                    int64_t sendMax, Currency const& destCur,
                    int64_t destAmount, SequenceNumber seq,
                    std::vector<Currency>* path)
{
    Operation op;
    op.body.type(PATH_PAYMENT);
    PathPaymentOp& ppop = op.body.pathPaymentOp();
    ppop.sendCurrency = sendCur;
    ppop.sendMax = sendMax;
    ppop.destCurrency = destCur;
    ppop.destAmount = destAmount;
    ppop.destination = to.getPublicKey();
    if (path)
    {
        for (auto const& cur : *path)
        {
            ppop.path.push_back(cur);
        }
    }

    return transactionFromOperation(from, seq, op);
}

PathPaymentResult
applyPathPaymentTx(Application& app, SecretKey& from, SecretKey& to,
                   Currency const& sendCur, int64_t sendMax,
                   Currency const& destCur, int64_t destAmount,
                   SequenceNumber seq, PathPaymentResultCode result,
                   std::vector<Currency>* path)
{
    TransactionFramePtr txFrame;

    txFrame = createPathPaymentTx(from, to, sendCur, sendMax, destCur,
                                  destAmount, seq, path);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);

    auto& firstResult = getFirstResult(*txFrame);

    PathPaymentResult res = firstResult.tr().pathPaymentResult();
    auto resCode = res.code();
    REQUIRE(resCode == result);
    return res;
}

TransactionFramePtr
createPassiveOfferOp(SecretKey& source, Currency& takerGets,
                     Currency& takerPays, Price const& price, int64_t amount,
                     SequenceNumber seq)
{
    Operation op;
    op.body.type(CREATE_PASSIVE_OFFER);
    op.body.createPassiveOfferOp().amount = amount;
    op.body.createPassiveOfferOp().takerGets = takerGets;
    op.body.createPassiveOfferOp().takerPays = takerPays;
    op.body.createPassiveOfferOp().price = price;

    return transactionFromOperation(source, seq, op);
}

TransactionFramePtr
manageOfferOp(uint64 offerId, SecretKey& source, Currency& takerGets,
              Currency& takerPays, Price const& price, int64_t amount,
              SequenceNumber seq)
{
    Operation op;
    op.body.type(MANAGE_OFFER);
    op.body.manageOfferOp().amount = amount;
    op.body.manageOfferOp().takerGets = takerGets;
    op.body.manageOfferOp().takerPays = takerPays;
    op.body.manageOfferOp().offerID = offerId;
    op.body.manageOfferOp().price = price;

    return transactionFromOperation(source, seq, op);
}

static ManageOfferResult
applyCreateOfferHelper(Application& app, LedgerDelta& delta, uint64 offerId,
                       SecretKey& source, Currency& takerGets,
                       Currency& takerPays, Price const& price, int64_t amount,
                       SequenceNumber seq)
{
    uint64_t expectedOfferID = delta.getHeaderFrame().getLastGeneratedID() + 1;
    if (offerId != 0)
    {
        expectedOfferID = offerId;
    }

    TransactionFramePtr txFrame;

    txFrame = manageOfferOp(offerId, source, takerGets, takerPays, price,
                            amount, seq);

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
            REQUIRE(offerEntry.takerGets == takerGets);
            REQUIRE(offerEntry.takerPays == takerPays);
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
                 SecretKey& source, Currency& takerGets, Currency& takerPays,
                 Price const& price, int64_t amount, SequenceNumber seq)
{
    ManageOfferResult const& createOfferRes = applyCreateOfferHelper(
        app, delta, offerId, source, takerGets, takerPays, price, amount, seq);

    REQUIRE(createOfferRes.code() == MANAGE_OFFER_SUCCESS);

    auto& success = createOfferRes.success().offer;

    REQUIRE(success.effect() == MANAGE_OFFER_CREATED);

    return success.offer().offerID;
}

ManageOfferResult
applyCreateOfferWithResult(Application& app, LedgerDelta& delta, uint64 offerId,
                           SecretKey& source, Currency& takerGets,
                           Currency& takerPays, Price const& price,
                           int64_t amount, SequenceNumber seq,
                           ManageOfferResultCode result)
{
    ManageOfferResult const& manageOfferRes = applyCreateOfferHelper(
        app, delta, offerId, source, takerGets, takerPays, price, amount, seq);

    auto res = manageOfferRes.code();
    REQUIRE(res == result);

    return manageOfferRes;
}

TransactionFramePtr
createSetOptions(SecretKey& source, SequenceNumber seq,
                 AccountID* inflationDest, uint32_t* setFlags,
                 uint32_t* clearFlags, ThresholdSetter* thrs, Signer* signer)
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

    return transactionFromOperation(source, seq, op);
}

void
applySetOptions(Application& app, SecretKey& source, SequenceNumber seq,
                AccountID* inflationDest, uint32_t* setFlags,
                uint32_t* clearFlags, ThresholdSetter* thrs, Signer* signer,
                SetOptionsResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createSetOptions(source, seq, inflationDest, setFlags, clearFlags,
                               thrs, signer);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    REQUIRE(SetOptionsOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
}

TransactionFramePtr
createInflation(SecretKey& from, SequenceNumber seq)
{
    Operation op;
    op.body.type(INFLATION);

    return transactionFromOperation(from, seq, op);
}

OperationResult
applyInflation(Application& app, SecretKey& from, SequenceNumber seq,
               InflationResultCode result)
{
    TransactionFramePtr txFrame = createInflation(from, seq);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
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
createAccountMerge(SecretKey& source, SecretKey& dest, SequenceNumber seq)
{
    Operation op;
    op.body.type(ACCOUNT_MERGE);
    op.body.destination() = dest.getPublicKey();

    return transactionFromOperation(source, seq, op);
}

void
applyAccountMerge(Application& app, SecretKey& source, SecretKey& dest,
                  SequenceNumber seq, AccountMergeResultCode result)
{
    TransactionFramePtr txFrame = createAccountMerge(source, dest, seq);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    applyCheck(txFrame, delta, app);

    REQUIRE(MergeOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
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
}
}
