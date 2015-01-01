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

// STR Payment 
// Credit Payment
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

    VirtualClock clock;
    Application app(clock, cfg);
    app.start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");

    TransactionEnvelope txEnvelope;
    txEnvelope.tx.body.type(PAYMENT);
    txEnvelope.tx.account = root.getPublicKey();
    txEnvelope.tx.maxFee = 12;
    txEnvelope.tx.maxLedger = 1000;
    txEnvelope.tx.minLedger = 0;
    txEnvelope.tx.seqNum = 1;
    txEnvelope.tx.body.paymentTx().amount = 1000;
    txEnvelope.tx.body.paymentTx().destination = a1.getPublicKey();
    txEnvelope.tx.body.paymentTx().sendMax = 2000;
    txEnvelope.tx.body.paymentTx().currency.native(true);
    
    TransactionFramePtr paymentTx = TransactionFrame::makeTransactionFromWire(txEnvelope);

    TxDelta delta;
    paymentTx->apply(delta,app.getLedgerMaster());

    Json::Value jsonResult;
    LedgerDelta ledgerDelta;

    delta.commitDelta(jsonResult,ledgerDelta, app.getLedgerMaster());
    LOG(INFO) << jsonResult.toStyledString();

    LOG(INFO) << "************ Ending payment test";
}




