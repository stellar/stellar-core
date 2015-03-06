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
#include "transactions/AllowTrustTxFrame.h"

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

void checkTransaction(TransactionFrame& txFrame)
{
    REQUIRE(txFrame.getResult().feeCharged == 10); // default fee
    REQUIRE((txFrame.getResultCode() == txSUCCESS ||
        txFrame.getResultCode() == txFAILED));
}

TransactionFramePtr transactionFromOperation(SecretKey& from, uint32_t seq, Operation const& op)
{
    TransactionEnvelope e;

    e.tx.account = from.getPublicKey();
    e.tx.maxLedger = 1000;
    e.tx.minLedger = 0;
    e.tx.maxFee = 12;
    e.tx.seqNum = seq;
    e.tx.seqSlot = 0;
    e.tx.operations.push_back(op);

    TransactionFramePtr res = TransactionFrame::makeTransactionFromWire(e);

    res->addSignature(from);

    return res;
}

TransactionFramePtr createChangeTrust(SecretKey& from, SecretKey& to, uint32_t seq, const std::string& currencyCode, int64_t limit)
{
    Operation op;

    op.body.type(CHANGE_TRUST);
    op.body.changeTrustTx().limit = limit;
    op.body.changeTrustTx().line.type(ISO4217);
    strToCurrencyCode(op.body.changeTrustTx().line.isoCI().currencyCode,currencyCode);
    op.body.changeTrustTx().line.isoCI().issuer = to.getPublicKey();

    return transactionFromOperation(from, seq, op);
}

TransactionFramePtr createAllowTrust(SecretKey& from, SecretKey& trustor, uint32_t seq,
    const std::string& currencyCode, bool authorize)
{
    Operation op;

    op.body.type(ALLOW_TRUST);
    op.body.allowTrustTx().trustor = trustor.getPublicKey();
    op.body.allowTrustTx().currency.type(ISO4217);
    strToCurrencyCode(op.body.allowTrustTx().currency.currencyCode(), currencyCode);
    op.body.allowTrustTx().authorize = authorize;

    return transactionFromOperation(from, seq, op);
}

void applyAllowTrust(Application& app, SecretKey& from, SecretKey& trustor, uint32_t seq,
    const std::string& currencyCode, bool authorize, AllowTrust::AllowTrustResultCode result)
{
    TransactionFramePtr txFrame;
    txFrame = createAllowTrust(from, trustor, seq, currencyCode, authorize);

    LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());
    txFrame->apply(delta, app);

    checkTransaction(*txFrame);
    REQUIRE(AllowTrust::getInnerCode(txFrame->getResult().result.results()[0]) == result);

}

TransactionFramePtr createPaymentTx(SecretKey& from, SecretKey& to, uint32_t seq, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentTx().amount = amount;
    op.body.paymentTx().destination = to.getPublicKey();
    op.body.paymentTx().sendMax = INT64_MAX;
    op.body.paymentTx().currency.type(NATIVE);

    return transactionFromOperation(from, seq, op);
}

void applyPaymentTx(Application& app, SecretKey& from, SecretKey& to, uint32_t seq, int64_t amount, Payment::PaymentResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createPaymentTx(from, to, seq, amount);

    LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());
    txFrame->apply(delta, app);

    checkTransaction(*txFrame);
    REQUIRE(Payment::getInnerCode(txFrame->getResult().result.results()[0]) == result);
}

void applyChangeTrust(Application& app, SecretKey& from, SecretKey& to, uint32_t seq, const std::string& currencyCode, int64_t limit, ChangeTrust::ChangeTrustResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createChangeTrust(from, to, seq, currencyCode, limit);

    LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());
    txFrame->apply(delta, app);

    checkTransaction(*txFrame);
    REQUIRE(ChangeTrust::getInnerCode(txFrame->getResult().result.results()[0]) == result);
}

TransactionFramePtr createCreditPaymentTx(SecretKey& from, SecretKey& to, Currency& ci,uint32_t seq, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentTx().amount = amount;
    op.body.paymentTx().currency = ci;
    op.body.paymentTx().destination = to.getPublicKey();
    op.body.paymentTx().sendMax = INT64_MAX;

    return transactionFromOperation(from, seq, op);
}

Currency makeCurrency(SecretKey& issuer, const std::string& code)
{
    Currency currency;
    currency.type(ISO4217);
    currency.isoCI().issuer = issuer.getPublicKey();
    strToCurrencyCode(currency.isoCI().currencyCode, code);
    return currency;
}

void applyCreditPaymentTx(Application& app, SecretKey& from, SecretKey& to, 
    Currency& ci, uint32_t seq,
    int64_t amount, Payment::PaymentResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createCreditPaymentTx(from,to,ci,seq,amount);

    LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());
    txFrame->apply(delta, app);

    checkTransaction(*txFrame);
    REQUIRE(Payment::getInnerCode(txFrame->getResult().result.results()[0]) == result);
}

TransactionFramePtr createOfferTx(SecretKey& source, Currency& takerGets,
    Currency& takerPays, Price const &price, int64_t amount, uint32_t seq)
{
    Operation op;
    op.body.type(CREATE_OFFER);
    op.body.createOfferTx().amount = amount;
    op.body.createOfferTx().takerGets = takerGets;
    op.body.createOfferTx().takerPays = takerPays;
    op.body.createOfferTx().price = price;

    return transactionFromOperation(source, seq, op);
}

void applyCreateOffer(Application& app, LedgerDelta& delta, SecretKey& source, Currency& takerGets,
    Currency& takerPays, Price const& price, int64_t amount, uint32_t seq, 
    CreateOffer::CreateOfferResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createOfferTx(source, takerGets,takerPays,price,amount, seq);

    txFrame->apply(delta, app);

    checkTransaction(*txFrame);
    REQUIRE(CreateOffer::getInnerCode(txFrame->getResult().result.results()[0]) == result);
}

TransactionFramePtr createSetOptions(SecretKey& source, AccountID *inflationDest,
    uint32_t *setFlags, uint32_t *clearFlags, KeyValue *data, Thresholds *thrs,
    Signer *signer, uint32_t seq)
{
    Operation op;
    op.body.type(SET_OPTIONS);

    if (inflationDest)
    {
        op.body.setOptionsTx().inflationDest.activate() = *inflationDest;
    }

    if (setFlags)
    {
        op.body.setOptionsTx().setFlags.activate() = *setFlags;
    }

    if (clearFlags)
    {
        op.body.setOptionsTx().clearFlags.activate() = *clearFlags;
    }

    if (data)
    {
        op.body.setOptionsTx().data.activate() = *data;
    }

    if (thrs)
    {
        op.body.setOptionsTx().thresholds.activate() = *thrs;
    }

    if (signer)
    {
        op.body.setOptionsTx().signer.activate() = *signer;
    }

    return transactionFromOperation(source, seq, op);
}

void applySetOptions(Application& app, SecretKey& source, AccountID *inflationDest,
    uint32_t *setFlags, uint32_t *clearFlags, KeyValue *data, Thresholds *thrs,
    Signer *signer, uint32_t seq, SetOptions::SetOptionsResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createSetOptions(source, inflationDest,
        setFlags, clearFlags, data, thrs,
        signer, seq);

    LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());
    txFrame->apply(delta, app);

    checkTransaction(*txFrame);
    REQUIRE(SetOptions::getInnerCode(txFrame->getResult().result.results()[0]) == result);
}

OperationFrame& getFirstOperationFrame(TransactionFrame& tx)
{
    return *(tx.getOperations()[0]);
}

OperationResult& getFirstResult(TransactionFrame& tx)
{
    return getFirstOperationFrame(tx).getResult();
}

OperationResultCode getFirstResultCode(TransactionFrame& tx)
{
    return getFirstOperationFrame(tx).getResultCode();
}

Operation& getFirstOperation(TransactionFrame& tx)
{
    return tx.getEnvelope().tx.operations[0];
}

}
}

