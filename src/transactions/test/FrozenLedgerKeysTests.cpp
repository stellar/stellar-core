// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/NetworkConfig.h"
#include "ledger/TrustLineWrapper.h"
#include "ledger/test/LedgerTestUtils.h"
#include "main/Application.h"
#include "test/Catch2.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/ProtocolVersion.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include <array>
#include <fmt/format.h>
#include <random>

namespace stellar
{
namespace
{
using namespace stellar::txtest;
EncodedLedgerKey
encodeLedgerKey(LedgerKey const& key)
{
    return xdr::xdr_to_opaque(key);
}

void
freezeKey(Application& app, LedgerKey const& key)
{
    modifySorobanNetworkConfig(app, [&](SorobanNetworkConfig& cfg) {
        cfg.mFrozenLedgerKeys.insert(key);
    });
}

void
unfreezeKey(Application& app, LedgerKey const& key)
{
    modifySorobanNetworkConfig(app, [&](SorobanNetworkConfig& cfg) {
        cfg.mFrozenLedgerKeys.erase(key);
    });
}

void
freezeAndUnfreezeKeys(Application& app, std::vector<LedgerKey> const& toFreeze,
                      std::vector<LedgerKey> const& toUnfreeze)
{
    modifySorobanNetworkConfig(app, [&](SorobanNetworkConfig& cfg) {
        cfg.mFrozenLedgerKeys.insert(toFreeze.begin(), toFreeze.end());
        for (auto const& k : toUnfreeze)
        {
            cfg.mFrozenLedgerKeys.erase(k);
        }
    });
}

void
bypassTxHash(Application& app, Hash const& txHash)
{
    modifySorobanNetworkConfig(app, [&](SorobanNetworkConfig& cfg) {
        cfg.mFreezeBypassTxs.insert(txHash);
    });
}

void
unbypassTxHash(Application& app, Hash const& txHash)
{
    modifySorobanNetworkConfig(app, [&](SorobanNetworkConfig& cfg) {
        cfg.mFreezeBypassTxs.erase(txHash);
    });
}

void
bypassAndUnbypassTxHashes(Application& app, std::vector<Hash> const& toBypass,
                          std::vector<Hash> const& toUnbypass)
{
    modifySorobanNetworkConfig(app, [&](SorobanNetworkConfig& cfg) {
        cfg.mFreezeBypassTxs.insert(toBypass.begin(), toBypass.end());
        for (auto const& h : toUnbypass)
        {
            cfg.mFreezeBypassTxs.erase(h);
        }
    });
}

UnorderedSet<LedgerKey>
loadFrozenKeysFromLedger(Application& app)
{
    LedgerSnapshot ls(app);
    auto configKey = configSettingKey(CONFIG_SETTING_FROZEN_LEDGER_KEYS);
    auto entry = ls.load(configKey);
    REQUIRE(entry);

    auto const& frozenKeys =
        entry.current().data.configSetting().frozenLedgerKeys().keys;
    UnorderedSet<LedgerKey> result;
    for (auto const& encodedKey : frozenKeys)
    {
        LedgerKey lk;
        xdr::xdr_from_opaque(encodedKey, lk);
        result.insert(lk);
    }

    // Verify that we cache the correct frozen key set.
    auto const& sorobanConfig =
        app.getLedgerManager().getLastClosedSorobanNetworkConfig();
    REQUIRE(sorobanConfig.frozenLedgerKeys() == result);
    return result;
}

UnorderedSet<Hash>
loadFreezeBypassTxsFromLedger(Application& app)
{
    LedgerSnapshot ls(app);
    auto configKey = configSettingKey(CONFIG_SETTING_FREEZE_BYPASS_TXS);
    auto entry = ls.load(configKey);
    REQUIRE(entry);

    auto const& bypassTxs =
        entry.current().data.configSetting().freezeBypassTxs().txHashes;
    UnorderedSet<Hash> result;
    for (auto const& txHash : bypassTxs)
    {
        result.insert(txHash);
    }

    // Verify that we cache the correct bypass tx hash set.
    auto const& sorobanConfig =
        app.getLedgerManager().getLastClosedSorobanNetworkConfig();
    REQUIRE(sorobanConfig.freezeBypassTxs() == result);
    return result;
}

TEST_CASE("frozen ledger keys config setting does not exist prior to p26",
          "[frozenledgerkeys][upgrades][gen-lcm]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = static_cast<uint32_t>(ProtocolVersion::V_25);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(ProtocolVersion::V_25);
    auto app = createTestApplication(clock, cfg);
    auto root = app->getRoot();
    LedgerSnapshot ls(*app);
    auto configKey = configSettingKey(CONFIG_SETTING_FROZEN_LEDGER_KEYS);
    auto entry = ls.load(configKey);
    REQUIRE(!entry);
}

TEST_CASE("freeze bypass txs config setting does not exist prior to p26",
          "[frozenledgerkeys][upgrades][gen-lcm]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = static_cast<uint32_t>(ProtocolVersion::V_25);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(ProtocolVersion::V_25);
    auto app = createTestApplication(clock, cfg);
    auto root = app->getRoot();
    LedgerSnapshot ls(*app);
    auto configKey = configSettingKey(CONFIG_SETTING_FREEZE_BYPASS_TXS);
    auto entry = ls.load(configKey);
    REQUIRE(!entry);
}

TEST_CASE_VERSIONS("frozen ledger keys config setting upgrades",
                   "[frozenledgerkeys][upgrades][gen-lcm]")
{
    auto cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(26, *app, [&] {
        auto const protocolVersion = app->getLedgerManager()
                                         .getLastClosedLedgerHeader()
                                         .header.ledgerVersion;

        auto initFrozenKeys = loadFrozenKeysFromLedger(*app);
        // Frozen keys are initially empty.
        REQUIRE(initFrozenKeys.empty());

        // CONFIG_SETTING_FROZEN_LEDGER_KEYS cannot be upgraded directly;
        // only the delta mechanism is allowed.
        REQUIRE(SorobanNetworkConfig::isNonUpgradeableConfigSettingEntry(
            CONFIG_SETTING_FROZEN_LEDGER_KEYS));

        auto makeAccountKey = [&](uint8_t id) {
            AccountID accountID{};
            accountID.ed25519()[0] = id;
            return accountKey(accountID);
        };

        SECTION("frozen keys delta validation accepts valid key types")
        {
            // Create a delta with valid key types: ACCOUNT, TRUSTLINE,
            // CONTRACT_DATA, CONTRACT_CODE
            ConfigSettingEntry deltaEntry;
            deltaEntry.configSettingID(CONFIG_SETTING_FROZEN_LEDGER_KEYS_DELTA);

            auto accKey = makeAccountKey(1);
            Asset asset{};
            asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
            asset.alphaNum4().issuer = makeAccountKey(2).account().accountID;
            strToAssetCode(asset.alphaNum4().assetCode, "USD");
            auto tlKey = trustlineKey(accKey.account().accountID, asset);

            SCAddress contractAddr;
            contractAddr.type(SC_ADDRESS_TYPE_CONTRACT);
            contractAddr.contractId() = sha256("test_contract");
            SCVal scKey;
            scKey.type(SCV_SYMBOL);
            scKey.sym() = "key";
            auto cdKey = contractDataKey(contractAddr, scKey,
                                         ContractDataDurability::PERSISTENT);
            auto ccKey = contractCodeKey(sha256("test_code"));

            deltaEntry.frozenLedgerKeysDelta().keysToFreeze.emplace_back(
                encodeLedgerKey(accKey));
            deltaEntry.frozenLedgerKeysDelta().keysToFreeze.emplace_back(
                encodeLedgerKey(tlKey));
            deltaEntry.frozenLedgerKeysDelta().keysToFreeze.emplace_back(
                encodeLedgerKey(cdKey));
            deltaEntry.frozenLedgerKeysDelta().keysToFreeze.emplace_back(
                encodeLedgerKey(ccKey));

            REQUIRE(SorobanNetworkConfig::isValidConfigSettingEntry(
                deltaEntry, protocolVersion));
        }

        SECTION("frozen keys delta validation rejects liquidity pool share "
                "trustlines")
        {
            ConfigSettingEntry deltaEntry;
            deltaEntry.configSettingID(CONFIG_SETTING_FROZEN_LEDGER_KEYS_DELTA);

            auto accKey = makeAccountKey(1);
            PoolID poolID = sha256("test_pool");
            auto poolShareTlKey =
                poolShareTrustLineKey(accKey.account().accountID, poolID);

            deltaEntry.frozenLedgerKeysDelta().keysToFreeze.emplace_back(
                encodeLedgerKey(poolShareTlKey));

            REQUIRE(!SorobanNetworkConfig::isValidConfigSettingEntry(
                deltaEntry, protocolVersion));
        }

        SECTION("frozen keys delta validation rejects issuer trustline keys")
        {
            ConfigSettingEntry deltaEntry;
            deltaEntry.configSettingID(CONFIG_SETTING_FROZEN_LEDGER_KEYS_DELTA);

            auto accKey = makeAccountKey(1);
            Asset asset{};
            asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
            asset.alphaNum4().issuer = accKey.account().accountID;
            strToAssetCode(asset.alphaNum4().assetCode, "USD");
            auto issuerTlKey = trustlineKey(accKey.account().accountID, asset);

            deltaEntry.frozenLedgerKeysDelta().keysToFreeze.emplace_back(
                encodeLedgerKey(issuerTlKey));

            REQUIRE(!SorobanNetworkConfig::isValidConfigSettingEntry(
                deltaEntry, protocolVersion));
        }

        SECTION("frozen keys delta validation rejects invalid key types")
        {
            for (auto t : xdr::xdr_traits<LedgerEntryType>::enum_values())
            {
                auto type = static_cast<LedgerEntryType>(t);
                if (type == ACCOUNT || type == TRUSTLINE ||
                    type == CONTRACT_DATA || type == CONTRACT_CODE)
                {
                    continue;
                }

                ConfigSettingEntry deltaEntry;
                deltaEntry.configSettingID(
                    CONFIG_SETTING_FROZEN_LEDGER_KEYS_DELTA);

                LedgerKey lk;
                lk.type(type);
                deltaEntry.frozenLedgerKeysDelta().keysToFreeze.emplace_back(
                    encodeLedgerKey(lk));

                REQUIRE(!SorobanNetworkConfig::isValidConfigSettingEntry(
                    deltaEntry, protocolVersion));
            }
        }

        SECTION("frozen keys delta validation rejects malformed XDR")
        {
            ConfigSettingEntry deltaEntry;
            deltaEntry.configSettingID(CONFIG_SETTING_FROZEN_LEDGER_KEYS_DELTA);

            LedgerKey lk;
            lk.type(ACCOUNT);
            deltaEntry.frozenLedgerKeysDelta().keysToFreeze.emplace_back(
                encodeLedgerKey(lk));
            REQUIRE(SorobanNetworkConfig::isValidConfigSettingEntry(
                deltaEntry, protocolVersion));
            // Corrupt the XDR by changing some bytes in the encoded key.
            deltaEntry.frozenLedgerKeysDelta().keysToFreeze[0][0] = 0xFF;
            deltaEntry.frozenLedgerKeysDelta().keysToFreeze[0][3] = 0xFF;
            REQUIRE(!SorobanNetworkConfig::isValidConfigSettingEntry(
                deltaEntry, protocolVersion));
        }

        SECTION("adds and remove frozen keys")
        {
            auto k1 = makeAccountKey(1);
            freezeKey(*app, k1);

            // Add a key
            auto frozenKeys = loadFrozenKeysFromLedger(*app);
            REQUIRE(frozenKeys.size() == 1);
            REQUIRE(frozenKeys.count(k1) == 1);

            // Add another key
            auto k2 = makeAccountKey(2);
            freezeKey(*app, k2);

            frozenKeys = loadFrozenKeysFromLedger(*app);
            REQUIRE(frozenKeys.size() == 2);
            REQUIRE(frozenKeys.count(k1) == 1);
            REQUIRE(frozenKeys.count(k2) == 1);

            // Remove a key
            unfreezeKey(*app, k1);
            frozenKeys = loadFrozenKeysFromLedger(*app);
            REQUIRE(frozenKeys.size() == 1);
            REQUIRE(frozenKeys.count(k2) == 1);

            // Add and remove in a single upgrade
            freezeAndUnfreezeKeys(*app, {k1}, {k2});
            frozenKeys = loadFrozenKeysFromLedger(*app);
            REQUIRE(frozenKeys.size() == 1);
            REQUIRE(frozenKeys.count(k1) == 1);

            // Add multiple keys at once
            auto k3 = makeAccountKey(3);
            auto k4 = makeAccountKey(4);
            freezeAndUnfreezeKeys(*app, {k2, k3, k4}, {});
            frozenKeys = loadFrozenKeysFromLedger(*app);
            REQUIRE(frozenKeys.size() == 4);
            REQUIRE(frozenKeys.count(k1) == 1);
            REQUIRE(frozenKeys.count(k2) == 1);
            REQUIRE(frozenKeys.count(k3) == 1);
            REQUIRE(frozenKeys.count(k4) == 1);

            // Remove multiple keys at once
            freezeAndUnfreezeKeys(*app, {}, {k1, k3});
            frozenKeys = loadFrozenKeysFromLedger(*app);
            REQUIRE(frozenKeys.size() == 2);
            REQUIRE(frozenKeys.count(k2) == 1);
            REQUIRE(frozenKeys.count(k4) == 1);

            // Removing non-existent key is no-op
            unfreezeKey(*app, k1);
            auto frozenKeys2 = loadFrozenKeysFromLedger(*app);
            REQUIRE(frozenKeys2 == frozenKeys);

            // Adding a key that's already frozen is no-op
            freezeKey(*app, k2);
            frozenKeys2 = loadFrozenKeysFromLedger(*app);
            REQUIRE(frozenKeys2 == frozenKeys);
        }

        SECTION("same key in both freeze and unfreeze results in key not "
                "frozen")
        {
            auto k1 = makeAccountKey(1);
            freezeKey(*app, k1);
            auto frozenKeys = loadFrozenKeysFromLedger(*app);
            REQUIRE(frozenKeys.size() == 1);
            REQUIRE(frozenKeys.count(k1) == 1);

            // Apply a delta with the same key in both lists.
            // Implementation inserts first, then removes, so the key
            // ends up not frozen.
            freezeAndUnfreezeKeys(*app, {k1}, {k1});
            frozenKeys = loadFrozenKeysFromLedger(*app);
            REQUIRE(frozenKeys.empty());
        }
    });
}

TEST_CASE_VERSIONS("freeze bypass txs config setting upgrades",
                   "[frozenledgerkeys][upgrades][gen-lcm]")
{
    auto cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    for_versions_from(26, *app, [&] {
        auto const protocolVersion = app->getLedgerManager()
                                         .getLastClosedLedgerHeader()
                                         .header.ledgerVersion;

        auto bypassTxs = loadFreezeBypassTxsFromLedger(*app);
        // Bypass tx hashes are initially empty.
        REQUIRE(bypassTxs.empty());

        // CONFIG_SETTING_FREEZE_BYPASS_TXS cannot be upgraded directly;
        // only the delta mechanism is allowed.
        REQUIRE(SorobanNetworkConfig::isNonUpgradeableConfigSettingEntry(
            CONFIG_SETTING_FREEZE_BYPASS_TXS));

        auto makeHash = [&](uint8_t id) {
            Hash h{};
            h[0] = id;
            return h;
        };

        SECTION("freeze bypass txs delta validation accepts hashes")
        {
            ConfigSettingEntry deltaEntry;
            deltaEntry.configSettingID(CONFIG_SETTING_FREEZE_BYPASS_TXS_DELTA);

            deltaEntry.freezeBypassTxsDelta().addTxs.emplace_back(makeHash(1));
            deltaEntry.freezeBypassTxsDelta().removeTxs.emplace_back(
                makeHash(2));

            REQUIRE(SorobanNetworkConfig::isValidConfigSettingEntry(
                deltaEntry, protocolVersion));
        }

        SECTION("adds and removes bypass tx hashes")
        {
            auto h1 = makeHash(1);
            bypassTxHash(*app, h1);

            // Add a hash
            bypassTxs = loadFreezeBypassTxsFromLedger(*app);
            REQUIRE(bypassTxs.size() == 1);
            REQUIRE(bypassTxs.count(h1) == 1);

            // Add another hash
            auto h2 = makeHash(2);
            bypassTxHash(*app, h2);

            bypassTxs = loadFreezeBypassTxsFromLedger(*app);
            REQUIRE(bypassTxs.size() == 2);
            REQUIRE(bypassTxs.count(h1) == 1);
            REQUIRE(bypassTxs.count(h2) == 1);

            // Remove a hash
            unbypassTxHash(*app, h1);
            bypassTxs = loadFreezeBypassTxsFromLedger(*app);
            REQUIRE(bypassTxs.size() == 1);
            REQUIRE(bypassTxs.count(h2) == 1);

            // Add and remove in a single upgrade
            bypassAndUnbypassTxHashes(*app, {h1}, {h2});
            bypassTxs = loadFreezeBypassTxsFromLedger(*app);
            REQUIRE(bypassTxs.size() == 1);
            REQUIRE(bypassTxs.count(h1) == 1);

            // Add multiple hashes at once
            auto h3 = makeHash(3);
            auto h4 = makeHash(4);
            bypassAndUnbypassTxHashes(*app, {h2, h3, h4}, {});
            bypassTxs = loadFreezeBypassTxsFromLedger(*app);
            REQUIRE(bypassTxs.size() == 4);
            REQUIRE(bypassTxs.count(h1) == 1);
            REQUIRE(bypassTxs.count(h2) == 1);
            REQUIRE(bypassTxs.count(h3) == 1);
            REQUIRE(bypassTxs.count(h4) == 1);

            // Remove multiple hashes at once
            bypassAndUnbypassTxHashes(*app, {}, {h1, h3});
            bypassTxs = loadFreezeBypassTxsFromLedger(*app);
            REQUIRE(bypassTxs.size() == 2);
            REQUIRE(bypassTxs.count(h2) == 1);
            REQUIRE(bypassTxs.count(h4) == 1);

            // Removing non-existent hash is no-op
            unbypassTxHash(*app, h1);
            auto bypassTxs2 = loadFreezeBypassTxsFromLedger(*app);
            REQUIRE(bypassTxs2 == bypassTxs);

            // Adding an existing hash is no-op
            bypassTxHash(*app, h2);
            bypassTxs2 = loadFreezeBypassTxsFromLedger(*app);
            REQUIRE(bypassTxs2 == bypassTxs);
        }
    });
}

TEST_CASE("freeze bypass tx hash allows frozen key access at validation time",
          "[frozenledgerkeys][tx][bypass][gen-lcm]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    auto root = app->getRoot();
    auto const& lm = app->getLedgerManager();

    auto a1 =
        root->create("A1", lm.getLastMinBalance(10) + 10 * lm.getLastTxFee());
    auto a2 =
        root->create("A2", lm.getLastMinBalance(10) + 10 * lm.getLastTxFee());
    auto feeSource = root->create("feeSource", lm.getLastMinBalance(10) +
                                                   10 * lm.getLastTxFee());

    enum class TxWrapperKind
    {
        REGULAR,
        FEE_BUMP
    };

    auto makeTx = [&](TxWrapperKind txWrapperKind) {
        std::vector<Operation> ops = {payment(a2.getPublicKey(), 100)};
        auto innerTx = transactionFromOperations(*app, a1.getSecretKey(),
                                                 a1.nextSequenceNumber(), ops);
        if (txWrapperKind == TxWrapperKind::REGULAR)
        {
            return std::make_pair(TransactionTestFramePtr(innerTx),
                                  innerTx->getContentsHash());
        }

        auto tx = feeBump(*app, feeSource, innerTx, 10000);
        return std::make_pair(tx, tx->getContentsHash());
    };

    auto checkFrozen = [&](TransactionTestFramePtr& tx,
                           bool expectInnerFrozenResult) {
        LedgerSnapshot ls(*app);
        auto result = tx->checkValid(app->getAppConnector(), ls, 0, 0, 0);
        REQUIRE(!result->isSuccess());
        if (expectInnerFrozenResult)
        {
            REQUIRE(result->getResultCode() == txFEE_BUMP_INNER_FAILED);
            auto const& fbRes = result->getXDR();
            auto const& innerRes = fbRes.result.innerResultPair().result;
            REQUIRE(innerRes.result.code() == txFROZEN_KEY_ACCESSED);
        }
        else
        {
            REQUIRE(result->getResultCode() == txFROZEN_KEY_ACCESSED);
        }
    };

    auto checkValid = [&](TransactionTestFramePtr& tx) {
        LedgerSnapshot ls(*app);
        auto result = tx->checkValid(app->getAppConnector(), ls, 0, 0, 0);
        REQUIRE(result->isSuccess());
    };

    auto checkBypassScenario = [&](TxWrapperKind txWrapperKind,
                                   std::function<void()> freezeKeyFn,
                                   bool expectInnerFrozenResult) {
        freezeKeyFn();
        auto [tx, bypassHash] = makeTx(txWrapperKind);
        checkFrozen(tx, expectInnerFrozenResult);

        bypassTxHash(*app, bypassHash);
        checkValid(tx);
    };

    SECTION("bypass frozen tx source account in regular tx")
    {
        checkBypassScenario(
            TxWrapperKind::REGULAR,
            [&] { freezeKey(*app, accountKey(a1.getPublicKey())); }, false);
    }

    SECTION("bypass frozen inner tx source account in fee bump")
    {
        checkBypassScenario(
            TxWrapperKind::FEE_BUMP,
            [&] { freezeKey(*app, accountKey(a1.getPublicKey())); }, true);
    }

    SECTION("bypass frozen key accessed by operation in regular tx")
    {
        checkBypassScenario(
            TxWrapperKind::REGULAR,
            [&] { freezeKey(*app, accountKey(a2.getPublicKey())); }, false);
    }

    SECTION("bypass frozen key accessed by operation in fee bump inner tx")
    {
        checkBypassScenario(
            TxWrapperKind::FEE_BUMP,
            [&] { freezeKey(*app, accountKey(a2.getPublicKey())); }, true);
    }

    SECTION("bypass frozen fee bump source account")
    {
        checkBypassScenario(
            TxWrapperKind::FEE_BUMP,
            [&] { freezeKey(*app, accountKey(feeSource.getPublicKey())); },
            false);
    }

    SECTION("inner tx hash does not bypass frozen fee bump source")
    {
        freezeKey(*app, accountKey(feeSource.getPublicKey()));

        // Build the inner tx explicitly so we can capture its contents
        // hash separately from the fee bump contents hash.
        std::vector<Operation> ops = {payment(a2.getPublicKey(), 100)};
        auto innerTx = transactionFromOperations(*app, a1.getSecretKey(),
                                                 a1.nextSequenceNumber(), ops);
        auto innerHash = innerTx->getContentsHash();

        auto tx = feeBump(*app, feeSource, innerTx, 10000);
        auto feeBumpHash = tx->getContentsHash();
        REQUIRE(innerHash != feeBumpHash);

        // Bypass only the inner tx hash -- fee bump source is still
        // frozen and the fee bump contents hash is not bypassed.
        bypassTxHash(*app, innerHash);
        checkFrozen(tx, false);

        // Now bypass the actual fee bump hash -- should pass.
        bypassTxHash(*app, feeBumpHash);
        checkValid(tx);
    }
}

TEST_CASE("frozen ledger keys in Soroban footprint",
          "[frozenledgerkeys][tx][soroban][gen-lcm]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    overrideSorobanNetworkConfigForTest(*app);
    auto root = app->getRoot();

    auto a1 = root->create("A1", app->getLedgerManager().getLastMinBalance(2) +
                                     10'000'000);

    std::unordered_set<LedgerEntryType> const sorobanFootprintTypes = {
        CONTRACT_DATA, CONTRACT_CODE, ACCOUNT, TRUSTLINE};

    auto isFreezableSorobanKey = [](LedgerKey const& key) {
        if (key.type() != TRUSTLINE)
        {
            return true;
        }

        auto const& tl = key.trustLine();
        return tl.asset.type() != ASSET_TYPE_POOL_SHARE &&
               !isIssuer(tl.accountID, tl.asset);
    };

    for (int i = 0; i < 10; ++i)
    {
        INFO("iteration " << i);

        UnorderedSet<LedgerKey> seenKeys;
        auto readOnly = LedgerTestUtils::generateValidUniqueLedgerKeysWithTypes(
            sorobanFootprintTypes, 20, seenKeys);
        auto readWrite =
            LedgerTestUtils::generateValidUniqueLedgerKeysWithTypes(
                sorobanFootprintTypes, 20, seenKeys);

        std::vector<LedgerKey> freezableKeys;
        freezableKeys.reserve(readOnly.size() + readWrite.size());

        for (auto const& key : readOnly)
        {
            if (isFreezableSorobanKey(key))
            {
                freezableKeys.emplace_back(key);
            }
        }

        for (auto const& key : readWrite)
        {
            if (isFreezableSorobanKey(key))
            {
                freezableKeys.emplace_back(key);
            }
        }

        REQUIRE(!freezableKeys.empty());
        stellar::uniform_int_distribution<size_t> dist(0, freezableKeys.size() -
                                                              1);
        auto frozenKey = freezableKeys[dist(Catch::rng())];
        freezeKey(*app, frozenKey);

        SorobanResources resources;
        resources.instructions = 800'000;
        resources.diskReadBytes = 1000;
        resources.writeBytes = 1000;
        resources.footprint.readOnly.insert(resources.footprint.readOnly.end(),
                                            readOnly.begin(), readOnly.end());
        resources.footprint.readWrite.insert(
            resources.footprint.readWrite.end(), readWrite.begin(),
            readWrite.end());

        auto tx = createUploadWasmTx(*app, a1, 1000, DEFAULT_TEST_RESOURCE_FEE,
                                     resources);

        LedgerSnapshot ls(*app);
        auto result = tx->checkValid(app->getAppConnector(), ls, 0, 0, 0);
        REQUIRE(!result->isSuccess());
        REQUIRE(result->getResultCode() == txFROZEN_KEY_ACCESSED);

        unfreezeKey(*app, frozenKey);
    }
}

TEST_CASE("source account frozen", "[frozenledgerkeys][tx][gen-lcm]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    auto root = app->getRoot();
    auto const& lm = app->getLedgerManager();

    auto a1 =
        root->create("A1", lm.getLastMinBalance(10) + 10 * lm.getLastTxFee());
    auto a2 =
        root->create("A2", lm.getLastMinBalance(10) + 10 * lm.getLastTxFee());

    auto checkTx = [&](TransactionTestFramePtr& tx) {
        LedgerSnapshot ls(*app);
        auto result = tx->checkValid(app->getAppConnector(), ls, 0, 0, 0);
        REQUIRE(!result->isSuccess());
        REQUIRE(result->getResultCode() == txFROZEN_KEY_ACCESSED);
    };

    SECTION("tx source account frozen")
    {
        auto a1Key = accountKey(a1.getPublicKey());
        freezeKey(*app, a1Key);

        auto tx = transactionFromOperations(*app, a1.getSecretKey(),
                                            a1.nextSequenceNumber(),
                                            {payment(a2.getPublicKey(), 100)});
        checkTx(tx);
    }

    SECTION("fee bump source account frozen")
    {
        auto feeSource = root->create("feeSource", lm.getLastMinBalance(10) +
                                                       10 * lm.getLastTxFee());
        auto feeSrcKey = accountKey(feeSource.getPublicKey());
        freezeKey(*app, feeSrcKey);
        auto innerTx = transactionFromOperations(
            *app, a1.getSecretKey(), a1.nextSequenceNumber(),
            {payment(a2.getPublicKey(), 100)});
        auto feeBumpTx = feeBump(*app, feeSource, innerTx, 10000);
        checkTx(feeBumpTx);
    }

    SECTION("op source account frozen")
    {
        auto opSource = root->create("opSource", lm.getLastMinBalance(10) +
                                                     10 * lm.getLastTxFee());
        auto opSrcKey = accountKey(opSource.getPublicKey());
        freezeKey(*app, opSrcKey);

        auto payOp = payment(a2.getPublicKey(), 100);
        payOp.sourceAccount.activate() =
            toMuxedAccount(opSource.getPublicKey());

        auto tx = transactionFromOperations(*app, a1.getSecretKey(),
                                            a1.nextSequenceNumber(), {payOp});
        tx->addSignature(opSource.getSecretKey());
        checkTx(tx);
    }

    SECTION("one of multiple ops source account frozen")
    {
        auto opSource1 = root->create("opSource1", lm.getLastMinBalance(10) +
                                                       10 * lm.getLastTxFee());
        auto opSrcKey1 = accountKey(opSource1.getPublicKey());
        auto opSource2 = root->create("opSource2", lm.getLastMinBalance(10) +
                                                       10 * lm.getLastTxFee());
        auto opSrcKey2 = accountKey(opSource2.getPublicKey());
        freezeKey(*app, opSrcKey2);

        auto op1 = payment(a2.getPublicKey(), 100);
        op1.sourceAccount.activate() = toMuxedAccount(opSource1.getPublicKey());
        auto op2 = payment(a2.getPublicKey(), 100);
        auto op3 = payment(a2.getPublicKey(), 100);
        op3.sourceAccount.activate() = toMuxedAccount(opSource2.getPublicKey());

        auto tx = transactionFromOperations(
            *app, a1.getSecretKey(), a1.nextSequenceNumber(), {op1, op2, op3});
        tx->addSignature(opSource1.getSecretKey());

        checkTx(tx);
    }

    SECTION("tx source AND destination both frozen")
    {
        auto a1Key = accountKey(a1.getPublicKey());
        auto a2Key = accountKey(a2.getPublicKey());
        freezeKey(*app, a1Key);
        freezeKey(*app, a2Key);

        auto tx = transactionFromOperations(*app, a1.getSecretKey(),
                                            a1.nextSequenceNumber(),
                                            {payment(a2.getPublicKey(), 100)});
        checkTx(tx);
    }

    SECTION("tx source frozen via muxed account ID")
    {
        auto a1Key = accountKey(a1.getPublicKey());
        freezeKey(*app, a1Key);

        // Build a tx envelope with a muxed source account (includes a
        // memo id). The frozen key is keyed on the underlying ed25519
        // account, so it must still be detected.
        auto muxedSrc = toMuxedAccount(a1.getPublicKey(), 12345);
        auto payOp = payment(a2.getPublicKey(), 100);
        TransactionEnvelope env(ENVELOPE_TYPE_TX);
        env.v1().tx.sourceAccount = muxedSrc;
        env.v1().tx.fee = 100;
        env.v1().tx.seqNum = a1.nextSequenceNumber();
        env.v1().tx.operations.emplace_back(payOp);

        auto tx = TransactionTestFrame::fromTxFrame(
            TransactionFrameBase::makeTransactionFromWire(app->getNetworkID(),
                                                          env));
        tx->addSignature(a1.getSecretKey());
        checkTx(tx);
    }

    SECTION("op source frozen via muxed account ID")
    {
        auto opSource = root->create("opSourceMux", lm.getLastMinBalance(10) +
                                                        10 * lm.getLastTxFee());
        auto opSrcKey = accountKey(opSource.getPublicKey());
        freezeKey(*app, opSrcKey);

        auto payOp = payment(a2.getPublicKey(), 100);
        payOp.sourceAccount.activate() =
            toMuxedAccount(opSource.getPublicKey(), 99999);

        auto tx = transactionFromOperations(*app, a1.getSecretKey(),
                                            a1.nextSequenceNumber(), {payOp});
        tx->addSignature(opSource.getSecretKey());
        checkTx(tx);
    }

    SECTION("unfreeze restores tx validity")
    {
        auto a1Key = accountKey(a1.getPublicKey());
        freezeKey(*app, a1Key);

        auto tx = transactionFromOperations(*app, a1.getSecretKey(),
                                            a1.nextSequenceNumber(),
                                            {payment(a2.getPublicKey(), 100)});
        checkTx(tx);

        unfreezeKey(*app, a1Key);

        LedgerSnapshot ls(*app);
        auto result = tx->checkValid(app->getAppConnector(), ls, 0, 0, 0);
        REQUIRE(result->isSuccess());
    }
}

TEST_CASE("source trustline frozen", "[frozenledgerkeys][tx][gen-lcm]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    auto root = app->getRoot();
    auto const& lm = app->getLedgerManager();

    auto issuer = root->create("issuer", lm.getLastMinBalance(10) +
                                             10 * lm.getLastTxFee());
    auto a1 =
        root->create("A1", lm.getLastMinBalance(10) + 10 * lm.getLastTxFee());
    auto a2 =
        root->create("A2", lm.getLastMinBalance(10) + 10 * lm.getLastTxFee());

    auto usd = makeAsset(issuer, "USD");
    auto eur = makeAsset(issuer, "EUR");

    a1.changeTrust(usd, INT64_MAX);
    a1.changeTrust(eur, INT64_MAX);
    a2.changeTrust(usd, INT64_MAX);
    a2.changeTrust(eur, INT64_MAX);
    issuer.pay(a1, usd, 10000);
    issuer.pay(a1, eur, 10000);
    issuer.pay(a2, usd, 10000);
    issuer.pay(a2, eur, 10000);

    auto checkAccessesFrozenKey = [&](Operation const& op) {
        auto tx = transactionFromOperations(*app, a1.getSecretKey(),
                                            a1.nextSequenceNumber(), {op});
        LedgerSnapshot ls(*app);
        auto result = tx->checkValid(app->getAppConnector(), ls, 0, 0, 0);
        REQUIRE(!result->isSuccess());
        REQUIRE(result->getResultCode() == txFROZEN_KEY_ACCESSED);
    };

    auto a1UsdTL = trustlineKey(a1.getPublicKey(), usd);
    auto a1EurTL = trustlineKey(a1.getPublicKey(), eur);

    SECTION("PaymentOp source trustline frozen")
    {
        freezeKey(*app, a1UsdTL);
        checkAccessesFrozenKey(payment(a2.getPublicKey(), usd, 100));
    }

    SECTION("PathPaymentStrictReceive sendAsset trustline frozen")
    {
        freezeKey(*app, a1UsdTL);
        checkAccessesFrozenKey(
            pathPayment(a2.getPublicKey(), usd, 100, usd, 100, {}));
    }

    SECTION("PathPaymentStrictSend sendAsset trustline frozen")
    {
        freezeKey(*app, a1UsdTL);
        checkAccessesFrozenKey(
            pathPaymentStrictSend(a2.getPublicKey(), usd, 100, usd, 90, {}));
    }

    SECTION("ManageSellOffer selling trustline frozen")
    {
        freezeKey(*app, a1UsdTL);
        checkAccessesFrozenKey(manageOffer(0, usd, eur, Price{1, 1}, 100));
    }

    SECTION("ManageSellOffer buying trustline frozen")
    {
        freezeKey(*app, a1EurTL);
        checkAccessesFrozenKey(manageOffer(0, usd, eur, Price{1, 1}, 100));
    }

    SECTION("ManageBuyOffer selling trustline frozen")
    {
        freezeKey(*app, a1UsdTL);
        checkAccessesFrozenKey(manageBuyOffer(0, usd, eur, Price{1, 1}, 100));
    }

    SECTION("ManageBuyOffer buying trustline frozen")
    {
        freezeKey(*app, a1EurTL);
        checkAccessesFrozenKey(manageBuyOffer(0, usd, eur, Price{1, 1}, 100));
    }

    SECTION("CreatePassiveSellOffer selling trustline frozen")
    {
        freezeKey(*app, a1UsdTL);
        checkAccessesFrozenKey(createPassiveOffer(usd, eur, Price{1, 1}, 100));
    }

    SECTION("CreatePassiveSellOffer buying trustline frozen")
    {
        freezeKey(*app, a1EurTL);
        checkAccessesFrozenKey(createPassiveOffer(usd, eur, Price{1, 1}, 100));
    }

    SECTION("ChangeTrustOp trustline frozen")
    {
        freezeKey(*app, a1UsdTL);
        checkAccessesFrozenKey(changeTrust(usd, INT64_MAX - 1));
    }
    SECTION("CreateClaimableBalance source trustline frozen")
    {
        freezeKey(*app, a1UsdTL);

        Claimant claimant;
        claimant.v0().destination = a2.getPublicKey();
        claimant.v0().predicate.type(CLAIM_PREDICATE_UNCONDITIONAL);

        checkAccessesFrozenKey(createClaimableBalance(usd, 100, {claimant}));
    }
}

TEST_CASE("operation destination frozen", "[frozenledgerkeys][tx][gen-lcm]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    auto root = app->getRoot();
    auto const& lm = app->getLedgerManager();

    auto issuer = root->create("issuer", lm.getLastMinBalance(10) +
                                             10 * lm.getLastTxFee());
    auto a1 =
        root->create("A1", lm.getLastMinBalance(10) + 10 * lm.getLastTxFee());
    auto a2 =
        root->create("A2", lm.getLastMinBalance(10) + 10 * lm.getLastTxFee());

    auto usd = makeAsset(issuer, "USD");
    auto xlm = makeNativeAsset();
    auto eur = makeAsset(issuer, "EUR");

    a1.changeTrust(usd, INT64_MAX);
    a1.changeTrust(eur, INT64_MAX);
    a2.changeTrust(usd, INT64_MAX);
    a2.changeTrust(eur, INT64_MAX);
    issuer.pay(a1, usd, 10000);
    issuer.pay(a1, eur, 10000);
    issuer.pay(a2, usd, 10000);
    issuer.pay(a2, eur, 10000);

    auto checkAccessesFrozenKeyWithSource = [&](auto& sourceAccount,
                                                Operation const& op) {
        auto tx =
            transactionFromOperations(*app, sourceAccount.getSecretKey(),
                                      sourceAccount.nextSequenceNumber(), {op});
        LedgerSnapshot ls(*app);
        auto result = tx->checkValid(app->getAppConnector(), ls, 0, 0, 0);
        REQUIRE(!result->isSuccess());
        REQUIRE(result->getResultCode() == txFROZEN_KEY_ACCESSED);
    };

    auto checkAccessesFrozenKey = [&](Operation const& op) {
        checkAccessesFrozenKeyWithSource(a1, op);
    };

    auto a2UsdTL = trustlineKey(a2.getPublicKey(), usd);

    SECTION("PaymentOp destination trustline frozen")
    {
        freezeKey(*app, a2UsdTL);
        checkAccessesFrozenKey(payment(a2.getPublicKey(), usd, 100));
    }

    SECTION("PaymentOp native destination account frozen")
    {
        auto a2Key = accountKey(a2.getPublicKey());
        freezeKey(*app, a2Key);
        checkAccessesFrozenKey(payment(a2.getPublicKey(), 100));
    }

    SECTION("PathPaymentStrictReceive destination trustline frozen")
    {
        freezeKey(*app, a2UsdTL);
        checkAccessesFrozenKey(
            pathPayment(a2.getPublicKey(), usd, 100, usd, 100, {}));
    }

    SECTION("PathPaymentStrictReceive native destination account frozen")
    {
        auto a2Key = accountKey(a2.getPublicKey());
        freezeKey(*app, a2Key);
        checkAccessesFrozenKey(
            pathPayment(a2.getPublicKey(), xlm, 100, xlm, 100, {}));
    }

    SECTION("PathPaymentStrictSend destination trustline frozen")
    {
        freezeKey(*app, a2UsdTL);
        checkAccessesFrozenKey(
            pathPaymentStrictSend(a2.getPublicKey(), usd, 100, usd, 90, {}));
    }

    SECTION("PathPaymentStrictSend native destination account frozen")
    {
        auto a2Key = accountKey(a2.getPublicKey());
        freezeKey(*app, a2Key);
        checkAccessesFrozenKey(
            pathPaymentStrictSend(a2.getPublicKey(), xlm, 100, xlm, 100, {}));
    }

    SECTION("AllowTrustOp trustor trustline frozen")
    {
        freezeKey(*app, a2UsdTL);
        checkAccessesFrozenKeyWithSource(
            issuer, allowTrust(a2.getPublicKey(), usd, AUTHORIZED_FLAG));
    }

    SECTION("SetTrustLineFlagsOp trustor trustline frozen")
    {
        // Issuer must have AUTH_REVOCABLE flag to set trustline flags
        issuer.setOptions(setFlags(AUTH_REVOCABLE_FLAG));
        freezeKey(*app, a2UsdTL);
        checkAccessesFrozenKeyWithSource(
            issuer, setTrustLineFlags(a2.getPublicKey(), usd,
                                      setTrustLineFlags(AUTHORIZED_FLAG)));
    }

    SECTION("ClawbackOp from trustline frozen")
    {
        // Issuer needs clawback enabled
        auto clawbackIssuer = root->create(
            "clawIssuer", lm.getLastMinBalance(10) + 10 * lm.getLastTxFee());
        clawbackIssuer.setOptions(
            setFlags(AUTH_REVOCABLE_FLAG | AUTH_CLAWBACK_ENABLED_FLAG));
        auto clawAsset = makeAsset(clawbackIssuer, "CLW");
        a2.changeTrust(clawAsset, INT64_MAX);
        clawbackIssuer.pay(a2, clawAsset, 1000);

        auto a2ClwTL = trustlineKey(a2.getPublicKey(), clawAsset);
        freezeKey(*app, a2ClwTL);

        checkAccessesFrozenKeyWithSource(
            clawbackIssuer, clawback(a2.getPublicKey(), clawAsset, 100));
    }

    SECTION("RevokeSponsorshipOp ledger key frozen")
    {
        auto a2Key = accountKey(a2.getPublicKey());
        freezeKey(*app, a2Key);
        checkAccessesFrozenKey(revokeSponsorship(a2Key));
    }

    SECTION("RevokeSponsorshipOp signer account frozen")
    {
        auto a2Key = accountKey(a2.getPublicKey());
        freezeKey(*app, a2Key);

        SignerKey signerKey;
        signerKey.type(SIGNER_KEY_TYPE_ED25519);
        signerKey.ed25519() = a1.getPublicKey().ed25519();

        checkAccessesFrozenKey(revokeSponsorship(a2.getPublicKey(), signerKey));
    }

    SECTION("AccountMergeOp destination account frozen")
    {
        auto a2Key = accountKey(a2.getPublicKey());
        freezeKey(*app, a2Key);
        checkAccessesFrozenKey(accountMerge(a2.getPublicKey()));
    }

    SECTION("CreateAccountOp destination account frozen")
    {
        AccountID destKey{};
        freezeKey(*app, accountKey(destKey));
        checkAccessesFrozenKey(createAccount(destKey, 10000000));
    }
}

TEST_CASE("frozen ledger keys apply time validation",
          "[frozenledgerkeys][tx][gen-lcm]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    auto root = app->getRoot();
    auto const& lm = app->getLedgerManager();

    auto issuer = root->create("issuer", lm.getLastMinBalance(10) +
                                             10 * lm.getLastTxFee());
    auto a1 =
        root->create("A1", lm.getLastMinBalance(10) + 10 * lm.getLastTxFee());
    auto a2 =
        root->create("A2", lm.getLastMinBalance(10) + 10 * lm.getLastTxFee());

    auto usd = makeAsset(issuer, "USD");
    auto eur = makeAsset(issuer, "EUR");

    a1.changeTrust(usd, INT64_MAX);
    a1.changeTrust(eur, INT64_MAX);
    a2.changeTrust(usd, INT64_MAX);
    a2.changeTrust(eur, INT64_MAX);
    issuer.pay(a1, usd, 10000);
    issuer.pay(a1, eur, 10000);
    issuer.pay(a2, usd, 10000);

    SECTION("claim claimable balance trustline frozen")
    {
        Claimant claimant;
        claimant.v0().destination = a2.getPublicKey();
        claimant.v0().predicate.type(CLAIM_PREDICATE_UNCONDITIONAL);
        a1.createClaimableBalance(usd, 100, {claimant});
        auto cbID = a1.getBalanceID(0);

        auto a2UsdTL = trustlineKey(a2.getPublicKey(), usd);
        freezeKey(*app, a2UsdTL);

        auto tx = transactionFromOperations(*app, a2.getSecretKey(),
                                            a2.nextSequenceNumber(),
                                            {claimClaimableBalance(cbID)});
        auto r = closeLedger(*app, {tx});
        checkTx(0, r, txFAILED);
        REQUIRE(r.results[0]
                    .result.result.results()[0]
                    .tr()
                    .claimClaimableBalanceResult()
                    .code() == CLAIM_CLAIMABLE_BALANCE_TRUSTLINE_FROZEN);
    }

    auto checkLPTrustlineFrozen = [&](LedgerKey const& frozenTrustline,
                                      bool isDeposit) {
        auto share =
            makeChangeTrustAssetPoolShare(eur, usd, LIQUIDITY_POOL_FEE_V18);
        auto poolID = xdrSha256(share.liquidityPool());
        a1.changeTrust(share, INT64_MAX);
        a1.liquidityPoolDeposit(poolID, 1000, 1000, Price{1, 1}, Price{1, 1});

        auto op = isDeposit ? liquidityPoolDeposit(poolID, 100, 100,
                                                   Price{1, 1}, Price{1, 1})
                            : liquidityPoolWithdraw(poolID, 100, 0, 0);

        freezeKey(*app, frozenTrustline);
        auto tx = transactionFromOperations(*app, a1.getSecretKey(),
                                            a1.nextSequenceNumber(), {op});
        auto r = closeLedger(*app, {tx});
        checkTx(0, r, txFAILED);

        auto const& opResult = r.results[0].result.result.results()[0].tr();
        if (isDeposit)
        {
            REQUIRE(opResult.liquidityPoolDepositResult().code() ==
                    LIQUIDITY_POOL_DEPOSIT_TRUSTLINE_FROZEN);
        }
        else
        {
            REQUIRE(opResult.liquidityPoolWithdrawResult().code() ==
                    LIQUIDITY_POOL_WITHDRAW_TRUSTLINE_FROZEN);
        }
    };
    SECTION("liquidity pool deposit assetA trustline frozen")
    {
        auto a1EurTL = trustlineKey(a1.getPublicKey(), eur);
        checkLPTrustlineFrozen(a1EurTL, true);
    }

    SECTION("liquidity pool deposit assetB trustline frozen")
    {
        auto a1UsdTL = trustlineKey(a1.getPublicKey(), usd);
        checkLPTrustlineFrozen(a1UsdTL, true);
    }

    SECTION("liquidity pool withdraw assetA trustline frozen")
    {
        auto a1EurTL = trustlineKey(a1.getPublicKey(), eur);
        checkLPTrustlineFrozen(a1EurTL, false);
    }

    SECTION("liquidity pool withdraw assetB trustline frozen")
    {
        auto a1UsdTL = trustlineKey(a1.getPublicKey(), usd);
        checkLPTrustlineFrozen(a1UsdTL, false);
    }
}

TEST_CASE("sponsorship can be removed with frozen sponsor",
          "[frozenledgerkeys][tx][sponsorship][gen-lcm]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    auto root = app->getRoot();
    auto const& lm = app->getLedgerManager();

    auto minBalance = lm.getLastMinBalance(10) + 20 * lm.getLastTxFee();
    auto frozenSponsor = root->create("frozenSponsor", minBalance);
    auto sponsored = root->create("sponsored", minBalance);
    auto issuer = root->create("issuer", minBalance);

    auto usd = makeAsset(issuer, "USD");

    auto getNumSponsoringFor = [&](TestAccount const& account) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto ltxe = loadAccount(ltx, account.getPublicKey(), true);
        REQUIRE(ltxe);
        return getNumSponsoring(ltxe.current());
    };

    auto getNumSponsoredFor = [&](TestAccount const& account) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto ltxe = loadAccount(ltx, account.getPublicKey(), true);
        REQUIRE(ltxe);
        return getNumSponsored(ltxe.current());
    };

    auto createSponsoredTLTx = transactionFrameFromOps(
        app->getNetworkID(), frozenSponsor,
        {frozenSponsor.op(beginSponsoringFutureReserves(sponsored)),
         sponsored.op(changeTrust(usd, INT64_MAX)),
         sponsored.op(endSponsoringFutureReserves())},
        {sponsored});
    auto createRes = closeLedger(*app, {createSponsoredTLTx});
    checkTx(0, createRes, txSUCCESS);

    REQUIRE(sponsored.hasTrustLine(usd));
    REQUIRE(getNumSponsoringFor(frozenSponsor) == 1);
    REQUIRE(getNumSponsoredFor(sponsored) == 1);

    freezeKey(*app, accountKey(frozenSponsor.getPublicKey()));

    auto removeTx = transactionFromOperations(*app, sponsored.getSecretKey(),
                                              sponsored.nextSequenceNumber(),
                                              {changeTrust(usd, 0)});
    auto removeRes = closeLedger(*app, {removeTx});
    checkTx(0, removeRes, txSUCCESS);

    REQUIRE(!sponsored.hasTrustLine(usd));
    REQUIRE(getNumSponsoringFor(frozenSponsor) == 0);
    REQUIRE(getNumSponsoredFor(sponsored) == 0);
}

TEST_CASE("deauth removes offers on frozen account",
          "[frozenledgerkeys][tx][gen-lcm]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    auto root = app->getRoot();
    auto const& lm = app->getLedgerManager();

    auto minBalance = lm.getLastMinBalance(10) + 20 * lm.getLastTxFee();
    auto issuer = root->create("issuer", minBalance);
    issuer.setOptions(setFlags(AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG));
    auto frozenAcct = root->create("frozenAcct", minBalance);

    auto usd = makeAsset(issuer, "USD");
    auto xlm = makeNativeAsset();

    frozenAcct.changeTrust(usd, INT64_MAX);
    issuer.allowTrust(usd, frozenAcct.getPublicKey(), AUTHORIZED_FLAG);
    issuer.pay(frozenAcct, usd, 5000);

    auto offerID = frozenAcct.manageOffer(0, usd, xlm, Price{1, 1}, 1000);

    // Verify the offer exists and selling liabilities are set.
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        REQUIRE(loadOffer(ltx, frozenAcct.getPublicKey(), offerID));
        auto tl = loadTrustLine(ltx, frozenAcct.getPublicKey(), usd);
        REQUIRE(tl);
        REQUIRE(tl.getSellingLiabilities(ltx.loadHeader()) > 0);
    }

    // Freeze the ACCOUNT (not the trustline).
    freezeKey(*app, accountKey(frozenAcct.getPublicKey()));

    // The issuer deauthorizes the (non-frozen) trustline. This triggers offer
    // removal which releases liabilities on the frozen account — an allowed
    // modification per CAP-77.
    auto deauthTx = transactionFromOperations(
        *app, issuer.getSecretKey(), issuer.nextSequenceNumber(),
        {setTrustLineFlags(frozenAcct.getPublicKey(), usd,
                           clearTrustLineFlags(AUTHORIZED_FLAG))});
    auto r = closeLedger(*app, {deauthTx});
    checkTx(0, r, txSUCCESS);

    // The offer should be removed and liabilities released, even though the
    // account is frozen.
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        REQUIRE(!loadOffer(ltx, frozenAcct.getPublicKey(), offerID));
        auto tl = loadTrustLine(ltx, frozenAcct.getPublicKey(), usd);
        REQUIRE(tl);
        REQUIRE(tl.getSellingLiabilities(ltx.loadHeader()) == 0);
    }
}

// Below are the helper types/functions for the DEX tests.

enum class DexOfferOpKind
{
    MANAGE_SELL,
    MANAGE_BUY,
    CREATE_PASSIVE_SELL
};

enum class DexPathOpKind
{
    STRICT_RECEIVE,
    STRICT_SEND
};

enum class DexAssetPairKind
{
    CREDIT_NATIVE,
    NATIVE_CREDIT,
    CREDIT_CREDIT
};

enum class FrozenSide
{
    SELLING,
    BUYING,
    BOTH
};

char const*
toString(DexOfferOpKind opKind)
{
    switch (opKind)
    {
    case DexOfferOpKind::MANAGE_SELL:
        return "manage-sell";
    case DexOfferOpKind::MANAGE_BUY:
        return "manage-buy";
    case DexOfferOpKind::CREATE_PASSIVE_SELL:
        return "create-passive-sell";
    }
    throw std::runtime_error("unexpected dex offer op kind");
}

char const*
toString(DexPathOpKind opKind)
{
    switch (opKind)
    {
    case DexPathOpKind::STRICT_RECEIVE:
        return "path-strict-receive";
    case DexPathOpKind::STRICT_SEND:
        return "path-strict-send";
    }
    throw std::runtime_error("unexpected dex path op kind");
}

char const*
toString(DexAssetPairKind pairKind)
{
    switch (pairKind)
    {
    case DexAssetPairKind::CREDIT_NATIVE:
        return "credit-native";
    case DexAssetPairKind::NATIVE_CREDIT:
        return "native-credit";
    case DexAssetPairKind::CREDIT_CREDIT:
        return "credit-credit";
    }
    throw std::runtime_error("unexpected dex asset pair kind");
}

char const*
toString(FrozenSide side)
{
    switch (side)
    {
    case FrozenSide::SELLING:
        return "selling-frozen";
    case FrozenSide::BUYING:
        return "buying-frozen";
    case FrozenSide::BOTH:
        return "both-frozen";
    }
    throw std::runtime_error("unexpected frozen side");
}

std::vector<Asset>
getFrozenAssets(Asset const& selling, Asset const& buying, FrozenSide side)
{
    switch (side)
    {
    case FrozenSide::SELLING:
        return {selling};
    case FrozenSide::BUYING:
        return {buying};
    case FrozenSide::BOTH:
        return {selling, buying};
    }
    throw std::runtime_error("unexpected frozen side");
}

struct DexAssetState
{
    int64_t balance;
    Liabilities liabilities;
};

DexAssetState
loadDexAssetState(Application& app, TestAccount const& account,
                  Asset const& asset)
{
    DexAssetState res;
    res.liabilities.selling = 0;
    res.liabilities.buying = 0;
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto header = ltx.loadHeader();
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        auto acc = stellar::loadAccount(ltx, account.getPublicKey());
        REQUIRE(acc);
        res.balance = acc.current().data.account().balance;
        res.liabilities.selling = getSellingLiabilities(header, acc);
        res.liabilities.buying = getBuyingLiabilities(header, acc);
    }
    else
    {
        auto tl = stellar::loadTrustLine(ltx, account.getPublicKey(), asset);
        REQUIRE(tl);
        res.balance = tl.getBalance();
        res.liabilities.selling = tl.getSellingLiabilities(header);
        res.liabilities.buying = tl.getBuyingLiabilities(header);
    }
    return res;
}

bool
offerExists(Application& app, TestAccount const& seller, int64_t offerID)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    return static_cast<bool>(loadOffer(ltx, seller.getPublicKey(), offerID));
}

LedgerKey
frozenKeyForAsset(TestAccount const& account, Asset const& asset)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        return accountKey(account.getPublicKey());
    }
    return trustlineKey(account.getPublicKey(), asset);
}

Operation
makeOfferDexOp(DexOfferOpKind opKind, Asset const& selling, Asset const& buying)
{
    switch (opKind)
    {
    case DexOfferOpKind::MANAGE_SELL:
        return manageOffer(0, selling, buying, Price{1, 100}, 300);
    case DexOfferOpKind::MANAGE_BUY:
        return manageBuyOffer(0, selling, buying, Price{2, 1}, 300);
    case DexOfferOpKind::CREATE_PASSIVE_SELL:
        return createPassiveOffer(selling, buying, Price{1, 100}, 300);
    }
    throw std::runtime_error("unexpected dex offer op kind");
}

template <typename T>
size_t
claimedOfferCount(T const& tr, DexOfferOpKind opKind)
{
    switch (opKind)
    {
    case DexOfferOpKind::MANAGE_SELL:
        return tr.manageSellOfferResult().success().offersClaimed.size();
    case DexOfferOpKind::MANAGE_BUY:
        return tr.manageBuyOfferResult().success().offersClaimed.size();
    case DexOfferOpKind::CREATE_PASSIVE_SELL:
        return tr.manageSellOfferResult().success().offersClaimed.size();
    }
    throw std::runtime_error("unexpected dex offer op kind");
}

template <typename T>
size_t
claimedOfferCount(T const& tr, DexPathOpKind opKind)
{
    if (opKind == DexPathOpKind::STRICT_RECEIVE)
    {
        return tr.pathPaymentStrictReceiveResult().success().offers.size();
    }
    return tr.pathPaymentStrictSendResult().success().offers.size();
}

TEST_CASE("frozen ledger keys DEX offer operations",
          "[frozenledgerkeys][tx][offers][gen-lcm]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    auto root = app->getRoot();
    auto const& lm = app->getLedgerManager();

    auto minBalance = lm.getLastMinBalance(20) + 100 * lm.getLastTxFee();
    auto issuerA = root->create("issuerA", minBalance);
    auto issuerB = root->create("issuerB", minBalance);
    auto frozenSeller1 = root->create("frozenSeller1", minBalance);
    auto frozenSeller2 = root->create("frozenSeller2", minBalance);
    auto activeSeller = root->create("activeSeller", minBalance);
    auto buyer = root->create("buyer", minBalance);

    auto xlm = makeNativeAsset();
    auto usd = makeAsset(issuerA, "USD");
    auto eur = makeAsset(issuerB, "EUR");

    auto ensureTrust = [&](TestAccount& account, Asset const& asset) {
        if (asset.type() != ASSET_TYPE_NATIVE && !account.hasTrustLine(asset))
        {
            account.changeTrust(asset, INT64_MAX);
        }
    };

    auto fundAsset = [&](TestAccount& account, Asset const& asset,
                         int64_t amount) {
        if (asset.type() == ASSET_TYPE_NATIVE)
        {
            root->pay(account, amount);
        }
        else
        {
            ensureTrust(account, asset);
            if (asset == usd)
            {
                issuerA.pay(account, asset, amount);
            }
            else
            {
                REQUIRE(asset == eur);
                issuerB.pay(account, asset, amount);
            }
        }
    };

    auto opKind =
        GENERATE(DexOfferOpKind::MANAGE_SELL, DexOfferOpKind::MANAGE_BUY,
                 DexOfferOpKind::CREATE_PASSIVE_SELL);
    auto pairKind = GENERATE(DexAssetPairKind::CREDIT_NATIVE,
                             DexAssetPairKind::NATIVE_CREDIT,
                             DexAssetPairKind::CREDIT_CREDIT);
    auto side =
        GENERATE(FrozenSide::SELLING, FrozenSide::BUYING, FrozenSide::BOTH);
    // Generate two offers: first is always frozen, second is active or frozen
    // based on this flag.
    auto secondOfferIsActive = GENERATE(true, false);

    DYNAMIC_SECTION(fmt::format(
        "{} [{}][{}][{}]",
        secondOfferIsActive ? "second offer active" : "second offer frozen",
        toString(opKind), toString(pairKind), toString(side)))
    {
        Asset makerSelling;
        Asset makerBuying;
        switch (pairKind)
        {
        case DexAssetPairKind::CREDIT_NATIVE:
            makerSelling = usd;
            makerBuying = xlm;
            break;
        case DexAssetPairKind::NATIVE_CREDIT:
            makerSelling = xlm;
            makerBuying = usd;
            break;
        case DexAssetPairKind::CREDIT_CREDIT:
            makerSelling = usd;
            makerBuying = eur;
            break;
        }

        auto prepareMaker = [&](TestAccount& account) {
            ensureTrust(account, makerSelling);
            ensureTrust(account, makerBuying);
            fundAsset(account, makerSelling, 10'000);
        };

        prepareMaker(frozenSeller1);
        prepareMaker(frozenSeller2);
        prepareMaker(activeSeller);
        ensureTrust(buyer, makerSelling);
        ensureTrust(buyer, makerBuying);
        fundAsset(buyer, makerBuying, 10'000);

        auto frozenOffer1 = frozenSeller1.manageOffer(
            0, makerSelling, makerBuying, Price{1, 1}, 1'000);
        auto frozenOffer2 = int64_t{0};
        auto activeOffer = int64_t{0};
        if (secondOfferIsActive)
        {
            activeOffer = activeSeller.manageOffer(0, makerSelling, makerBuying,
                                                   Price{2, 1}, 1'000);
        }
        else
        {
            frozenOffer2 = frozenSeller2.manageOffer(
                0, makerSelling, makerBuying, Price{2, 1}, 1'000);
        }

        auto frozenAssets = getFrozenAssets(makerSelling, makerBuying, side);
        struct FrozenState
        {
            TestAccount* seller;
            Asset asset;
            DexAssetState pre;
        };
        std::vector<FrozenState> frozenStates;

        auto freezeForSeller = [&](TestAccount& seller) {
            for (auto const& asset : frozenAssets)
            {
                freezeKey(*app, frozenKeyForAsset(seller, asset));
                auto pre = loadDexAssetState(*app, seller, asset);
                REQUIRE(pre.liabilities.selling + pre.liabilities.buying > 0);
                frozenStates.emplace_back(FrozenState{&seller, asset, pre});
            }
        };

        freezeForSeller(frozenSeller1);
        if (!secondOfferIsActive)
        {
            freezeForSeller(frozenSeller2);
        }

        auto frozen1SellingPre =
            loadDexAssetState(*app, frozenSeller1, makerSelling).balance;
        auto frozen1BuyingPre =
            loadDexAssetState(*app, frozenSeller1, makerBuying).balance;
        auto frozen2SellingPre =
            loadDexAssetState(*app, frozenSeller2, makerSelling).balance;
        auto frozen2BuyingPre =
            loadDexAssetState(*app, frozenSeller2, makerBuying).balance;
        auto activeSellingPre =
            loadDexAssetState(*app, activeSeller, makerSelling).balance;
        auto activeBuyingPre =
            loadDexAssetState(*app, activeSeller, makerBuying).balance;
        auto buyerSellingPre =
            loadDexAssetState(*app, buyer, makerBuying).balance;
        auto buyerBuyingPre =
            loadDexAssetState(*app, buyer, makerSelling).balance;

        auto op = makeOfferDexOp(opKind, makerBuying, makerSelling);
        op.sourceAccount.activate() = toMuxedAccount(buyer.getPublicKey());
        // Pay for transaction from the root account in order to have clean XLM
        // balance changes.
        auto tx = transactionFromOperations(*app, root->getSecretKey(),
                                            root->nextSequenceNumber(), {op});
        tx->addSignature(buyer.getSecretKey());
        auto r = closeLedger(*app, {tx});
        checkTx(0, r, txSUCCESS);

        auto const& tr = r.results[0].result.result.results()[0].tr();
        if (secondOfferIsActive)
        {
            REQUIRE(claimedOfferCount(tr, opKind) > 0);
        }
        else
        {
            REQUIRE(claimedOfferCount(tr, opKind) == 0);
        }

        REQUIRE(!offerExists(*app, frozenSeller1, frozenOffer1));
        if (secondOfferIsActive)
        {
            REQUIRE(offerExists(*app, activeSeller, activeOffer));
        }
        else
        {
            REQUIRE(!offerExists(*app, frozenSeller2, frozenOffer2));
        }

        auto frozen1SellingPost =
            loadDexAssetState(*app, frozenSeller1, makerSelling).balance;
        auto frozen1BuyingPost =
            loadDexAssetState(*app, frozenSeller1, makerBuying).balance;
        auto frozen2SellingPost =
            loadDexAssetState(*app, frozenSeller2, makerSelling).balance;
        auto frozen2BuyingPost =
            loadDexAssetState(*app, frozenSeller2, makerBuying).balance;
        auto activeSellingPost =
            loadDexAssetState(*app, activeSeller, makerSelling).balance;
        auto activeBuyingPost =
            loadDexAssetState(*app, activeSeller, makerBuying).balance;
        auto buyerSellingPost =
            loadDexAssetState(*app, buyer, makerBuying).balance;
        auto buyerBuyingPost =
            loadDexAssetState(*app, buyer, makerSelling).balance;

        REQUIRE(frozen1SellingPost == frozen1SellingPre);
        REQUIRE(frozen1BuyingPost == frozen1BuyingPre);
        // With second offer active, exactly one unfrozen offer at price 2:1 is
        // crossed. Math:
        // - manage-buy buys 300 units -> pays 600 units.
        // - manage-sell/create-passive sell 300 units -> receive 150 units.
        int64_t expectedActiveSold = 0;
        int64_t expectedActiveBought = 0;
        switch (opKind)
        {
        case DexOfferOpKind::MANAGE_BUY:
            expectedActiveSold = 300;
            expectedActiveBought = 600;
            break;
        case DexOfferOpKind::MANAGE_SELL:
        case DexOfferOpKind::CREATE_PASSIVE_SELL:
            expectedActiveSold = 150;
            expectedActiveBought = 300;
            break;
        }

        if (secondOfferIsActive)
        {
            REQUIRE(activeSellingPre - activeSellingPost == expectedActiveSold);
            REQUIRE(activeBuyingPost - activeBuyingPre == expectedActiveBought);
            REQUIRE(buyerBuyingPost - buyerBuyingPre == expectedActiveSold);
            REQUIRE(buyerSellingPre - buyerSellingPost == expectedActiveBought);
        }
        else
        {
            REQUIRE(activeSellingPost == activeSellingPre);
            REQUIRE(activeBuyingPost == activeBuyingPre);
            REQUIRE(frozen2SellingPost == frozen2SellingPre);
            REQUIRE(frozen2BuyingPost == frozen2BuyingPre);
            REQUIRE(buyerBuyingPost == buyerBuyingPre);
            REQUIRE(buyerSellingPost == buyerSellingPre);
        }

        for (auto const& frozenState : frozenStates)
        {
            auto post =
                loadDexAssetState(*app, *frozenState.seller, frozenState.asset);
            REQUIRE(post.balance == frozenState.pre.balance);
            REQUIRE(post.liabilities.selling == 0);
            REQUIRE(post.liabilities.buying == 0);
        }
    }
}

TEST_CASE("frozen ledger keys DEX path payments",
          "[frozenledgerkeys][tx][offers][gen-lcm]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);
    auto root = app->getRoot();
    auto const& lm = app->getLedgerManager();

    auto minBalance = lm.getLastMinBalance(20) + 100 * lm.getLastTxFee();
    auto issuerB = root->create("issuerB", minBalance);
    auto issuerC = root->create("issuerC", minBalance);

    auto payer = root->create("payer", minBalance);
    auto destination = root->create("destination", minBalance);

    auto l1Best = root->create("l1Best", minBalance);
    auto l1Fallback = root->create("l1Fallback", minBalance);
    auto l2Best = root->create("l2Best", minBalance);
    auto l2Fallback = root->create("l2Fallback", minBalance);
    auto l3Best = root->create("l3Best", minBalance);
    auto l3Fallback = root->create("l3Fallback", minBalance);

    auto a = makeNativeAsset();
    auto b = makeAsset(issuerB, "USD");
    auto c = makeAsset(issuerC, "EUR");

    auto ensureTrust = [&](TestAccount& account, Asset const& asset) {
        if (asset.type() != ASSET_TYPE_NATIVE && !account.hasTrustLine(asset))
        {
            account.changeTrust(asset, INT64_MAX);
        }
    };

    auto fundAsset = [&](TestAccount& account, Asset const& asset,
                         int64_t amount) {
        if (asset.type() == ASSET_TYPE_NATIVE)
        {
            root->pay(account, amount);
        }
        else
        {
            ensureTrust(account, asset);
            if (asset == b)
            {
                issuerB.pay(account, asset, amount);
            }
            else
            {
                REQUIRE(asset == c);
                issuerC.pay(account, asset, amount);
            }
        }
    };

    struct LegState
    {
        TestAccount* best;
        TestAccount* fallback;
        Asset selling;
        Asset buying;
        int64_t bestOffer;
        int64_t fallbackOffer;
    };

    std::array<LegState, 3> legs = {LegState{&l1Best, &l1Fallback, b, a, 0, 0},
                                    LegState{&l2Best, &l2Fallback, c, b, 0, 0},
                                    LegState{&l3Best, &l3Fallback, a, c, 0, 0}};

    auto prepareMaker = [&](TestAccount& account, Asset const& selling,
                            Asset const& buying) {
        ensureTrust(account, selling);
        ensureTrust(account, buying);
        fundAsset(account, selling, 10'000);
    };

    for (auto& leg : legs)
    {
        prepareMaker(*leg.best, leg.selling, leg.buying);
        prepareMaker(*leg.fallback, leg.selling, leg.buying);
        leg.bestOffer = leg.best->manageOffer(0, leg.selling, leg.buying,
                                              Price{1, 1}, 1'000);
        leg.fallbackOffer = leg.fallback->manageOffer(
            0, leg.selling, leg.buying, Price{2, 1}, 1'000);
    }

    auto makePathDexOp = [&](DexPathOpKind opKind, PublicKey const& destination,
                             Asset const& a, Asset const& b, Asset const& c) {
        if (opKind == DexPathOpKind::STRICT_RECEIVE)
        {
            return pathPayment(destination, a, 10'000, a, 200, {b, c});
        }
        return pathPaymentStrictSend(destination, a, 400, a, 1, {b, c});
    };

    auto opKind =
        GENERATE(DexPathOpKind::STRICT_RECEIVE, DexPathOpKind::STRICT_SEND);
    auto side =
        GENERATE(FrozenSide::SELLING, FrozenSide::BUYING, FrozenSide::BOTH);
    auto legToFreeze = GENERATE(0, 1, 2);

    DYNAMIC_SECTION(fmt::format("A->B->C->A [{}][leg={}][{}]", toString(opKind),
                                legToFreeze, toString(side)))
    {
        auto& targetLeg = legs[legToFreeze];
        auto frozenAssets =
            getFrozenAssets(targetLeg.selling, targetLeg.buying, side);

        struct FrozenState
        {
            Asset asset;
            DexAssetState pre;
        };
        std::vector<FrozenState> frozenStates;
        for (auto const& asset : frozenAssets)
        {
            freezeKey(*app, frozenKeyForAsset(*targetLeg.best, asset));
            auto pre = loadDexAssetState(*app, *targetLeg.best, asset);
            REQUIRE(pre.liabilities.selling + pre.liabilities.buying > 0);
            frozenStates.emplace_back(FrozenState{asset, pre});
        }

        auto bestSellingPre =
            loadDexAssetState(*app, *targetLeg.best, targetLeg.selling).balance;
        auto bestBuyingPre =
            loadDexAssetState(*app, *targetLeg.best, targetLeg.buying).balance;
        auto fallbackSellingPre =
            loadDexAssetState(*app, *targetLeg.fallback, targetLeg.selling)
                .balance;
        auto fallbackBuyingPre =
            loadDexAssetState(*app, *targetLeg.fallback, targetLeg.buying)
                .balance;
        auto payerPre = loadDexAssetState(*app, payer, a).balance;
        auto destinationPre = loadDexAssetState(*app, destination, a).balance;

        auto op = makePathDexOp(opKind, destination.getPublicKey(), a, b, c);
        op.sourceAccount.activate() = toMuxedAccount(payer.getPublicKey());

        auto tx = transactionFromOperations(*app, root->getSecretKey(),
                                            root->nextSequenceNumber(), {op});
        tx->addSignature(payer.getSecretKey());

        auto r = closeLedger(*app, {tx});
        checkTx(0, r, txSUCCESS);

        auto const& tr = r.results[0].result.result.results()[0].tr();
        REQUIRE(claimedOfferCount(tr, opKind) > 0);

        REQUIRE(!offerExists(*app, *targetLeg.best, targetLeg.bestOffer));
        REQUIRE(
            offerExists(*app, *targetLeg.fallback, targetLeg.fallbackOffer));

        auto bestSellingPost =
            loadDexAssetState(*app, *targetLeg.best, targetLeg.selling).balance;
        auto bestBuyingPost =
            loadDexAssetState(*app, *targetLeg.best, targetLeg.buying).balance;
        auto fallbackSellingPost =
            loadDexAssetState(*app, *targetLeg.fallback, targetLeg.selling)
                .balance;
        auto fallbackBuyingPost =
            loadDexAssetState(*app, *targetLeg.fallback, targetLeg.buying)
                .balance;
        auto payerPost = loadDexAssetState(*app, payer, a).balance;
        auto destinationPost = loadDexAssetState(*app, destination, a).balance;

        // Exactly one leg uses the fallback 2:1 offer, other two legs use 1:1.
        // For both strict-receive (dest=200 A) and strict-send (source=400 A):
        // - fallback seller sells 200 of its selling asset and receives 400 of
        //   its buying asset,
        // - payer sends 400 A,
        // - destination receives 200 A.
        constexpr int64_t expectedFallbackSold = 200;
        constexpr int64_t expectedFallbackBought = 400;
        constexpr int64_t expectedPayerSent = 400;
        constexpr int64_t expectedDestinationReceived = 200;

        REQUIRE(bestSellingPost == bestSellingPre);
        REQUIRE(bestBuyingPost == bestBuyingPre);
        REQUIRE(fallbackSellingPre - fallbackSellingPost ==
                expectedFallbackSold);
        REQUIRE(fallbackBuyingPost - fallbackBuyingPre ==
                expectedFallbackBought);
        REQUIRE(payerPre - payerPost == expectedPayerSent);
        REQUIRE(destinationPost - destinationPre ==
                expectedDestinationReceived);

        for (auto const& frozenState : frozenStates)
        {
            auto post =
                loadDexAssetState(*app, *targetLeg.best, frozenState.asset);
            REQUIRE(post.balance == frozenState.pre.balance);
            REQUIRE(post.liabilities.selling == 0);
            REQUIRE(post.liabilities.buying == 0);
        }
    }
}

} // namespace
} // namespace stellar
