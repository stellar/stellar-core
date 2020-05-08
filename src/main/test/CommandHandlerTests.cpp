// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/CommandHandler.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/Decoder.h"
#include "xdrpp/marshal.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("transaction envelope bridge", "[commandhandler]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto& ch = app->getCommandHandler();
    auto baseFee = app->getLedgerManager().getLastTxFee();

    std::string const PENDING_RESULT = "{\"status\": \"PENDING\"}";
    auto notSupportedResult = [](int64_t fee) {
        TransactionResult txRes;
        txRes.feeCharged = fee;
        txRes.result.code(txNOT_SUPPORTED);
        auto inner = decoder::encode_b64(xdr::xdr_to_opaque(txRes));
        return "{\"status\": \"ERROR\" , \"error\": \"" + inner + "\"}";
    };

    auto sign = [&](auto& signatures, SecretKey const& key, auto... input) {
        auto hash = sha256(xdr::xdr_to_opaque(app->getNetworkID(), input...));
        signatures.emplace_back(SignatureUtils::sign(key, hash));
    };

    auto submit = [&](auto... input) {
        std::string ret;
        auto opaque = decoder::encode_b64(xdr::xdr_to_opaque(input...));
        ch.tx("?blob=" + opaque, ret);
        return ret;
    };

    SECTION("old-style transaction")
    {
        for_all_versions(*app, [&]() {
            closeLedgerOn(*app, 2, 1, 1, 2017);

            auto root = TestAccount::createRoot(*app);

            Transaction tx;
            tx.sourceAccount = toMuxedAccount(root);
            tx.fee = baseFee;
            tx.seqNum = root.nextSequenceNumber();
            tx.operations.emplace_back(payment(root, 1));

            xdr::xvector<DecoratedSignature, 20> signatures;
            sign(signatures, root, ENVELOPE_TYPE_TX, tx);
            REQUIRE(submit(tx, signatures) == PENDING_RESULT);
        });
    }

    SECTION("new-style transaction v0")
    {
        for_all_versions(*app, [&]() {
            closeLedgerOn(*app, 2, 1, 1, 2017);

            auto root = TestAccount::createRoot(*app);

            TransactionEnvelope env(ENVELOPE_TYPE_TX_V0);
            auto& tx = env.v0().tx;
            tx.sourceAccountEd25519 = root.getPublicKey().ed25519();
            tx.fee = baseFee;
            tx.seqNum = root.nextSequenceNumber();
            tx.operations.emplace_back(payment(root, 1));

            sign(env.v0().signatures, root, ENVELOPE_TYPE_TX, 0, tx);
            REQUIRE(submit(env) == PENDING_RESULT);
        });
    }

    auto createV1 = [&]() {
        auto root = TestAccount::createRoot(*app);

        TransactionEnvelope env(ENVELOPE_TYPE_TX);
        auto& tx = env.v1().tx;
        tx.sourceAccount = toMuxedAccount(root);
        tx.fee = baseFee;
        tx.seqNum = root.nextSequenceNumber();
        tx.operations.emplace_back(payment(root, 1));

        sign(env.v1().signatures, root, ENVELOPE_TYPE_TX, tx);
        return env;
    };

    SECTION("new-style transaction v1")
    {
        for_versions_to(12, *app, [&]() {
            closeLedgerOn(*app, 2, 1, 1, 2017);
            REQUIRE(submit(createV1()) == notSupportedResult(baseFee));
        });

        for_versions_from(13, *app, [&]() {
            closeLedgerOn(*app, 2, 1, 1, 2017);
            REQUIRE(submit(createV1()) == PENDING_RESULT);
        });
    }

    SECTION("fee-bump")
    {
        auto createFeeBump = [&]() {
            auto root = TestAccount::createRoot(*app);

            TransactionEnvelope env(ENVELOPE_TYPE_TX_FEE_BUMP);
            auto& fb = env.feeBump().tx;
            fb.feeSource = toMuxedAccount(root.getPublicKey());
            fb.fee = 2 * baseFee;
            fb.innerTx.type(ENVELOPE_TYPE_TX);
            fb.innerTx.v1() = createV1().v1();

            sign(env.feeBump().signatures, root, ENVELOPE_TYPE_TX_FEE_BUMP, fb);
            return env;
        };

        for_versions_to(12, *app, [&]() {
            closeLedgerOn(*app, 2, 1, 1, 2017);
            REQUIRE(submit(createFeeBump()) == notSupportedResult(2 * baseFee));
        });

        for_versions_from(13, *app, [&]() {
            closeLedgerOn(*app, 2, 1, 1, 2017);
            REQUIRE(submit(createFeeBump()) == PENDING_RESULT);
        });
    }
}
