// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/test/LedgerTestUtils.h"
#include "main/Application.h"
#include "main/CommandHandler.h"
#include "test/Catch2.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionUtils.h"
#include "util/Decoder.h"
#include "util/Math.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"
#include "xdrpp/marshal.h"
#include <fmt/format.h>
#include <optional>
#include <stdexcept>

using namespace stellar;
using namespace stellar::txbridge;
using namespace stellar::txtest;

TEST_CASE_VERSIONS("transaction envelope bridge", "[commandhandler]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto& ch = app->getCommandHandler();
    auto baseFee = app->getLedgerManager().getLastTxFee();

    std::string const PENDING_RESULT = "{\"status\":\"PENDING\"}\n";
    auto errorResult = [](TransactionResultCode resultCode, int64_t fee) {
        TransactionResult txRes;
        txRes.feeCharged = fee;
        txRes.result.code(resultCode);
        auto inner = decoder::encode_b64(xdr::xdr_to_opaque(txRes));
        return

            std::string("{\"error\":\"") + inner + "\",\"status\":\"ERROR\"}\n";
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

            auto root = app->getRoot();

            Transaction tx;
            tx.sourceAccount = toMuxedAccount(*root);
            tx.fee = baseFee;
            tx.seqNum = root->nextSequenceNumber();
            tx.operations.emplace_back(payment(*root, 1));

            xdr::xvector<DecoratedSignature, 20> signatures;
            sign(signatures, *root, ENVELOPE_TYPE_TX, tx);
            REQUIRE(submit(tx, signatures) == PENDING_RESULT);
        });
    }

    SECTION("new-style transaction v0")
    {
        auto timeBoundsTest = [&](xdr::pointer<TimeBounds> timeBounds,
                                  std::string const& res) {
            for_all_versions(*app, [&]() {
                closeLedgerOn(*app, 2, 1, 1, 2017);

                auto root = app->getRoot();

                TransactionEnvelope env(ENVELOPE_TYPE_TX_V0);
                auto& tx = env.v0().tx;
                tx.sourceAccountEd25519 = root->getPublicKey().ed25519();
                tx.fee = baseFee;
                tx.seqNum = root->nextSequenceNumber();
                tx.operations.emplace_back(payment(*root, 1));
                tx.timeBounds = timeBounds;

                sign(env.v0().signatures, *root, ENVELOPE_TYPE_TX, 0, tx);

                REQUIRE(submit(env) == res);
            });
        };

        SECTION("valid without timebounds")
        {
            xdr::pointer<TimeBounds> timeBounds;
            timeBoundsTest(timeBounds, PENDING_RESULT);
        }

        SECTION("valid with timebounds and on time")
        {
            xdr::pointer<TimeBounds> timeBounds;
            timeBounds.activate().minTime = getTestDate(31, 12, 2016);
            timeBounds.activate().maxTime = getTestDate(2, 1, 2017);
            timeBoundsTest(timeBounds, PENDING_RESULT);
        }

        SECTION("invalid with timebounds and too early")
        {
            xdr::pointer<TimeBounds> timeBounds;
            timeBounds.activate().minTime = getTestDate(2, 1, 2017);
            timeBounds.activate().maxTime = getTestDate(3, 1, 2017);
            timeBoundsTest(timeBounds, errorResult(txTOO_EARLY, baseFee));
        }

        SECTION("invalid with timebounds and too late")
        {
            xdr::pointer<TimeBounds> timeBounds;
            timeBounds.activate().minTime = getTestDate(30, 12, 2016);
            timeBounds.activate().maxTime = getTestDate(31, 12, 2016);
            timeBoundsTest(timeBounds, errorResult(txTOO_LATE, baseFee));
        }
    }

    auto createV1 = [&]() {
        auto root = app->getRoot();

        TransactionEnvelope env(ENVELOPE_TYPE_TX);
        auto& tx = env.v1().tx;
        tx.sourceAccount = toMuxedAccount(*root);
        tx.fee = baseFee;
        tx.seqNum = root->nextSequenceNumber();
        tx.operations.emplace_back(payment(*root, 1));

        sign(env.v1().signatures, *root, ENVELOPE_TYPE_TX, tx);
        return env;
    };

    SECTION("new-style transaction v1")
    {
        for_versions_to(12, *app, [&]() {
            closeLedgerOn(*app, 2, 1, 1, 2017);
            REQUIRE(submit(createV1()) ==
                    errorResult(txNOT_SUPPORTED, baseFee));
        });

        for_versions_from(13, *app, [&]() {
            closeLedgerOn(*app, 2, 1, 1, 2017);
            REQUIRE(submit(createV1()) == PENDING_RESULT);
        });
    }

    SECTION("fee-bump")
    {
        auto createFeeBump = [&]() {
            auto root = app->getRoot();

            TransactionEnvelope env(ENVELOPE_TYPE_TX_FEE_BUMP);
            auto& fb = env.feeBump().tx;
            fb.feeSource = toMuxedAccount(root->getPublicKey());
            fb.fee = 2 * baseFee;
            fb.innerTx.type(ENVELOPE_TYPE_TX);
            fb.innerTx.v1() = createV1().v1();

            sign(env.feeBump().signatures, *root, ENVELOPE_TYPE_TX_FEE_BUMP,
                 fb);
            return env;
        };

        for_versions_to(12, *app, [&]() {
            closeLedgerOn(*app, 2, 1, 1, 2017);
            REQUIRE(submit(createFeeBump()) ==
                    errorResult(txNOT_SUPPORTED, 2 * baseFee));
        });

        for_versions_from(13, *app, [&]() {
            closeLedgerOn(*app, 2, 1, 1, 2017);
            REQUIRE(submit(createFeeBump()) == PENDING_RESULT);
        });
    }
}

TEST_CASE("endpoints reject requests before setReady", "[commandhandler]")
{
    // Use persistent DB so the second app instance can open the same database
    // without calling newDB.
    Config cfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));

    // First app: create the genesis ledger so a valid DB exists on disk.
    {
        VirtualClock clock;
        auto app = createTestApplication(clock, cfg);
    }

    // Second app: open the existing DB but don't start, so
    // loadLastKnownLedger and setReady have not been called.
    VirtualClock clock;
    auto app = createTestApplication(clock, cfg, false, false);
    auto& ch = app->getCommandHandler();

    // Minimal valid param for /tx endpoint
    TransactionEnvelope env(ENVELOPE_TYPE_TX);
    auto blob = decoder::encode_b64(xdr::xdr_to_opaque(env));

    // Make sure we gracefully handle endpoints being called before setReady is
    // called.
    auto result = ch.manualCmd("tx?blob=" + blob);
    REQUIRE(result.find("Core is booting") != std::string::npos);
}

TEST_CASE("toggleoverlayonlymode", "[commandhandler]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto& ch = app->getCommandHandler();
    bool initialMode = app->getRunInOverlayOnlyMode();

    for (int i = 0; i < 5; ++i)
    {
        std::string retStr;
        ch.toggleOverlayOnlyMode("", retStr);

        bool expectedMode = !initialMode;
        if (i % 2 == 1)
        {
            expectedMode = initialMode;
        }

        REQUIRE(app->getRunInOverlayOnlyMode() == expectedMode);

        Json::Value root;
        Json::Reader reader;
        REQUIRE(reader.parse(retStr, root));
        REQUIRE(root["overlay_only_mode"].asBool() == expectedMode);
    }
}

TEST_CASE("tx force flag bypasses banned account filter", "[commandhandler]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.FILTERED_G_ADDRESSES = {};
    auto app = createTestApplication(clock, cfg);
    auto& ch = app->getCommandHandler();

    closeLedgerOn(*app, 2, 1, 1, 2017);

    auto root = app->getRoot();
    auto srcKey = SecretKey::pseudoRandomForTesting();
    auto src = root->create(srcKey, 1000000000);

    // Ban the source account
    auto addr = KeyUtils::toStrKey(srcKey.getPublicKey());
    ch.manualCmd("banaccounts?accountids=" + addr);

    // Build a valid transaction from the banned account
    auto acc = getAccount("forceTestAcc");
    auto tx = src.tx({createAccount(acc.getPublicKey(), 1)});
    auto blob = decoder::encode_b64(xdr::xdr_to_opaque(tx->getEnvelope()));

    SECTION("without force flag, tx is filtered")
    {
        std::string ret;
        ch.tx("?blob=" + blob, ret);
        REQUIRE(ret.find("FILTERED") != std::string::npos);
    }

    SECTION("with force=true, tx bypasses account ban")
    {
        std::string ret;
        ch.tx("?blob=" + blob + "&force=true", ret);
        REQUIRE(ret.find("PENDING") != std::string::npos);
    }
}
