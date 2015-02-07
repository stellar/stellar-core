// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/Base58.h"
#include "lib/json/json.h"
#include "transactions/TxTests.h"
#include "util/types.h"
#include "transactions/TransactionFrame.h"
#include "ledger/LedgerDelta.h"
#include "transactions/PaymentFrame.h"
#include "transactions/ChangeTrustTxFrame.h"
#include "transactions/CreateOfferFrame.h"
#include "transactions/SetOptionsFrame.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;
namespace stellar
{
namespace txtest
{
SecretKey getRoot()
{
    std::string b58SeedStr = toBase58Check(VER_SEED, "masterpassphrasemasterpassphrase");
    return SecretKey::fromBase58Seed(b58SeedStr);
}

SecretKey getAccount(const char* n)
{
    // stretch name to 32 bytes
    std::string name(n);
    while(name.size() < 32) name += '.';
    std::string b58SeedStr = toBase58Check(VER_SEED, name);
    return SecretKey::fromBase58Seed(b58SeedStr);
}

TransactionFramePtr changeTrust(SecretKey& from, SecretKey& to, uint32_t seq, const std::string& currencyCode, int64_t limit)
{
    TransactionEnvelope txEnvelope;
    txEnvelope.tx.body.type(CHANGE_TRUST);
    txEnvelope.tx.account = from.getPublicKey();
    txEnvelope.tx.maxFee = 12;
    txEnvelope.tx.maxLedger = 1000;
    txEnvelope.tx.minLedger = 0;
    txEnvelope.tx.seqNum = seq;
    txEnvelope.tx.body.changeTrustTx().limit = limit;
    txEnvelope.tx.body.changeTrustTx().line.type(ISO4217);
    strToCurrencyCode(txEnvelope.tx.body.changeTrustTx().line.isoCI().currencyCode,currencyCode);
    txEnvelope.tx.body.changeTrustTx().line.isoCI().issuer = to.getPublicKey();

    TransactionFramePtr res = TransactionFrame::makeTransactionFromWire(txEnvelope);

    res->addSignature(from);
    return res;
}

TransactionFramePtr createPaymentTx(SecretKey& from, SecretKey& to, uint32_t seq, int64_t amount)
{
    TransactionEnvelope txEnvelope;
    txEnvelope.tx.body.type(PAYMENT);
    txEnvelope.tx.account = from.getPublicKey();
    txEnvelope.tx.maxFee = 12;
    txEnvelope.tx.maxLedger = 1000;
    txEnvelope.tx.minLedger = 0;
    txEnvelope.tx.seqNum = seq;
    txEnvelope.tx.body.paymentTx().amount = amount;
    txEnvelope.tx.body.paymentTx().destination = to.getPublicKey();
    txEnvelope.tx.body.paymentTx().sendMax = INT64_MAX;
    txEnvelope.tx.body.paymentTx().currency.type(NATIVE);

    TransactionFramePtr res = TransactionFrame::makeTransactionFromWire(txEnvelope);

    res->addSignature(from);

    return res;
}

void applyPaymentTx(Application& app, SecretKey& from, SecretKey& to, uint32_t seq, int64_t amount, Payment::PaymentResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createPaymentTx(from, to, seq, amount);

    LedgerDelta delta;
    txFrame->apply(delta, app);

    REQUIRE(Payment::getInnerCode(txFrame->getResult()) == result);
}

void applyChangeTrust(Application& app, SecretKey& from, SecretKey& to, uint32_t seq, const std::string& currencyCode, int64_t limit, ChangeTrust::ChangeTrustResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = changeTrust(from, to, seq, currencyCode, limit);

    LedgerDelta delta;
    txFrame->apply(delta, app);

    REQUIRE(ChangeTrust::getInnerCode(txFrame->getResult()) == result);
}

TransactionFramePtr createCreditPaymentTx(SecretKey& from, SecretKey& to, Currency& ci,uint32_t seq, int64_t amount)
{
    TransactionEnvelope txEnvelope;
    txEnvelope.tx.body.type(PAYMENT);
    txEnvelope.tx.account = from.getPublicKey();
    txEnvelope.tx.maxFee = 12;
    txEnvelope.tx.maxLedger = 1000;
    txEnvelope.tx.minLedger = 0;
    txEnvelope.tx.seqNum = seq;
    txEnvelope.tx.body.paymentTx().amount = amount;
    txEnvelope.tx.body.paymentTx().currency = ci;
    txEnvelope.tx.body.paymentTx().destination = to.getPublicKey();
    txEnvelope.tx.body.paymentTx().sendMax = INT64_MAX;

    TransactionFramePtr res = TransactionFrame::makeTransactionFromWire(txEnvelope);

    res->addSignature(from);

    return res;
}

Currency makeCurrency(SecretKey& issuer, const std::string& code)
{
    Currency currency;
    currency.type(ISO4217);
    currency.isoCI().issuer = issuer.getPublicKey();
    strToCurrencyCode(currency.isoCI().currencyCode, code);
    return currency;
}

void applyCreditPaymentTx(Application& app, SecretKey& from, SecretKey& to, Currency& ci, uint32_t seq,
    int64_t amount, Payment::PaymentResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createCreditPaymentTx(from,to,ci,seq,amount);

    LedgerDelta delta;
    txFrame->apply(delta, app);

    REQUIRE(Payment::getInnerCode(txFrame->getResult()) == result);
}

TransactionFramePtr createOfferTx(SecretKey& source, Currency& takerGets,
    Currency& takerPays, uint64_t price, int64_t amount, uint32_t seq)
{
    TransactionEnvelope txEnvelope;
    txEnvelope.tx.body.type(CREATE_OFFER);
    txEnvelope.tx.account = source.getPublicKey();
    txEnvelope.tx.maxFee = 12;
    txEnvelope.tx.maxLedger = 1000;
    txEnvelope.tx.minLedger = 0;
    txEnvelope.tx.seqNum = seq;
    txEnvelope.tx.body.createOfferTx().amount = amount;
    txEnvelope.tx.body.createOfferTx().takerGets = takerGets;
    txEnvelope.tx.body.createOfferTx().takerPays = takerPays;
    txEnvelope.tx.body.createOfferTx().price = price;
    txEnvelope.tx.body.createOfferTx().sequence = seq;

    TransactionFramePtr res = TransactionFrame::makeTransactionFromWire(txEnvelope);

    res->addSignature(source);

    return res;
}

void applyOffer(Application& app, SecretKey& source, Currency& takerGets,
    Currency& takerPays, uint64_t price, int64_t amount, uint32_t seq, CreateOffer::CreateOfferResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createOfferTx(source, takerGets,takerPays,price,amount, seq);

    LedgerDelta delta;
    txFrame->apply(delta, app);

    REQUIRE(CreateOffer::getInnerCode(txFrame->getResult()) == result);
}

TransactionFramePtr createSetOptions(SecretKey& source, AccountID *inflationDest,
    uint32_t *setFlags, uint32_t *clearFlags, KeyValue *data, Thresholds *thrs,
    Signer *signer, uint32_t seq)
{
    TransactionEnvelope txEnvelope;
    txEnvelope.tx.body.type(SET_OPTIONS);
    txEnvelope.tx.account = source.getPublicKey();
    txEnvelope.tx.maxFee = 12;
    txEnvelope.tx.maxLedger = 1000;
    txEnvelope.tx.minLedger = 0;
    txEnvelope.tx.seqNum = seq;

    if (inflationDest)
    {
        txEnvelope.tx.body.setOptionsTx().inflationDest.activate() = *inflationDest;
    }

    if (setFlags)
    {
        txEnvelope.tx.body.setOptionsTx().setFlags.activate() = *setFlags;
    }

    if (clearFlags)
    {
        txEnvelope.tx.body.setOptionsTx().clearFlags.activate() = *clearFlags;
    }

    if (data)
    {
        txEnvelope.tx.body.setOptionsTx().data.activate() = *data;
    }

    if (thrs)
    {
        txEnvelope.tx.body.setOptionsTx().thresholds.activate() = *thrs;
    }

    if (signer)
    {
        txEnvelope.tx.body.setOptionsTx().signer.activate() = *signer;
    }

    TransactionFramePtr res = TransactionFrame::makeTransactionFromWire(txEnvelope);

    res->addSignature(source);

    return res;
}

void applySetOptions(Application& app, SecretKey& source, AccountID *inflationDest,
    uint32_t *setFlags, uint32_t *clearFlags, KeyValue *data, Thresholds *thrs,
    Signer *signer, uint32_t seq, SetOptions::SetOptionsResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createSetOptions(source, inflationDest,
        setFlags, clearFlags, data, thrs,
        signer, seq);

    LedgerDelta delta;
    txFrame->apply(delta, app);

    REQUIRE(SetOptions::getInnerCode(txFrame->getResult()) == result);
}


}
}

