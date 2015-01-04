// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/Sign.h"
#include "crypto/Base58.h"
#include "lib/json/json.h"

using namespace stellar;

typedef std::unique_ptr<Application> appPtr;

SecretKey getRoot()
{
    ByteSlice bytes("masterpassphrasemasterpassphrase");
    std::string b58SeedStr = toBase58Check(VER_SEED, bytes);
    return SecretKey::fromBase58Seed(b58SeedStr);
}

SecretKey getAccount(const char* n)
{
    // stretch name to 32 bytes
    std::string name(n);
    while(name.size() < 32) name += '.';
    ByteSlice bytes(name);
    std::string b58SeedStr = toBase58Check(VER_SEED, bytes);
    return SecretKey::fromBase58Seed(b58SeedStr);
}

TransactionFramePtr setTrust(SecretKey& from, SecretKey& to, uint32_t seq, uint256& currencyCode)
{
    TransactionEnvelope txEnvelope;
    txEnvelope.tx.body.type(CHANGE_TRUST);
    txEnvelope.tx.account = from.getPublicKey();
    txEnvelope.tx.maxFee = 12;
    txEnvelope.tx.maxLedger = 1000;
    txEnvelope.tx.minLedger = 0;
    txEnvelope.tx.seqNum = seq;
    txEnvelope.tx.body.changeTrustTx().limit = 1000000;
    txEnvelope.tx.body.changeTrustTx().line.currencyCode = currencyCode;
    txEnvelope.tx.body.changeTrustTx().line.issuer = to.getPublicKey();

    return TransactionFrame::makeTransactionFromWire(txEnvelope);
}

TransactionFramePtr createPaymentTx(SecretKey& from, SecretKey& to, uint32_t seq, uint64_t amount)
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
    txEnvelope.tx.body.paymentTx().sendMax = amount+1000;
    txEnvelope.tx.body.paymentTx().currency.native(true);

    return TransactionFrame::makeTransactionFromWire(txEnvelope);
}

TransactionFramePtr createCreditPaymentTx(SecretKey& from, SecretKey& to, CurrencyIssuer& ci,uint32_t seq, uint64_t amount)
{
    TransactionEnvelope txEnvelope;
    txEnvelope.tx.body.type(PAYMENT);
    txEnvelope.tx.account = from.getPublicKey();
    txEnvelope.tx.maxFee = 12;
    txEnvelope.tx.maxLedger = 1000;
    txEnvelope.tx.minLedger = 0;
    txEnvelope.tx.seqNum = seq;
    txEnvelope.tx.body.paymentTx().amount = amount;
    txEnvelope.tx.body.paymentTx().currency.native(false);
    txEnvelope.tx.body.paymentTx().currency.ci() = ci;
    txEnvelope.tx.body.paymentTx().destination = to.getPublicKey();
    txEnvelope.tx.body.paymentTx().sendMax = amount + 1000;

    return TransactionFrame::makeTransactionFromWire(txEnvelope);
}



// Offer that doesn't cross
// Offer that crosses exactly
// Offer that takes multiple other offers and is cleared
// Offer that takes multiple other offers and remains
// Offer selling STR
// Offer buying STR
// Offer with transfer rate
// Offer for more than you have
// Offer for something you can't hold

TEST_CASE("create offer", "[tx]")
{
    Config cfg;
    cfg.RUN_STANDALONE = true;
    cfg.START_NEW_NETWORK = true;

    VirtualClock clock;
    Application app(clock, cfg);

    // set up world

}

// *STR Payment 
// *Credit Payment
// STR -> Credit Payment
// Credit -> STR Payment
// Credit -> STR -> Credit Payment
// Credit -> Credit -> Credit -> Credit Payment
// path payment where there isn't enough in the path
// path payment with a transfer rate
TEST_CASE("payment", "[tx]")
{
    LOG(INFO) << "************ Starting payment test";

    Config cfg;
    cfg.RUN_STANDALONE = true;
    cfg.START_NEW_NETWORK = true;
    cfg.DESIRED_BASE_FEE = 10;
    //cfg.DATABASE = "postgresql://dbmaster:-island-@localhost/hayashi";

    VirtualClock clock;
    Application app(clock, cfg);
    app.start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");

    TransactionFramePtr txFrame = createPaymentTx(root, a1, 1, 1000);

    { // simple payment
        TxDelta delta;
        txFrame->apply(delta, app.getLedgerMaster());

        Json::Value jsonResult;
        LedgerDelta ledgerDelta;

        delta.commitDelta(jsonResult, ledgerDelta, app.getLedgerMaster());

        LOG(INFO) << jsonResult.toStyledString();

        REQUIRE(txFrame->getResultCode() == txSUCCESS);
        AccountFrame a1Account, rootAccount;
        REQUIRE(app.getDatabase().loadAccount(root.getPublicKey(), rootAccount));
        REQUIRE(app.getDatabase().loadAccount(a1.getPublicKey(), a1Account));
        REQUIRE(a1Account.getBalance() == 1000);
        REQUIRE(rootAccount.getBalance() == (100000000000000 - 1000 - 10));
    }
    { // make sure 2nd time fails
        TxDelta delta;
        txFrame->apply(delta, app.getLedgerMaster());

        Json::Value jsonResult;
        LedgerDelta ledgerDelta;

        delta.commitDelta(jsonResult, ledgerDelta, app.getLedgerMaster());

        LOG(INFO) << jsonResult.toStyledString();

        REQUIRE(txFrame->getResultCode() == txBADSEQ);
    }

    { // credit payment with no trust
        CurrencyIssuer ci;
        ci.issuer = root.getPublicKey();
        ci.currencyCode= root.getPublicKey();

        txFrame = createCreditPaymentTx(root, a1, ci, 2, 100);
        TxDelta delta;
        txFrame->apply(delta, app.getLedgerMaster());

        Json::Value jsonResult;
        LedgerDelta ledgerDelta;

        delta.commitDelta(jsonResult, ledgerDelta, app.getLedgerMaster());

        LOG(INFO) << jsonResult.toStyledString();

        REQUIRE(txFrame->getResultCode() == txNOTRUST);
    }

    {
        txFrame = setTrust(a1, root, 1, root.getPublicKey());
        TxDelta delta;
        txFrame->apply(delta, app.getLedgerMaster());

        Json::Value jsonResult;
        LedgerDelta ledgerDelta;

        delta.commitDelta(jsonResult, ledgerDelta, app.getLedgerMaster());

        LOG(INFO) << jsonResult.toStyledString();

        REQUIRE(txFrame->getResultCode() == txSUCCESS);
    }

    { // simple credit payment
        CurrencyIssuer ci;
        ci.issuer = root.getPublicKey();
        ci.currencyCode = root.getPublicKey();

        txFrame = createCreditPaymentTx(root, a1, ci, 3, 100);
        TxDelta delta;
        txFrame->apply(delta, app.getLedgerMaster());

        Json::Value jsonResult;
        LedgerDelta ledgerDelta;

        delta.commitDelta(jsonResult, ledgerDelta, app.getLedgerMaster());

        LOG(INFO) << jsonResult.toStyledString();

        REQUIRE(txFrame->getResultCode() == txSUCCESS);
    }

    LOG(INFO) << "************ Ending payment test";
}




