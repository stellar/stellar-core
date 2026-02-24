// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "herder/Herder.h"
#include "herder/TransactionQueue.h"
#include "main/Application.h"
#include "main/BannedAccountsPersistor.h"
#include "main/CommandHandler.h"
#include "test/Catch2.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("BannedAccountsPersistor basic operations", "[banaccounts]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.FILTERED_G_ADDRESSES = {};
    Application::pointer app = createTestApplication(clock, cfg);

    auto& persistor = app->getBannedAccountsPersistor();

    auto key1 = SecretKey::pseudoRandomForTesting();
    auto key2 = SecretKey::pseudoRandomForTesting();
    auto key3 = SecretKey::pseudoRandomForTesting();
    auto addr1 = KeyUtils::toStrKey(key1.getPublicKey());
    auto addr2 = KeyUtils::toStrKey(key2.getPublicKey());
    auto addr3 = KeyUtils::toStrKey(key3.getPublicKey());

    SECTION("initially empty")
    {
        REQUIRE(persistor.getBannedAccounts().empty());
        REQUIRE(persistor.getBannedAccountStrKeys().empty());
    }

    SECTION("add accounts")
    {
        persistor.addBannedAccounts({addr1, addr2});
        REQUIRE(persistor.getBannedAccounts().size() == 2);

        auto keys = persistor.getBannedAccountStrKeys();
        REQUIRE(keys.size() == 2);
        // Keys are sorted
        REQUIRE(std::is_sorted(keys.begin(), keys.end()));
        REQUIRE(std::find(keys.begin(), keys.end(), addr1) != keys.end());
        REQUIRE(std::find(keys.begin(), keys.end(), addr2) != keys.end());
    }

    SECTION("add is idempotent")
    {
        persistor.addBannedAccounts({addr1});
        persistor.addBannedAccounts({addr1, addr2});
        REQUIRE(persistor.getBannedAccounts().size() == 2);
    }

    SECTION("remove accounts")
    {
        persistor.addBannedAccounts({addr1, addr2, addr3});
        REQUIRE(persistor.getBannedAccounts().size() == 3);

        persistor.removeBannedAccounts({addr2});
        REQUIRE(persistor.getBannedAccounts().size() == 2);

        auto keys = persistor.getBannedAccountStrKeys();
        REQUIRE(std::find(keys.begin(), keys.end(), addr2) == keys.end());
        REQUIRE(std::find(keys.begin(), keys.end(), addr1) != keys.end());
        REQUIRE(std::find(keys.begin(), keys.end(), addr3) != keys.end());
    }
    SECTION("remove non-existent account is no-op")
    {
        persistor.addBannedAccounts({addr1, addr2});
        REQUIRE(persistor.getBannedAccounts().size() == 2);

        persistor.removeBannedAccounts({addr3});
        REQUIRE(persistor.getBannedAccounts().size() == 2);
    }

    SECTION("clear all")
    {
        persistor.addBannedAccounts({addr1, addr2});
        REQUIRE(persistor.getBannedAccounts().size() == 2);

        persistor.clearBannedAccounts();
        REQUIRE(persistor.getBannedAccounts().empty());
    }
}

TEST_CASE("FILTERED_G_ADDRESSES migration", "[banaccounts]")
{
    SECTION("addresses migrated from config on startup")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        auto key1 = SecretKey::pseudoRandomForTesting();
        auto key2 = SecretKey::pseudoRandomForTesting();
        auto addr1 = KeyUtils::toStrKey(key1.getPublicKey());
        auto addr2 = KeyUtils::toStrKey(key2.getPublicKey());

        cfg.FILTERED_G_ADDRESSES = {addr1, addr2};
        Application::pointer app = createTestApplication(clock, cfg);

        auto& persistor = app->getBannedAccountsPersistor();
        REQUIRE(persistor.getBannedAccounts().size() == 2);

        auto keys = persistor.getBannedAccountStrKeys();
        REQUIRE(std::find(keys.begin(), keys.end(), addr1) != keys.end());
        REQUIRE(std::find(keys.begin(), keys.end(), addr2) != keys.end());
    }

    SECTION("empty config does not affect existing bans")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.FILTERED_G_ADDRESSES = {};
        Application::pointer app = createTestApplication(clock, cfg);

        auto key1 = SecretKey::pseudoRandomForTesting();
        auto addr1 = KeyUtils::toStrKey(key1.getPublicKey());

        // Add a ban via the persistor
        app->getBannedAccountsPersistor().addBannedAccounts({addr1});
        REQUIRE(app->getBannedAccountsPersistor().getBannedAccounts().size() ==
                1);
    }

    SECTION("default FILTERED_G_ADDRESSES are migrated")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        // Default config has 3 hardcoded addresses
        Application::pointer app = createTestApplication(clock, cfg);

        auto& persistor = app->getBannedAccountsPersistor();
        auto keys = persistor.getBannedAccountStrKeys();
        REQUIRE(keys.size() == 3);
        REQUIRE(std::find(keys.begin(), keys.end(),
                          "GBO7VUL2TOKPWFAWKATIW7K3QYA7WQ63VDY5CAE6AFUUX6"
                          "BHZBOC2WXC") != keys.end());
        REQUIRE(std::find(keys.begin(), keys.end(),
                          "GATDQL767ZM2JQTBEG4BQ5WKOQNGAGWZDUN4GYT2UINPEU"
                          "3RT2UAMVZH") != keys.end());
        REQUIRE(std::find(keys.begin(), keys.end(),
                          "GCQCWEQDICASV3R737LPWPDJ3FPBC6XPWXKPJDL22DLQVG"
                          "OJAUH5DBJI") != keys.end());
    }
}

TEST_CASE("banaccounts HTTP command with persistence", "[banaccounts]")
{
    SECTION("ban via command persists and filters transactions")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.FILTERED_G_ADDRESSES = {};
        Application::pointer app = createTestApplication(clock, cfg);

        auto root = app->getRoot();
        auto srcKey = SecretKey::pseudoRandomForTesting();
        auto src = root->create(srcKey, 1000000000);

        // Ban via HTTP command
        auto addr = KeyUtils::toStrKey(srcKey.getPublicKey());
        auto result = app->getCommandHandler().manualCmd(
            "banaccounts?accountids=" + addr);
        REQUIRE(result.find("banned accounts updated") != std::string::npos);

        // Transaction from the banned source should be filtered
        auto acc = getAccount("acc");
        auto tx = src.tx({createAccount(acc.getPublicKey(), 1)});
        REQUIRE(app->getHerder().recvTransaction(tx, false).code ==
                TransactionQueue::AddResultCode::ADD_STATUS_FILTERED);

        // Verify persisted
        REQUIRE(app->getBannedAccountsPersistor().getBannedAccounts().size() ==
                1);
    }

    SECTION("ban is additive")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.FILTERED_G_ADDRESSES = {};
        Application::pointer app = createTestApplication(clock, cfg);

        auto key1 = SecretKey::pseudoRandomForTesting();
        auto key2 = SecretKey::pseudoRandomForTesting();
        auto addr1 = KeyUtils::toStrKey(key1.getPublicKey());
        auto addr2 = KeyUtils::toStrKey(key2.getPublicKey());

        // Add first account
        app->getCommandHandler().manualCmd("banaccounts?accountids=" + addr1);
        REQUIRE(app->getBannedAccountsPersistor().getBannedAccounts().size() ==
                1);

        // Add second account (additive, not replacing)
        app->getCommandHandler().manualCmd("banaccounts?accountids=" + addr2);
        REQUIRE(app->getBannedAccountsPersistor().getBannedAccounts().size() ==
                2);
    }

    SECTION("empty accountids returns error")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.FILTERED_G_ADDRESSES = {};
        Application::pointer app = createTestApplication(clock, cfg);

        auto result =
            app->getCommandHandler().manualCmd("banaccounts?accountids=");
        REQUIRE(result.find("accountids must not be empty") !=
                std::string::npos);
    }

    SECTION("list banned accounts")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.FILTERED_G_ADDRESSES = {};
        Application::pointer app = createTestApplication(clock, cfg);

        auto key1 = SecretKey::pseudoRandomForTesting();
        auto addr1 = KeyUtils::toStrKey(key1.getPublicKey());

        app->getCommandHandler().manualCmd("banaccounts?accountids=" + addr1);

        auto result = app->getCommandHandler().manualCmd("banaccounts");
        REQUIRE(result.find("bannedAccounts") != std::string::npos);
        REQUIRE(result.find(addr1) != std::string::npos);
    }

    SECTION("invalid address returns error")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.FILTERED_G_ADDRESSES = {};
        Application::pointer app = createTestApplication(clock, cfg);

        auto result = app->getCommandHandler().manualCmd(
            "banaccounts?accountids=NOT_A_VALID_ADDRESS");
        REQUIRE(result.find("invalid address") != std::string::npos);
    }

    SECTION("fee-bump with banned fee source is rejected")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.FILTERED_G_ADDRESSES = {};
        Application::pointer app = createTestApplication(clock, cfg);

        auto root = app->getRoot();
        auto filteredKey = SecretKey::pseudoRandomForTesting();
        auto feeSource = root->create(filteredKey, 1000000000);
        auto feeSourceAcct = TestAccount{*app, filteredKey};

        auto addr = KeyUtils::toStrKey(filteredKey.getPublicKey());
        app->getCommandHandler().manualCmd("banaccounts?accountids=" + addr);

        auto innerTx = root->tx({payment(root->getPublicKey(), 1)});
        auto fb = feeBump(*app, feeSourceAcct, innerTx, 200);

        REQUIRE(app->getHerder().recvTransaction(fb, false).code ==
                TransactionQueue::AddResultCode::ADD_STATUS_FILTERED);
    }
}

TEST_CASE("unbanaccounts HTTP command", "[banaccounts]")
{
    SECTION("unban specific accounts")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.FILTERED_G_ADDRESSES = {};
        Application::pointer app = createTestApplication(clock, cfg);

        auto key1 = SecretKey::pseudoRandomForTesting();
        auto key2 = SecretKey::pseudoRandomForTesting();
        auto addr1 = KeyUtils::toStrKey(key1.getPublicKey());
        auto addr2 = KeyUtils::toStrKey(key2.getPublicKey());

        // Ban both
        app->getCommandHandler().manualCmd("banaccounts?accountids=" + addr1 +
                                           "," + addr2);
        REQUIRE(app->getBannedAccountsPersistor().getBannedAccounts().size() ==
                2);

        // Unban one
        auto result = app->getCommandHandler().manualCmd(
            "unbanaccounts?accountids=" + addr1);
        REQUIRE(result.find("banned accounts updated") != std::string::npos);
        REQUIRE(result.find("\"removed\": 1") != std::string::npos);

        REQUIRE(app->getBannedAccountsPersistor().getBannedAccounts().size() ==
                1);

        // Verify addr2 is still banned
        auto keys = app->getBannedAccountsPersistor().getBannedAccountStrKeys();
        REQUIRE(std::find(keys.begin(), keys.end(), addr2) != keys.end());
        REQUIRE(std::find(keys.begin(), keys.end(), addr1) == keys.end());
    }

    SECTION("unban restores transaction acceptance")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.FILTERED_G_ADDRESSES = {};
        Application::pointer app = createTestApplication(clock, cfg);

        auto root = app->getRoot();
        auto srcKey = SecretKey::pseudoRandomForTesting();
        auto src = root->create(srcKey, 1000000000);
        auto addr = KeyUtils::toStrKey(srcKey.getPublicKey());

        // Ban
        app->getCommandHandler().manualCmd("banaccounts?accountids=" + addr);
        auto acc = getAccount("acc");
        auto tx = src.tx({createAccount(acc.getPublicKey(), 1)});
        REQUIRE(app->getHerder().recvTransaction(tx, false).code ==
                TransactionQueue::AddResultCode::ADD_STATUS_FILTERED);

        // Unban
        app->getCommandHandler().manualCmd("unbanaccounts?accountids=" + addr);
        REQUIRE(app->getHerder().recvTransaction(tx, false).code ==
                TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
    }

    SECTION("clear all banned accounts")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.FILTERED_G_ADDRESSES = {};
        Application::pointer app = createTestApplication(clock, cfg);

        auto root = app->getRoot();
        auto srcKey = SecretKey::pseudoRandomForTesting();
        auto src = root->create(srcKey, 1000000000);
        auto addr = KeyUtils::toStrKey(srcKey.getPublicKey());

        // Ban first
        app->getCommandHandler().manualCmd("banaccounts?accountids=" + addr);
        auto acc = getAccount("acc");
        auto tx = src.tx({createAccount(acc.getPublicKey(), 1)});
        REQUIRE(app->getHerder().recvTransaction(tx, false).code ==
                TransactionQueue::AddResultCode::ADD_STATUS_FILTERED);

        // Clear all via unbanaccounts with no accountids
        auto result = app->getCommandHandler().manualCmd("unbanaccounts");
        REQUIRE(result.find("banned accounts cleared") != std::string::npos);

        // Transaction should now be accepted
        REQUIRE(app->getHerder().recvTransaction(tx, false).code ==
                TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

        // Verify persisted state is empty
        REQUIRE(app->getBannedAccountsPersistor().getBannedAccounts().empty());
    }

    SECTION("invalid address returns error")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.FILTERED_G_ADDRESSES = {};
        Application::pointer app = createTestApplication(clock, cfg);

        auto result = app->getCommandHandler().manualCmd(
            "unbanaccounts?accountids=INVALID");
        REQUIRE(result.find("invalid address") != std::string::npos);
    }
}
