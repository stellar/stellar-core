// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "ledger/test/LedgerTestUtils.h"
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
#include "util/Math.h"
#include "util/optional.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"
#include "xdrpp/marshal.h"
#include <fmt/format.h>
#include <stdexcept>

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

TEST_CASE("manualclose", "[commandhandler]")
{
    auto testManualCloseConfig = [](auto configure, auto issue) {
        VirtualClock clock(VirtualClock::VIRTUAL_TIME);
        Config cfg = getTestConfig();
        configure(cfg);
        auto app = createTestApplication(clock, cfg);
        app->start();
        auto& commandHandler = app->getCommandHandler();
        std::string retStr;
        issue(commandHandler, retStr);
    };

    SECTION("'manualclose' is forbidden if MANUAL_CLOSE is not configured")
    {
        testManualCloseConfig(
            [](Config& cfg) {
                cfg.MANUAL_CLOSE = false;
                cfg.RUN_STANDALONE = false;
            },
            [](CommandHandler& handler, std::string& retStr) {
                REQUIRE_THROWS_AS(handler.manualClose("", retStr),
                                  std::invalid_argument);
            });
        testManualCloseConfig(
            [](Config& cfg) {
                cfg.MANUAL_CLOSE = false;
                cfg.RUN_STANDALONE = true;
            },
            [](CommandHandler& handler, std::string& retStr) {
                REQUIRE_THROWS_AS(handler.manualClose("", retStr),
                                  std::invalid_argument);
            });
    }

    SECTION("'manualclose' command without parameters is allowed if "
            "MANUAL_CLOSE is configured")
    {
        testManualCloseConfig(
            [](Config& cfg) {
                cfg.MANUAL_CLOSE = true;
                cfg.RUN_STANDALONE = false;
            },
            [](CommandHandler& handler, std::string& retStr) {
                handler.manualClose("", retStr);
            });
    }

    SECTION("'manualclose' command with parameters is forbidden if "
            "RUN_STANDALONE is not configured")
    {
        testManualCloseConfig(
            [](Config& cfg) {
                cfg.MANUAL_CLOSE = true;
                cfg.RUN_STANDALONE = false;
            },
            [](CommandHandler& handler, std::string& retStr) {
                CHECK_THROWS_AS(handler.manualClose("ledgerSeq=10", retStr),
                                std::invalid_argument);
                REQUIRE_THROWS_AS(handler.manualClose("closeTime=10", retStr),
                                  std::invalid_argument);
            });
    }

    SECTION("'manualclose' command with parameters is allowed if "
            "RUN_STANDALONE is configured")
    {
        testManualCloseConfig(
            [](Config& cfg) {
                cfg.MANUAL_CLOSE = true;
                cfg.RUN_STANDALONE = true;
            },
            [](CommandHandler& handler, std::string& retStr) {
                handler.manualClose("ledgerSeq=20&closeTime=20", retStr);
            });
    }

    VirtualClock clock(VirtualClock::VIRTUAL_TIME);
    auto app = createTestApplication(clock, getTestConfig());
    auto& commandHandler = app->getCommandHandler();
    app->start();

    REQUIRE(app->getConfig().MANUAL_CLOSE);
    REQUIRE(app->getConfig().NODE_IS_VALIDATOR);
    REQUIRE(app->getConfig().RUN_STANDALONE);
    REQUIRE(app->getConfig().FORCE_SCP);

    auto const defaultManualCloseTimeInterval =
        app->getConfig().getExpectedLedgerCloseTime().count();

    auto lastLedgerNum = [&]() {
        return app->getLedgerManager().getLastClosedLedgerNum();
    };

    auto lastCloseTime = [&]() {
        return app->getLedgerManager()
            .getLastClosedLedgerHeader()
            .header.scpValue.closeTime;
    };

    auto submitClose = [&](optional<uint32_t> const& ledgerSeq,
                           optional<TimePoint> const& closeTime,
                           std::string& retStr) {
        std::string ledgerSeqParam, closeTimeParam;
        if (ledgerSeq)
        {
            std::stringstream stream;
            stream << *ledgerSeq;
            ledgerSeqParam =
                fmt::format(FMT_STRING("?ledgerSeq={}"), stream.str());
        }
        if (closeTime)
        {
            std::stringstream stream;
            stream << *closeTime;
            closeTimeParam =
                fmt::format(FMT_STRING("?closeTime={}"), stream.str());
        }
        std::string const params = ledgerSeqParam + closeTimeParam;
        commandHandler.manualClose(params, retStr);
    };

    auto const noLedgerSeq = nullopt<uint32_t>();
    auto const noCloseTime = nullopt<TimePoint>();

    SECTION("manual close with no parameters increments sequence number and "
            "increases close time")
    {
        std::string retStr;
        auto const expectedNewLedgerSeq = lastLedgerNum() + 1;
        auto const minimumExpectedNewCloseTime = lastCloseTime() + 1;
        submitClose(noLedgerSeq, noCloseTime, retStr);
        REQUIRE(lastLedgerNum() == expectedNewLedgerSeq);
        REQUIRE(lastCloseTime() >= minimumExpectedNewCloseTime);
    }

    SECTION("manual close with explicit sequence number parameter sets new "
            "sequence number and increases close time")
    {
        std::vector<uint32_t> const seqNumIncrements{1, 2, 3, 10, 100, 1000000};
        for (auto const& seqNumIncrement : seqNumIncrements)
        {
            std::string retStr;
            auto const curLedgerSeq = lastLedgerNum();
            auto const expectedNewLedgerSeq = curLedgerSeq + seqNumIncrement;
            auto const minimumExpectedNewCloseTime = lastCloseTime() + 1;
            submitClose(make_optional<uint32_t>(expectedNewLedgerSeq),
                        noCloseTime, retStr);
            CAPTURE(curLedgerSeq);
            CAPTURE(expectedNewLedgerSeq);
            CAPTURE(retStr);
            REQUIRE(lastLedgerNum() == expectedNewLedgerSeq);
            REQUIRE(lastCloseTime() >= minimumExpectedNewCloseTime);
        }
    }

    SECTION("manual close with explicit close time parameter sets new close "
            "time and current virtual time, and increments sequence number")
    {
        std::vector<TimePoint> const closeTimeIncrements{1,  2,   3,
                                                         10, 100, 1000000};
        for (auto const& closeTimeIncrement : closeTimeIncrements)
        {
            std::string retStr;
            auto const curLedgerSeq = lastLedgerNum();
            auto const curLedgerCloseTime = lastCloseTime();
            auto const expectedNewCloseTime =
                curLedgerCloseTime + closeTimeIncrement;
            submitClose(noLedgerSeq,
                        make_optional<TimePoint>(expectedNewCloseTime), retStr);
            CAPTURE(curLedgerCloseTime);
            CAPTURE(expectedNewCloseTime);
            CAPTURE(retStr);
            REQUIRE(lastLedgerNum() == curLedgerSeq + 1);
            REQUIRE(lastCloseTime() == expectedNewCloseTime);
            REQUIRE(lastCloseTime() ==
                    static_cast<TimePoint>(
                        VirtualClock::to_time_t(app->getClock().system_now())));
        }
    }

    SECTION("manual close sequence number parameter that just barely does not "
            "overflow int32_t is accepted")
    {
        std::string retStr;
        REQUIRE(lastLedgerNum() == LedgerManager::GENESIS_LEDGER_SEQ);
        auto maxLedgerNum =
            static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
        submitClose(make_optional<uint32_t>(maxLedgerNum), noCloseTime, retStr);
        CAPTURE(retStr);
        REQUIRE(lastLedgerNum() == maxLedgerNum);
    }

    SECTION("manual close sequence number parameter that overflows int32_t is "
            "detected and rejected")
    {
        std::string retStr;
        REQUIRE_THROWS_AS(
            submitClose(
                make_optional<uint32_t>(
                    static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) +
                    1),
                noCloseTime, retStr),
            std::invalid_argument);
    }

    SECTION("manual close sequence number parameter that would not increase "
            "ledger sequence number is detected and rejected")
    {
        uint32_t initialSequenceNumber = LedgerManager::GENESIS_LEDGER_SEQ + 2;
        std::string retStr;

        submitClose(make_optional<uint32_t>(initialSequenceNumber), noCloseTime,
                    retStr);

        CHECK_THROWS_AS(
            submitClose(make_optional<uint32_t>(initialSequenceNumber - 1),
                        noCloseTime, retStr),
            std::invalid_argument);

        REQUIRE_THROWS_AS(
            submitClose(make_optional<uint32_t>(initialSequenceNumber),
                        noCloseTime, retStr),
            std::invalid_argument);
    }

    uint64_t const firstSecondOfYear2200GMT = 7'258'118'400;

    SECTION(
        "manual close time that does not overflow documented limit is accepted")
    {
        std::string retStr;
        REQUIRE(lastLedgerNum() == LedgerManager::GENESIS_LEDGER_SEQ);
        submitClose(noLedgerSeq,
                    make_optional<TimePoint>(firstSecondOfYear2200GMT), retStr);
        CAPTURE(retStr);
        REQUIRE(lastCloseTime() == firstSecondOfYear2200GMT);
    }

    SECTION("manual close time that overflows documented limit is detected and "
            "rejected")
    {
        uint64_t const overflowingCloseTime = firstSecondOfYear2200GMT + 1ULL;
        std::string retStr;
        REQUIRE(lastLedgerNum() == LedgerManager::GENESIS_LEDGER_SEQ);
        CAPTURE(retStr);
        REQUIRE_THROWS_AS(
            submitClose(noLedgerSeq,
                        make_optional<TimePoint>(overflowingCloseTime), retStr),
            std::invalid_argument);
    }

    SECTION("manual close induces expected transaction applications")
    {
        auto submitTx = [&](TransactionEnvelope const& envelope,
                            std::string& retStr) {
            commandHandler.tx(
                fmt::format(FMT_STRING("?blob={}"),
                            decoder::encode_b64(xdr::xdr_to_opaque(envelope))),
                retStr);
        };

        auto const initialCloseTime = 100;
        std::string retStr;

        submitClose(noLedgerSeq, make_optional<TimePoint>(initialCloseTime),
                    retStr);
        REQUIRE(lastLedgerNum() == LedgerManager::GENESIS_LEDGER_SEQ + 1);
        REQUIRE(lastCloseTime() == initialCloseTime);
        REQUIRE(VirtualClock::to_time_t(app->getClock().system_now()) ==
                initialCloseTime);
        auto root = TestAccount::createRoot(*app);

        SECTION("A single transaction is applied by a manual close")
        {
            LedgerEntry dle;
            dle.lastModifiedLedgerSeq = lastLedgerNum();
            dle.data.type(DATA);
            auto de = LedgerTestUtils::generateValidDataEntry();
            de.accountID = root.getPublicKey();
            dle.data.data() = de;

            auto dataOp = txtest::manageData(de.dataName, &de.dataValue);
            auto txFrame = root.tx({dataOp});
            submitTx(txFrame->getEnvelope(), retStr);

            {
                LedgerTxn lookupTx(app->getLedgerTxnRoot());
                auto entry = lookupTx.load(LedgerEntryKey(dle));
                REQUIRE(!entry);
            }

            submitClose(noLedgerSeq, noCloseTime, retStr);

            LedgerTxn lookupTx(app->getLedgerTxnRoot());
            auto entry = lookupTx.load(LedgerEntryKey(dle));
            REQUIRE(entry);
        }

        SECTION("A transaction which is submitted on time can become late by "
                "the time of a ledger close")
        {
            LedgerEntry dle;
            dle.lastModifiedLedgerSeq = lastLedgerNum();
            dle.data.type(DATA);
            auto de = LedgerTestUtils::generateValidDataEntry();
            de.accountID = root.getPublicKey();
            dle.data.data() = de;

            auto dataOp = txtest::manageData(de.dataName, &de.dataValue);
            auto txFrame = root.tx({dataOp});
            REQUIRE(txFrame->getEnvelope().type() == stellar::ENVELOPE_TYPE_TX);
            txFrame->getEnvelope().v1().tx.timeBounds.activate();
            txFrame->getEnvelope().v1().tx.timeBounds->minTime = 0;
            TimePoint const maxTime =
                lastCloseTime() + defaultManualCloseTimeInterval +
                getUpperBoundCloseTimeOffset(*app, lastCloseTime());
            txFrame->getEnvelope().v1().tx.timeBounds->maxTime = maxTime;
            txFrame->getEnvelope().v1().signatures.clear();
            txFrame->addSignature(root);

            {
                LedgerTxn checkLtx(app->getLedgerTxnRoot());
                auto valid = txFrame->checkValid(checkLtx, 0, 0, 0);
                REQUIRE(valid);
            }

            submitTx(txFrame->getEnvelope(), retStr);

            {
                LedgerTxn lookupTx(app->getLedgerTxnRoot());
                auto entry = lookupTx.load(LedgerEntryKey(dle));
                REQUIRE(!entry);
            }

            SECTION("Ledger closes on time; transaction is applied")
            {
                submitClose(noLedgerSeq, make_optional<TimePoint>(maxTime),
                            retStr);

                LedgerTxn lookupTx(app->getLedgerTxnRoot());
                auto entry = lookupTx.load(LedgerEntryKey(dle));
                REQUIRE(entry);
            }

            SECTION("Ledger closes too late; transaction is not applied")
            {
                submitClose(noLedgerSeq, make_optional<TimePoint>(maxTime + 1),
                            retStr);

                LedgerTxn lookupTx(app->getLedgerTxnRoot());
                auto entry = lookupTx.load(LedgerEntryKey(dle));
                REQUIRE(!entry);
            }
        }
    }
}
