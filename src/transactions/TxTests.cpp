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

TransactionFramePtr setTrust(SecretKey& from, SecretKey& to, uint32_t seq, const std::string& currencyCode)
{
    TransactionEnvelope txEnvelope;
    txEnvelope.tx.body.type(CHANGE_TRUST);
    txEnvelope.tx.account = from.getPublicKey();
    txEnvelope.tx.maxFee = 12;
    txEnvelope.tx.maxLedger = 1000;
    txEnvelope.tx.minLedger = 0;
    txEnvelope.tx.seqNum = seq;
    txEnvelope.tx.body.changeTrustTx().limit = 1000000;
    txEnvelope.tx.body.changeTrustTx().line.type(ISO4217);
    strToCurrencyCode(txEnvelope.tx.body.changeTrustTx().line.isoCI().currencyCode,currencyCode);
    txEnvelope.tx.body.changeTrustTx().line.isoCI().issuer = to.getPublicKey();

    TransactionFramePtr res = TransactionFrame::makeTransactionFromWire(txEnvelope);

    res->addSignature(from);
    return res;
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
    txEnvelope.tx.body.paymentTx().currency.type(NATIVE);

    TransactionFramePtr res = TransactionFrame::makeTransactionFromWire(txEnvelope);

    res->addSignature(from);

    return res;
}

TransactionFramePtr createCreditPaymentTx(SecretKey& from, SecretKey& to, Currency& ci,uint32_t seq, uint64_t amount)
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
    txEnvelope.tx.body.paymentTx().sendMax = amount + 1000;

    TransactionFramePtr res = TransactionFrame::makeTransactionFromWire(txEnvelope);

    res->addSignature(from);

    return res;
}
}
}

