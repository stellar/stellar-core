// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// This file is dedicated to a set of complex tests that generate random
// transaction sets, apply them to the ledger with different parallel execution
// schedules, and ensure that the results never diverge (outside of a narrow
// set of the expected differences).
#include "test/Catch2.h"

#include <limits>

#include "bucket/BucketManager.h"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "transactions/OperationFrame.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "util/UnorderedSet.h"

using namespace stellar;
using namespace stellar::txtest;

namespace
{

// The constants below define the general test generation parameters.

// Min/max number fraction of the entries that are uploaded to ledger before
// executing the test transactions.
double const MIN_PREUPLOADED_ENTRIES_FRACTION = 0.1;
double const MAX_PREUPLOADED_ENTRIES_FRACTION = 0.8;
// Min/max number fraction of the preuploaed entries that are expired.
double const MIN_EXPIRED_ENTRY_FRACTION = 0.0;
double const MAX_EXPIRED_ENTRY_FRACTION = 0.3;

// Average number of additional keys added to Soroban transaction footprints.
double const MEAN_ADDITIONAL_FOOTPRINT_KEYS = 10.0;
// Maximum number of additional keys added to Soroban transaction footprints.
int const MAX_ADDITONAL_FOOTPRINT_KEYS = 30;

// Min/max fraction of the keys added to read-only footprints (the rest are
// added to the read-write footprints).
double const MIN_READ_ONLY_FOOTPRINT_FRACTION = 0.05;
double const MAX_READ_ONLY_FOOTPRINT_FRACTION = 0.95;

// Maximum number of stages to use for applying transactions in parallel.
int const MAX_STAGE_COUNT = 4;
// The number of independent key clusters to generate. This also limits the
// maximum number of apply threads we can use.
int const KEY_CLUSTER_COUNT = 8;

// Initial balance of the test trustlines.
int const TRUSTLINE_BALANCE = 1'000'000;
// The initial TTL of the persistent test entries.
int const MIN_PERSISTENT_TTL = 10;

// TTL extensions for the persistent entries that are expired but still in the
// live state during the test execution.
int const TTL_EXTENSION_FOR_EXPIRED_IN_LIVE_STATE = 30;

// Number of test runs to perform for each scenario.
int const TEST_RUN_COUNT = 25;

using ResultType = std::tuple<
    std::vector<TransactionFrameBaseConstPtr>, TransactionResultSet,
    LedgerCloseMetaFrame,
    std::vector<std::pair<LedgerKey, std::pair<std::optional<LedgerEntry>,
                                               std::optional<LedgerEntry>>>>>;

struct TestConfig
{
    int randomWasmCount{};
    int contractDataKeyCount{};
    int trustlineCount{};
    int classicTxCount{};
    int restoreTxCount{};
    int extendTxCount{};
    int testScenarioCount{};
};

UnorderedMap<LedgerKey, std::vector<LedgerEntryChange>>
groupLedgerEntryChanges(LedgerEntryChanges const& changes)
{
    UnorderedMap<LedgerKey, std::vector<LedgerEntryChange>> changesByKey;
    for (auto const& change : changes)
    {
        LedgerKey key;
        switch (change.type())
        {
        case LEDGER_ENTRY_CREATED:
            key = LedgerEntryKey(change.created());
            break;
        case LEDGER_ENTRY_STATE:
            key = LedgerEntryKey(change.state());
            break;
        case LEDGER_ENTRY_UPDATED:
            key = LedgerEntryKey(change.updated());
            break;
        case LEDGER_ENTRY_RESTORED:
            key = LedgerEntryKey(change.restored());
            break;
        case LEDGER_ENTRY_REMOVED:
            key = change.removed();
            break;
        }
        changesByKey[key].push_back(change);
    }
    return changesByKey;
}

void
expectTTLBump(std::vector<LedgerEntryChange> const& changes)
{
    REQUIRE(changes.size() == 2);
    REQUIRE(changes[0].type() == LEDGER_ENTRY_STATE);
    REQUIRE(changes[1].type() == LEDGER_ENTRY_UPDATED);
    REQUIRE(changes[0].state().data.ttl() < changes[1].updated().data.ttl());
}

// Compares the results from two runs of `applyTestTransactions`.
// We only expect `res1` to ever be from protocol 22.
// `singleStage` indicates that both results come from a single stage runs
// (with any number of clusters).
void
compareResults(bool res1IsP22, bool singleStage, ResultType const& res1,
               ResultType const& res2, uint32_t hotArchiveEntryCreatedLedger)
{
    // Fees are different between the protocols. Since we observe
    // TTLs at the stage boundaries, we can only expect exactly the
    // same changes (and thus fees) when there is only a single
    // stage.
    bool expectExactChanges = !res1IsP22 && singleStage;

    auto const& [txs1, results1, lcm1, entries1] = res1;
    auto const& [txs2, results2, lcm2, entries2] = res2;
    int const txCount = txs1.size();
    REQUIRE(results1.results.size() == txCount);
    REQUIRE(results2.results.size() == txCount);
    for (int i = 0; i < txCount; ++i)
    {
        REQUIRE(txs1[i]->getFullHash() == txs2[i]->getFullHash());
        REQUIRE(results1.results[i].transactionHash ==
                results2.results[i].transactionHash);
        if (expectExactChanges)
        {
            REQUIRE(results1.results[i].result == results2.results[i].result);
        }
        else
        {
            // Don't compare fees as they might be different.
            REQUIRE(results1.results[i].result.result ==
                    results2.results[i].result.result);
        }
    }
    REQUIRE(lcm1.getTransactionResultMetaCount() == txCount);
    REQUIRE(lcm2.getTransactionResultMetaCount() == txCount);

    if (expectExactChanges)
    {
        for (int i = 0; i < txCount; ++i)
        {
            REQUIRE(lcm1.getPreTxApplyFeeProcessing(i) ==
                    lcm2.getPreTxApplyFeeProcessing(i));
            REQUIRE(lcm1.getPostTxApplyFeeProcessing(i) ==
                    lcm2.getPostTxApplyFeeProcessing(i));
        }
    }

    for (int txId = 0; txId < txCount; ++txId)
    {
        if (!isSuccessResult(results1.results[txId].result))
        {
            continue;
        }
        auto const& tx = txs1[txId];
        TransactionMetaFrame txMeta1(lcm1.getTransactionMeta(txId));
        TransactionMetaFrame txMeta2(lcm2.getTransactionMeta(txId));
        auto changes1 =
            groupLedgerEntryChanges(txMeta1.getLedgerEntryChangesAtOp(0));
        auto changes2 =
            groupLedgerEntryChanges(txMeta2.getLedgerEntryChangesAtOp(0));
        if (!tx->isSoroban())
        {
            continue;
        }

        UnorderedSet<LedgerKey> rwKeys;
        if (tx->isSoroban())
        {
            rwKeys.insert(tx->sorobanResources().footprint.readWrite.begin(),
                          tx->sorobanResources().footprint.readWrite.end());
        }

        if (expectExactChanges)
        {
            REQUIRE(changes1.size() == changes2.size());
        }
        else if (res1IsP22)
        {
            // In protocol 22 we may only have less changes due to some
            // TTL bumps being no-ops due to more eager TTL observation.
            REQUIRE(changes1.size() <= changes2.size());
        }
        UnorderedSet<LedgerKey> usedKeys2;

        for (auto const& [key, entryChanges1] : changes1)
        {
            if (auto it = changes2.find(key); it != changes2.end())
            {
                auto const& [_, entryChanges2] = *it;
                usedKeys2.insert(key);
                REQUIRE(entryChanges1.size() == entryChanges2.size());
                bool isRoTtlBump = key.type() == TTL && rwKeys.count(key) == 0;
                // After p22 only RO TTL bumps may diverge due to stage
                // observations.
                // In p22 the restoration lastModifiedLedgerSeq may diverge
                // (we handle that below).
                if (expectExactChanges || (!isRoTtlBump && !res1IsP22))
                {
                    REQUIRE(entryChanges1 == entryChanges2);
                    continue;
                }
                if (entryChanges1 != entryChanges2)
                {
                    if (key.type() == TTL)
                    {
                        // At least ensure that in both cases we've
                        // performed a TTL bump. Note: We could be more
                        // precise here by taking the stage observations and
                        // protocol versions into account, but that would be
                        // a bit too complex.
                        expectTTLBump(entryChanges1);
                        expectTTLBump(entryChanges2);
                    }
                    else
                    {
                        REQUIRE(res1IsP22);
                        // The p22 entries are always in the live state, so
                        // we expect the restoration lastModifiedLedgerSeq
                        // to stay the same as it was on entry creation.
                        REQUIRE(entryChanges1.at(0)
                                    .restored()
                                    .lastModifiedLedgerSeq ==
                                hotArchiveEntryCreatedLedger);
                        // That's the only diff we expect, so just update
                        // lastModifiedLedgerSeq and proceed to comparing
                        // the diffs.
                        auto modifiedEntryChanges1 = entryChanges1;
                        modifiedEntryChanges1.at(0)
                            .restored()
                            .lastModifiedLedgerSeq = entryChanges2.at(0)
                                                         .restored()
                                                         .lastModifiedLedgerSeq;
                        REQUIRE(modifiedEntryChanges1 == entryChanges2);
                    }
                }
            }
            else
            {
                // We only may end up here if we deal with a read-only TTL
                // bump that is redundant if TTL is more eagerly observed.
                REQUIRE(!res1IsP22);
                REQUIRE(!expectExactChanges);
                REQUIRE(key.type() == TTL);
                REQUIRE(rwKeys.count(key) == 0);
                expectTTLBump(entryChanges1);
            }
        }

        for (auto const& [key, entryChanges2] : changes2)
        {
            if (usedKeys2.count(key) > 0)
            {
                continue;
            }
            REQUIRE(!expectExactChanges);
            REQUIRE(rwKeys.count(key) == 0);
            // We only may end up here if we deal with a read-only TTL
            // bump that is redundant if TTL is more eagerly observed.
            expectTTLBump(entryChanges2);
        }
    }

    // The final state of all entries must be the same.
    REQUIRE(entries1.size() == entries2.size());
    for (int i = 0; i < entries1.size(); ++i)
    {
        // In p23+ we don't expect any differences at all.
        if (!res1IsP22)
        {
            REQUIRE(entries1[i] == entries2[i]);
            continue;
        }
        // In p22 there is no hot archive, so we need to account for that.
        auto const& [key1, entryPair1] = entries1[i];
        auto const& [liveEntry1, hotArchiveEntry1] = entryPair1;
        auto const& [key2, entryPair2] = entries2[i];
        auto const& [liveEntry2, hotArchiveEntry2] = entryPair2;
        REQUIRE(key1 == key2);
        REQUIRE(!hotArchiveEntry1);
        // Entry is in the live state in both cases.
        if (!hotArchiveEntry2)
        {
            if (!liveEntry1)
            {
                REQUIRE(!liveEntry2);
            }
            else if (liveEntry1 != liveEntry2)
            {
                if (key1.type() == TTL)
                {
                    // For TTL entries we expect that in p22 they always
                    // stay in the live state, while in p23+ they may be
                    // removed if the entry has been moved to hot archive.
                    // That's the only kind of diff we expect.
                    REQUIRE(!liveEntry2);
                    // The TTL of the entry is either non-bumped, or extended
                    // for long enough to be in the live state in the ledger
                    // where the transactions were applied.
                    auto nonBumpedTTL =
                        hotArchiveEntryCreatedLedger + MIN_PERSISTENT_TTL - 1;
                    auto smallBumpTTL = hotArchiveEntryCreatedLedger + 1 +
                                        TTL_EXTENSION_FOR_EXPIRED_IN_LIVE_STATE;
                    bool isExpectedTTL =
                        liveEntry1->data.ttl().liveUntilLedgerSeq ==
                            nonBumpedTTL ||
                        liveEntry1->data.ttl().liveUntilLedgerSeq ==
                            smallBumpTTL;
                    REQUIRE(isExpectedTTL);
                }
                else
                {
                    REQUIRE(liveEntry2);
                    // As with the similar meta check above, the
                    // lastModifiedLedgerSeq may be different in p22 due
                    // to absence of hot archive.
                    REQUIRE(liveEntry1->lastModifiedLedgerSeq ==
                            hotArchiveEntryCreatedLedger);
                    // That's the only diff we expect, so just update
                    // lastModifiedLedgerSeq and proceed to comparing
                    // the diffs.
                    auto modifiedLiveEntry1 = *liveEntry1;
                    modifiedLiveEntry1.lastModifiedLedgerSeq =
                        liveEntry2->lastModifiedLedgerSeq;
                    REQUIRE(modifiedLiveEntry1 == *liveEntry2);
                }
            }

            REQUIRE(!hotArchiveEntry2);
        }
        else
        {
            // Entry is in hot archive in p23+ and is still in live state
            // in p22.
            REQUIRE(liveEntry1 == hotArchiveEntry2);
            REQUIRE(!liveEntry2);
        }
    }
}

// This function sets up a new application with randomly generated, but stable
// (thanks to the seed) test data, applies a set of classic and Soroban
// transactions and returns the results of the transactions, the corresponding
// meta and the final state of the ledger entries.
// The transactions, meta, and results are all sorted by the transaction hash
// for the sake of comparison.
// This is meant to be called multiple times with the same seed and different
// protocol versions and parallelization settings, then the results can be
// compared via `compareResults`.
// The setup includes creation of the trustlines and upload of some contract
// data and contract code entries, some of which expire and maybe are also
// evicted to the hot archive.
// The transaction mix includes:
// - classic transactions that perform payments between the trustlines
// - Wasm uploads
// - contract data creations/updates/deletions via contract call
// - SAC transfers between the trustlines set up above
// - ExtendFootprintTTLOp transactions that extend data and code entries
// - RestoreFootprintOp transactions that restore data and code entries
// All the Soroban operations also have random read and write entries added to
// the footprint. When `autoRestore` is true, the test will also automatically
// restore any expired entries that are being accessed (otherwise we just let
// the transactions fail).
ResultType
applyTestTransactions(TestConfig const& testConfig, uint32_t protocolVersion,
                      int64_t seed, bool autoRestore, int stageCount,
                      int applyClusterCount,
                      std::vector<RustBuf> const& randomWasms,
                      uint32_t& hotArchiveEntryCreatedLedger)
{
    int const INVOKE_HOST_FN_TX_COUNT =
        (testConfig.contractDataKeyCount + testConfig.randomWasmCount +
         testConfig.trustlineCount) *
        3;

    int const SOURCE_ACCOUNT_COUNT =
        INVOKE_HOST_FN_TX_COUNT + testConfig.restoreTxCount +
        testConfig.extendTxCount + testConfig.classicTxCount;

    REQUIRE(applyClusterCount <= KEY_CLUSTER_COUNT);

    // NB: This test must perform reproducible random setup multiple times.
    // Make sure to only use the rng defined here and not rely on any
    // global random engines.
    std::mt19937 testRng(seed);

    stellar::uniform_int_distribution<uint64_t> contractDataEntryValueDistr;

    if (autoRestore)
    {
        REQUIRE(
            protocolVersionStartsFrom(protocolVersion, ProtocolVersion::V_23));
    }
    Config cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 2000;
    cfg.GENESIS_TEST_ACCOUNT_COUNT = SOURCE_ACCOUNT_COUNT;
    cfg.BACKFILL_RESTORE_META = true;
    SorobanTest test(
        cfg, /*useTestLimits=*/true, [&](SorobanNetworkConfig& cfg) {
            cfg.mStateArchivalSettings.minPersistentTTL = 100'000;
            // Set the eviction level low to make sure the entries are
            // evicted fast enough, but not 1, so that it's possible to
            // have expired and not evicted entries.
            cfg.mStateArchivalSettings.startingEvictionScanLevel = 2;
            // Make sure we evict all the entries when we can for
            // simplicity.
            cfg.mStateArchivalSettings.evictionScanSize = 10'000'000;

            // Increase the limits even further to not worry about them.
            cfg.mTxMaxSizeBytes = 10'000'000;
            cfg.mLedgerMaxTransactionsSizeBytes =
                std::numeric_limits<uint32_t>::max();

            cfg.mLedgerMaxInstructions = std::numeric_limits<int64_t>::max();

            cfg.mTxMaxDiskReadBytes = 10000 * 1024;
            cfg.mTxMaxDiskReadEntries = 1000;
            cfg.mTxMaxWriteLedgerEntries = 500;
            if (protocolVersionStartsFrom(protocolVersion,
                                          ProtocolVersion::V_23))
            {
                cfg.mTxMaxFootprintEntries = 1500;
            }
            cfg.mTxMaxWriteBytes = 10000 * 1024;

            cfg.mLedgerMaxDiskReadEntries =
                std::numeric_limits<uint32_t>::max();
            cfg.mLedgerMaxDiskReadBytes = std::numeric_limits<uint32_t>::max();
            cfg.mLedgerMaxWriteLedgerEntries =
                std::numeric_limits<uint32_t>::max();
            cfg.mLedgerMaxWriteBytes = std::numeric_limits<uint32_t>::max();

            cfg.mLedgerMaxTxCount = 10000;
        });
    auto applyValidTxs = [&test](auto const& txs) {
        auto resultSet = closeLedger(test.getApp(), txs);
        REQUIRE(txs.size() == resultSet.results.size());
        for (auto const& res : resultSet.results)
        {
            REQUIRE(isSuccessResult(res.result));
        }
    };

    std::vector<TestAccount> accounts;
    for (int i = 0; i < SOURCE_ACCOUNT_COUNT; ++i)
    {
        accounts.push_back(getGenesisAccount(test.getApp(), i));
    }

    Asset asset = makeAsset(test.getRoot().getSecretKey(), "aaa");

    std::vector<TransactionFrameBaseConstPtr> trustlineSetupTxs;

    for (int i = 0; i < testConfig.trustlineCount; ++i)
    {
        trustlineSetupTxs.push_back(accounts[i].tx(
            {changeTrust(asset, std::numeric_limits<int64_t>::max())}));
    }

    applyValidTxs(trustlineSetupTxs);
    std::vector<Operation> trustlinePayments;
    for (int i = 0; i < testConfig.trustlineCount; ++i)
    {
        trustlinePayments.push_back(
            payment(accounts[i].getPublicKey(), asset, TRUSTLINE_BALANCE));
    }
    test.getRoot().applyOpsBatch(trustlinePayments);

    AssetContractTestClient assetClient(test, asset);
    ContractStorageTestClient storageClient(test, 10'000'000);
    modifySorobanNetworkConfig(test.getApp(), [](SorobanNetworkConfig& cfg) {
        cfg.mStateArchivalSettings.minTemporaryTTL = 16;
        cfg.mStateArchivalSettings.minPersistentTTL = MIN_PERSISTENT_TTL;
    });

    std::vector<TransactionFrameBaseConstPtr> setupTxs;
    std::vector<LedgerKey> preuploadedEntryKeys;
    double preuploadedEntriesFraction = std::uniform_real_distribution<>(
        MIN_PREUPLOADED_ENTRIES_FRACTION,
        MAX_PREUPLOADED_ENTRIES_FRACTION)(testRng);
    int const PREUPLOADED_WASM_COUNT =
        preuploadedEntriesFraction * testConfig.randomWasmCount;
    for (int i = 0; i < PREUPLOADED_WASM_COUNT; ++i)
    {
        auto uploadResources = defaultUploadWasmResourcesWithoutFootprint(
            randomWasms[i], protocolVersion);
        auto tx =
            makeSorobanWasmUploadTx(test.getApp(), accounts.at(setupTxs.size()),
                                    randomWasms[i], uploadResources, 100);
        preuploadedEntryKeys.push_back(
            uploadResources.footprint.readWrite.at(0));
        setupTxs.push_back(tx);
    }
    int const PREUPLOADED_CONTRACT_DATA_KEY_COUNT =
        preuploadedEntriesFraction * testConfig.contractDataKeyCount;
    for (int i = 0; i < PREUPLOADED_CONTRACT_DATA_KEY_COUNT; ++i)
    {
        auto key = "key" + std::to_string(i);

        auto putInvocation = storageClient.putInvocation(
            key,
            i % 2 == 0 ? ContractDataDurability::PERSISTENT
                       : ContractDataDurability::TEMPORARY,
            contractDataEntryValueDistr(testRng));
        auto spec = putInvocation.getSpec();
        auto const& dataKey = spec.getResources().footprint.readWrite.at(0);
        REQUIRE(dataKey.type() == LedgerEntryType::CONTRACT_DATA);
        preuploadedEntryKeys.push_back(dataKey);

        setupTxs.push_back(
            putInvocation.withExactNonRefundableResourceFee().createTx(
                &accounts.at(setupTxs.size())));
    }

    applyValidTxs(setupTxs);
    hotArchiveEntryCreatedLedger = test.getLCLSeq();

    double expiredEntryFraction = std::uniform_real_distribution<>(
        MIN_EXPIRED_ENTRY_FRACTION, MAX_EXPIRED_ENTRY_FRACTION)(testRng);
    int liveEntryCount =
        preuploadedEntryKeys.size() * (1.0 - expiredEntryFraction);
    // Bump some entries that are supposed to expire a little bit to make
    // sure they are expired, but not yet moved to the hot archive.
    int expiredInLiveStateEntryCount =
        0.5 * expiredEntryFraction * preuploadedEntryKeys.size();
    stellar::shuffle(preuploadedEntryKeys.begin(), preuploadedEntryKeys.end(),
                     testRng);

    std::vector<TransactionFrameBaseConstPtr> bumpTxs;
    SorobanResources bumpResources;
    bumpResources.diskReadBytes =
        protocolVersionStartsFrom(protocolVersion, ProtocolVersion::V_23)
            ? 0
            : test.getNetworkCfg().txMaxDiskReadBytes();
    for (int i = 0; i < liveEntryCount; ++i)
    {
        auto lk = getTTLKey(preuploadedEntryKeys[i]);
        bumpResources.footprint.readOnly.push_back(preuploadedEntryKeys[i]);
    }
    bumpTxs.push_back(test.createExtendOpTx(
        bumpResources, 1000, 100, 100'000'000, &accounts.at(bumpTxs.size())));
    SorobanResources smallBumpResources = bumpResources;
    smallBumpResources.footprint.readOnly.clear();
    for (int i = 0; i < expiredInLiveStateEntryCount; ++i)
    {
        auto lk = getTTLKey(preuploadedEntryKeys[liveEntryCount + i]);
        smallBumpResources.footprint.readOnly.push_back(
            preuploadedEntryKeys[liveEntryCount + i]);
    }
    // Extend the entries to live for TTL_EXTENSION_FOR_EXPIRED_IN_LIVE_STATE
    // (=30) ledgers, which should be enough to trigger eviction at level 2 for
    // entries that live for just 10 ledgers.
    bumpTxs.push_back(test.createExtendOpTx(
        smallBumpResources, TTL_EXTENSION_FOR_EXPIRED_IN_LIVE_STATE, 100,
        100'000'000, &accounts.at(bumpTxs.size())));
    applyValidTxs(bumpTxs);

    // Close TTL_EXTENSION_FOR_EXPIRED_IN_LIVE_STATE ledgers to let the entries
    // to expire, and for some of the entries to be evicted/removed.
    for (int i = 0; i < TTL_EXTENSION_FOR_EXPIRED_IN_LIVE_STATE; ++i)
    {
        closeLedger(test.getApp());
    }
    for (int i = 0; i < preuploadedEntryKeys.size(); ++i)
    {
        auto const& key = preuploadedEntryKeys[i];
        if (i < liveEntryCount)
        {
            REQUIRE(test.getEntryExpirationStatus(key) ==
                    ExpirationStatus::LIVE);
        }
        else if (i < liveEntryCount + expiredInLiveStateEntryCount)
        {
            REQUIRE(test.getEntryExpirationStatus(key) ==
                    ExpirationStatus::EXPIRED_IN_LIVE_STATE);
        }
        else
        {
            if (isTemporaryEntry(key))
            {
                REQUIRE(test.getEntryExpirationStatus(key) ==
                        ExpirationStatus::NOT_FOUND);
            }
            else
            {
                if (protocolVersionStartsFrom(protocolVersion,
                                              ProtocolVersion::V_23))
                {
                    REQUIRE(test.getEntryExpirationStatus(key) ==
                            ExpirationStatus::HOT_ARCHIVE);
                }
                else
                {
                    // There is no hot archive in p22.
                    REQUIRE(test.getEntryExpirationStatus(key) ==
                            ExpirationStatus::EXPIRED_IN_LIVE_STATE);
                }
            }
        }
    }

    std::vector<LedgerKey> allKeys = preuploadedEntryKeys;
    for (int i = PREUPLOADED_WASM_COUNT; i < testConfig.randomWasmCount; ++i)
    {
        LedgerKey key(CONTRACT_CODE);
        key.contractCode().hash = sha256(randomWasms[i]);
        allKeys.push_back(key);
    }
    for (int i = PREUPLOADED_CONTRACT_DATA_KEY_COUNT;
         i < testConfig.contractDataKeyCount; ++i)
    {
        auto key = "key" + std::to_string(i);
        LedgerKey k = storageClient.getContract().getDataKey(
            makeSymbolSCVal(key), i % 2 == 0
                                      ? ContractDataDurability::PERSISTENT
                                      : ContractDataDurability::TEMPORARY);
        allKeys.push_back(k);
    }

    for (int i = 0; i < testConfig.trustlineCount; ++i)
    {
        allKeys.push_back(trustlineKey(accounts[i].getPublicKey(), asset));
    }

    stellar::shuffle(allKeys.begin(), allKeys.end(), testRng);
    // We put all the keys into clusters and then generate transactions that
    // only access the keys from a single cluster. This way we can easily build
    // parallel transaction sets without any conflicting clusters.
    std::vector<std::vector<LedgerKey>> keyClusters(KEY_CLUSTER_COUNT);
    // Keep track of the created trustlines per cluster for the sake of easier
    // SAC transfer generation.
    std::vector<std::vector<LedgerKey>> trustlineClusters(KEY_CLUSTER_COUNT);
    for (int i = 0; i < allKeys.size(); ++i)
    {
        keyClusters[i % KEY_CLUSTER_COUNT].push_back(allKeys[i]);
        if (allKeys[i].type() == TRUSTLINE)
        {
            trustlineClusters[i % KEY_CLUSTER_COUNT].push_back(allKeys[i]);
        }
    }

    std::vector<std::vector<TransactionFrameBaseConstPtr>> sorobanTxsPerCluster(
        KEY_CLUSTER_COUNT);
    int sorobanTxCount = 0;

    stellar::uniform_int_distribution<> clusterDistr(0, KEY_CLUSTER_COUNT - 1);
    std::poisson_distribution<> additionalFootprintDistr(
        MEAN_ADDITIONAL_FOOTPRINT_KEYS);

    // Subtle: we define a single random read-only fraction for *all* the
    // test transactions in order to get distinctly different footprint
    // patterns in different test runs. Just using per-transaction
    // randomness would result in uniform 'on average' patterns that are
    // likely to provide similar coverage across runs.
    double const readOnlyFootprintEntriesFraction =
        std::uniform_real_distribution<>(MIN_READ_ONLY_FOOTPRINT_FRACTION,
                                         MAX_READ_ONLY_FOOTPRINT_FRACTION)(
            testRng);

    auto test_rand_flip = [&testRng]() {
        return stellar::uniform_int_distribution<>(0, 1)(testRng) != 0;
    };

    stellar::uniform_int_distribution<> resizeSizeDistr(1, 10);
    stellar::uniform_int_distribution<> extendTtlDistr(100, 5000);
    // Make sure some transfers will fail due to insufficient balance.
    stellar::uniform_int_distribution<> transferAmountDistr(
        1, TRUSTLINE_BALANCE * 1.2);

    for (int txId = 0; txId < INVOKE_HOST_FN_TX_COUNT; ++txId)
    {
        auto clusterId = clusterDistr(testRng);
        auto& cluster = keyClusters[clusterId];
        stellar::uniform_int_distribution<> keyIdDistr(0, cluster.size() - 1);
        // Pick the key from the cluster that defines the type of operation
        // we perform.
        auto const& opKey = cluster[keyIdDistr(testRng)];

        // Throw-away tx that we build with helpers. We're only
        // interested in the operation and the initial footprint in its
        // resources.
        TransactionFrameBaseConstPtr tx;
        TestAccount* operationSourceAccount = nullptr;
        switch (opKey.type())
        {
        // For contract code perform a Wasm upload.
        case CONTRACT_CODE:
        {
            auto wasmIt = std::find_if(
                randomWasms.begin(), randomWasms.end(), [&](auto const& wasm) {
                    return sha256(wasm) == opKey.contractCode().hash;
                });
            REQUIRE(wasmIt != randomWasms.end());
            auto resources = defaultUploadWasmResourcesWithoutFootprint(
                *wasmIt, protocolVersion);
            resources.footprint.readWrite.push_back(opKey);

            tx = makeSorobanWasmUploadTx(test.getApp(), test.getRoot(), *wasmIt,
                                         resources, 100);

            break;
        }
        // For contract data perform a put, delete or resize_and_extend
        // operation.
        case CONTRACT_DATA:
        {
            // Temp entries can't be extended with the test contract, so
            // always use `put` function for them. For persistent
            // entries, randomly choose between `put`, `delete` and
            // `resize_and_extend`.
            if (isTemporaryEntry(opKey) || test_rand_flip())
            {
                if (test_rand_flip())
                {
                    auto invocation = storageClient.putInvocation(
                        opKey.contractData().key.sym(),
                        opKey.contractData().durability,
                        contractDataEntryValueDistr(testRng));
                    tx = invocation.createTx();
                }
                else
                {
                    auto invocation = storageClient.delInvocation(
                        opKey.contractData().key.sym(),
                        opKey.contractData().durability);
                    tx = invocation.createTx();
                }
            }
            else
            {
                auto extendTo = extendTtlDistr(testRng);
                auto threshold =
                    stellar::uniform_int_distribution<>(1, extendTo)(testRng);
                auto invocation =
                    storageClient.resizeStorageAndExtendInvocation(
                        opKey.contractData().key.sym(),
                        resizeSizeDistr(testRng), threshold, extendTo);
                tx = invocation.createTx();
            }

            break;
        }
        // For trustlines perform a SAC transfer.
        case TRUSTLINE:
        {
            operationSourceAccount = &*std::find_if(
                accounts.begin(), accounts.end(), [&](auto const& a) {
                    return a.getPublicKey() == opKey.trustLine().accountID;
                });
            // NB: self-payment is fine, so we don't need to compare
            // destination to source.
            auto const& otherTrustline = trustlineClusters.at(clusterId).at(
                stellar::uniform_int_distribution<>(
                    0, trustlineClusters[clusterId].size() - 1)(testRng));
            bool fromIssuer, toIssuer;
            SCAddress toAddr;
            auto invocation = assetClient.transferInvocation(
                *operationSourceAccount,
                makeAccountAddress(otherTrustline.trustLine().accountID),
                transferAmountDistr(testRng), fromIssuer, toIssuer, toAddr);
            tx = invocation.createTx();
            break;
        }
        default:
            REQUIRE(false);
            break;
        }

        auto op = tx->getOperationFrames().front()->getOperation();
        auto resources = tx->sorobanResources();
        // Ensure the resources are sufficient for any operation most of
        // the time.
        resources.instructions = 100'000'000;
        resources.diskReadBytes = 100'000;
        resources.writeBytes = 100'000;
        std::vector<uint32_t> archivedIndices;

        // Extend the footprint with random keys from the cluster.
        int additionalKeyCount = std::min(additionalFootprintDistr(testRng),
                                          MAX_ADDITONAL_FOOTPRINT_KEYS);
        additionalKeyCount =
            std::min<int>(additionalKeyCount, cluster.size() - 1);
        stellar::shuffle(cluster.begin(), cluster.end(), testRng);
        int readOnlyKeyCount =
            readOnlyFootprintEntriesFraction * additionalKeyCount;
        UnorderedSet<LedgerKey> footprintKeys(
            resources.footprint.readOnly.begin(),
            resources.footprint.readOnly.end());
        footprintKeys.insert(resources.footprint.readWrite.begin(),
                             resources.footprint.readWrite.end());

        for (int addKeyId = 0; addKeyId < additionalKeyCount; ++addKeyId)
        {
            if (footprintKeys.count(cluster[addKeyId]) > 0)
            {
                continue;
            }
            auto const& addedKey = cluster[addKeyId];
            if (autoRestore && isSorobanEntry(addedKey) &&
                !isTemporaryEntry(addedKey) &&
                isExpiredStatus(test.getEntryExpirationStatus(addedKey)))
            {
                // If this would've been read-only key, increase the
                // count of read-only keys in order to keep the expected
                // RO/RW ratio.
                if (addKeyId < readOnlyKeyCount)
                {
                    ++readOnlyKeyCount;
                }
                resources.footprint.readWrite.push_back(addedKey);
                continue;
            }

            if (addKeyId < readOnlyKeyCount)
            {
                resources.footprint.readOnly.push_back(cluster[addKeyId]);
            }
            else
            {
                resources.footprint.readWrite.push_back(cluster[addKeyId]);
            }
        }
        if (autoRestore)
        {
            for (int i = 0; i < resources.footprint.readWrite.size(); ++i)
            {
                auto const& k = resources.footprint.readWrite[i];
                if (isSorobanEntry(k) && !isTemporaryEntry(k) &&
                    isExpiredStatus(test.getEntryExpirationStatus(k)))
                {
                    archivedIndices.push_back(i);
                }
            }
        }
        std::vector<SecretKey> opKeys;
        if (operationSourceAccount != nullptr &&
            operationSourceAccount->getPublicKey().ed25519() !=
                accounts.at(sorobanTxCount).getPublicKey().ed25519())
        {
            opKeys.push_back(operationSourceAccount->getSecretKey());
            op = operationSourceAccount->op(op);
        }
        auto maybeArchivedIndices = archivedIndices.empty()
                                        ? std::nullopt
                                        : std::make_optional(archivedIndices);
        tx = sorobanTransactionFrameFromOps(
            test.getApp().getNetworkID(), accounts.at(sorobanTxCount++), {op},
            opKeys, resources, 1000, 100'000'000, std::nullopt, std::nullopt,
            maybeArchivedIndices);
        sorobanTxsPerCluster[clusterId].push_back(tx);
    }
    // Generate ExtendFootprintTTLOp transactions that only extend keys from
    // a single cluster.
    for (int txId = 0; txId < testConfig.extendTxCount; ++txId)
    {
        auto clusterId = clusterDistr(testRng);
        auto& cluster = keyClusters[clusterId];
        SorobanResources resources{};
        resources.diskReadBytes = 100'000;
        resources.writeBytes = 100'000;
        stellar::shuffle(cluster.begin(), cluster.end(), testRng);
        int extendedKeyCount = stellar::uniform_int_distribution<>(
            1, std::max<int>(cluster.size() / 2, 1))(testRng);
        for (auto const& k : cluster)
        {
            if (isSorobanEntry(k) &&
                (isTemporaryEntry(k) ||
                 !isExpiredStatus(test.getEntryExpirationStatus(k))))
            {
                resources.footprint.readOnly.push_back(k);
                if (resources.footprint.readOnly.size() >= extendedKeyCount)
                {
                    break;
                }
            }
        }
        if (resources.footprint.readOnly.empty())
        {
            continue;
        }
        auto tx =
            test.createExtendOpTx(resources, extendTtlDistr(testRng), 1000,
                                  100'000'000, &accounts.at(sorobanTxCount++));
        sorobanTxsPerCluster[clusterId].push_back(tx);
    }

    // Generate RestoreFootprintTtlOp transactions that only restore keys from
    // a single cluster.
    for (int txId = 0; txId < testConfig.restoreTxCount; ++txId)
    {
        auto clusterId = clusterDistr(testRng);
        auto& cluster = keyClusters[clusterId];
        SorobanResources resources{};
        resources.diskReadBytes = 100'000;
        resources.writeBytes = 100'000;
        stellar::shuffle(cluster.begin(), cluster.end(), testRng);
        int restoredKeyCount = stellar::uniform_int_distribution<>(
            1, std::max<int>(cluster.size() / 3, 1))(testRng);

        for (auto const& k : cluster)
        {
            if (isSorobanEntry(k) && !isTemporaryEntry(k) &&
                (isExpiredStatus(test.getEntryExpirationStatus(k)) ||
                 (test_rand_flip() && test_rand_flip())))
            {
                resources.footprint.readWrite.push_back(k);
                if (resources.footprint.readWrite.size() >= restoredKeyCount)
                {
                    break;
                }
            }
        }
        if (resources.footprint.readWrite.empty())
        {
            continue;
        }
        auto tx = test.createRestoreTx(resources, 1000, 100'000'000,
                                       &accounts.at(sorobanTxCount++));
        sorobanTxsPerCluster[clusterId].push_back(tx);
    }

    // Generate some classic transfers between the trustlines that are also
    // used for SAC transfers above.
    std::vector<TransactionFrameBaseConstPtr> classicTxs;
    stellar::uniform_int_distribution<> trustlineDistr(
        0, testConfig.trustlineCount - 1);
    for (int txId = 0; txId < testConfig.classicTxCount; ++txId)
    {
        auto& source = accounts[trustlineDistr(testRng)];
        auto& dest = accounts[trustlineDistr(testRng)];
        int amount = transferAmountDistr(testRng);
        auto op = source.op(payment(dest.getPublicKey(), asset, amount));
        auto txEnv = accounts.at(sorobanTxCount + txId).tx({op})->getEnvelope();
        if (source.getPublicKey().ed25519() !=
            accounts.at(sorobanTxCount + txId).getPublicKey().ed25519())
        {
            sign(test.getApp().getNetworkID(), source.getSecretKey(),
                 txEnv.v1());
        }

        classicTxs.push_back(TransactionFrameBase::makeTransactionFromWire(
            test.getApp().getNetworkID(), txEnv));
    }

    std::vector<TransactionFrameBaseConstPtr> sorobanTxs;
    // We reduce the number of the clusters from KEY_CLUSTER_COUNT to
    // applyClusterCount via merging the clusters and 'interleaving' the
    // transactions within the cluster randomly.
    std::vector<std::vector<uint32_t>> applyClusters(applyClusterCount);
    int clustersPerApplyCluster = KEY_CLUSTER_COUNT / applyClusterCount +
                                  (KEY_CLUSTER_COUNT % applyClusterCount != 0);
    auto mergeClusters = [&](int from, int to, int applyClusterId) {
        stellar::uniform_int_distribution<> clusterDistr(from, to);
        std::unordered_set<int> usedClusters;
        std::vector<int> idInCluster(to - from + 1);
        while (usedClusters.size() < to - from + 1)
        {
            int clusterId = clusterDistr(testRng);
            if (usedClusters.count(clusterId) > 0)
            {
                continue;
            }
            if (idInCluster[clusterId - from] >=
                sorobanTxsPerCluster[clusterId].size())
            {
                usedClusters.insert(clusterId);
                continue;
            }
            applyClusters[applyClusterId].push_back(sorobanTxs.size());
            sorobanTxs.push_back(
                sorobanTxsPerCluster[clusterId][idInCluster[clusterId - from]]);
            ++idInCluster[clusterId - from];
        }
    };
    for (int i = 0, applyClusterId = 0; i < KEY_CLUSTER_COUNT;
         i += clustersPerApplyCluster, ++applyClusterId)
    {
        mergeClusters(
            i, std::min(i + clustersPerApplyCluster - 1, KEY_CLUSTER_COUNT - 1),
            applyClusterId);
    }
    // After we have built the target number of clusters, split every cluster
    // into `stageCount` stages and build the final apply order.
    ParallelSorobanOrder sorobanApplyOrder(
        stageCount, std::vector<std::vector<uint32_t>>(applyClusterCount));
    for (int applyClusterId = 0; applyClusterId < applyClusters.size();
         ++applyClusterId)
    {
        int sizePerStage =
            applyClusters[applyClusterId].size() / stageCount +
            (applyClusters[applyClusterId].size() % stageCount != 0);
        for (int stageId = 0; stageId < stageCount; ++stageId)
        {
            auto& cluster = sorobanApplyOrder.at(stageId).at(applyClusterId);
            REQUIRE(cluster.empty());
            int begin = stageId * sizePerStage;
            if (begin >= applyClusters[applyClusterId].size())
            {
                break;
            }
            int end = std::min<int>((stageId + 1) * sizePerStage,
                                    applyClusters[applyClusterId].size());
            cluster.insert(cluster.begin(),
                           applyClusters[applyClusterId].begin() + begin,
                           applyClusters[applyClusterId].begin() + end);
        }
    }
    for (auto& stage : sorobanApplyOrder)
    {
        for (int clusterId = 0; clusterId < stage.size(); ++clusterId)
        {
            if (stage[clusterId].empty())
            {
                stage.erase(stage.begin() + clusterId);
                --clusterId;
            }
        }
    }
    for (int stageId = 0; stageId < sorobanApplyOrder.size(); ++stageId)
    {
        if (sorobanApplyOrder[stageId].empty())
        {
            sorobanApplyOrder.erase(sorobanApplyOrder.begin() + stageId);
            --stageId;
        }
    }
    auto allTxs = classicTxs;
    allTxs.insert(allTxs.end(), sorobanTxs.begin(), sorobanTxs.end());
    {
        LedgerSnapshot ls(test.getApp());
        auto diag = DiagnosticEventManager::createDisabled();
        for (auto const& tx : allTxs)
        {
            bool isValid = tx->checkValid(test.getApp().getAppConnector(), ls,
                                          0, 0, 0, diag)
                               ->isSuccess();
            if (!isValid)
            {
                tx->checkValid(test.getApp().getAppConnector(), ls, 0, 0, 0,
                               diag);
            }
            REQUIRE(isValid);
        }
    }

    TransactionResultSet resultSet;
    if (protocolVersionStartsFrom(protocolVersion,
                                  PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
    {
        resultSet = closeLedger(test.getApp(), allTxs, sorobanApplyOrder);
    }
    else
    {
        resultSet = closeLedger(test.getApp(), allTxs, /*strictOrder=*/true);
    }
    REQUIRE(resultSet.results.size() == allTxs.size());
    for (auto const& res : resultSet.results)
    {
        if (!isSuccessResult(res.result))
        {
            // Errors are expected, but we want the fees and limits to not
            // be a reason, as these could be not stable between protocol
            // versions.
            auto const& tr = res.result.result.results()[0].tr();
            switch (tr.type())
            {
            case INVOKE_HOST_FUNCTION:
            {
                auto code = tr.invokeHostFunctionResult().code();
                REQUIRE(code !=
                        INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);
                REQUIRE(code != INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
            }
            break;
            case EXTEND_FOOTPRINT_TTL:
            {
                auto code = tr.extendFootprintTTLResult().code();
                REQUIRE(code != EXTEND_FOOTPRINT_TTL_RESOURCE_LIMIT_EXCEEDED);
                REQUIRE(code !=
                        EXTEND_FOOTPRINT_TTL_INSUFFICIENT_REFUNDABLE_FEE);
            }
            break;
            case RESTORE_FOOTPRINT:
            {
                auto code = tr.restoreFootprintResult().code();
                REQUIRE(code != RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
                REQUIRE(code != RESTORE_FOOTPRINT_INSUFFICIENT_REFUNDABLE_FEE);
            }
            break;
            default:
                break;
            }
        }
    }
    std::vector<std::pair<LedgerKey, std::pair<std::optional<LedgerEntry>,
                                               std::optional<LedgerEntry>>>>
        finalEntries;
    LedgerSnapshot ls(test.getApp());
    auto hotArchive = test.getApp()
                          .getBucketManager()
                          .getBucketSnapshotManager()
                          .copySearchableHotArchiveBucketListSnapshot();
    releaseAssert(hotArchive);
    for (auto const& k : allKeys)
    {
        std::optional<LedgerEntry> liveEntry;
        std::optional<LedgerEntry> archivedEntry;
        if (auto e = ls.load(k))
        {
            liveEntry = e.current();
            // All the entries that were in the live state and were
            // not immediately bumped will be evicted to the hot archive, so
            // live entries should not be expired.
            if (protocolVersionStartsFrom(protocolVersion,
                                          ProtocolVersion::V_23) &&
                isSorobanEntry(k))
            {
                REQUIRE(!isExpiredStatus(test.getEntryExpirationStatus(k)));
            }
        }
        else if (isSorobanEntry(k))
        {
            if (auto e = hotArchive->load(k))
            {
                archivedEntry = e->archivedEntry();
            }
        }
        finalEntries.push_back({k, {liveEntry, archivedEntry}});

        // Also add TTL entry as allKeys doesn't contain them.
        if (isSorobanEntry(k))
        {
            LedgerKey ttlKey = getTTLKey(k);
            std::optional<LedgerEntry> liveTtlEntry;
            if (auto e = ls.load(ttlKey))
            {
                liveTtlEntry = e.current();
            }
            finalEntries.push_back({ttlKey, {liveTtlEntry, std::nullopt}});
        }
    }

    // Sort everything by hash to make the comparison easier.
    // Note, that while the 'canonical' apply order could be stable as long
    // as there is only a single stage, it is tricky to match it when
    // there are multiple stages. The normalization also allows us to
    // randomly merge clusters in order to improve test coverage.
    std::sort(allTxs.begin(), allTxs.end(), [](auto const& a, auto const& b) {
        return a->getFullHash() < b->getFullHash();
    });
    std::sort(resultSet.results.begin(), resultSet.results.end(),
              [](auto const& a, auto const& b) {
                  return a.transactionHash < b.transactionHash;
              });
    auto lcm = test.getLastLcm();
    lcm.sortTxMetaByHash();
    return std::make_tuple(allTxs, resultSet, lcm, finalEntries);
}

void
runTest(int64_t seed, std::vector<std::pair<int, int>> const& scenarios,
        bool autoRestore, std::vector<RustBuf> const& randomWasms,
        TestConfig const& testConfig)
{
    CAPTURE(seed, autoRestore);
    // Note: we could also compare results across different protocol
    // versions past 23, but that's not necessary at the moment.
    auto protocolVersion = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    // This is a 'global' variable that marks the expected creation ledger
    // of the entries that are supposed to be evicted into the hot archive
    // in p23. This is necessary for the precise comparison with p22 that
    // doesn't have hot archive. It's expect that this will stay same
    // across all the runs of the test.
    uint32_t hotArchiveEntryCreatedLedger = 0;

    auto baseResult =
        applyTestTransactions(testConfig, protocolVersion, seed, autoRestore, 1,
                              1, randomWasms, hotArchiveEntryCreatedLedger);

    for (int scenarioId = 0; scenarioId < testConfig.testScenarioCount;
         ++scenarioId)
    {
        auto [stageCount, clusterCount] = scenarios[scenarioId];
        CAPTURE(stageCount, clusterCount);

        auto runResult = applyTestTransactions(
            testConfig, protocolVersion, seed, autoRestore, stageCount,
            clusterCount, randomWasms, hotArchiveEntryCreatedLedger);
        compareResults(false, stageCount == 1, baseResult, runResult,
                       hotArchiveEntryCreatedLedger);
    }
    if (!autoRestore)
    {
        INFO("pre-parallel-soroban protocol");
        auto preParallelSorobanResult = applyTestTransactions(
            testConfig,
            static_cast<uint32_t>(PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION) - 1,
            seed, autoRestore, 1, 1, randomWasms, hotArchiveEntryCreatedLedger);
        compareResults(true, true, preParallelSorobanResult, baseResult,
                       hotArchiveEntryCreatedLedger);
    }
}

void
runTestCase(TestConfig const& testConfig)
{
    std::vector<RustBuf> randomWasms;
    stellar::uniform_int_distribution<int64_t> seedDist;
    for (int i = 0; i < testConfig.randomWasmCount; ++i)
    {
        randomWasms.push_back(
            rust_bridge::get_random_wasm(1000, seedDist(Catch::rng())));
    }

    std::vector<std::pair<int, int>> scenarios;
    for (int stageCount = 1; stageCount <= MAX_STAGE_COUNT; ++stageCount)
    {
        for (int clusterCount = 1; clusterCount <= KEY_CLUSTER_COUNT;
             ++clusterCount)
        {
            scenarios.emplace_back(stageCount, clusterCount);
        }
    }

    for (int testIter = 0; testIter < TEST_RUN_COUNT; ++testIter)
    {
        CAPTURE(testIter);
        int64_t seed = Catch::rng()();
        stellar::shuffle(scenarios.begin(), scenarios.end(), Catch::rng());
        runTest(seed, scenarios, testIter % 2, randomWasms, testConfig);
    }
}

TEST_CASE("parallel soroban application results are independent of transaction "
          "partitioning - tiny scenario",
          "[soroban][parallelapply][acceptance]")
{
    TestConfig testConfig;
    testConfig.randomWasmCount = 2;
    testConfig.contractDataKeyCount = 5;
    testConfig.trustlineCount = 3;
    testConfig.classicTxCount = 5;
    testConfig.restoreTxCount = 2;
    testConfig.extendTxCount = 2;
    testConfig.testScenarioCount = KEY_CLUSTER_COUNT * MAX_STAGE_COUNT / 2;
    runTestCase(testConfig);
}

TEST_CASE("parallel soroban application results are independent of transaction "
          "partitioning - small scenario",
          "[soroban][parallelapply][acceptance]")
{
    TestConfig testConfig;
    testConfig.randomWasmCount = 6;
    testConfig.contractDataKeyCount = 20;
    testConfig.trustlineCount = 10;
    testConfig.classicTxCount = 20;
    testConfig.restoreTxCount = 10;
    testConfig.extendTxCount = 10;
    testConfig.testScenarioCount = 3;
    runTestCase(testConfig);
}

TEST_CASE("parallel soroban application results are independent of transaction "
          "partitioning - medium scenario",
          "[soroban][parallelapply][acceptance]")
{
    TestConfig testConfig;
    testConfig.randomWasmCount = 30;
    testConfig.contractDataKeyCount = 100;
    testConfig.trustlineCount = 50;
    testConfig.classicTxCount = 100;
    testConfig.restoreTxCount = 50;
    testConfig.extendTxCount = 50;
    testConfig.testScenarioCount = 3;
    runTestCase(testConfig);
}

TEST_CASE("parallel soroban application results are independent of transaction "
          "partitioning - large scenario",
          "[soroban][parallelapply][acceptance]")
{
    TestConfig testConfig;
    testConfig.randomWasmCount = 50;
    testConfig.contractDataKeyCount = 200;
    testConfig.trustlineCount = 100;
    testConfig.classicTxCount = 200;
    testConfig.restoreTxCount = 100;
    testConfig.extendTxCount = 100;
    testConfig.testScenarioCount = 3;
    runTestCase(testConfig);
}

} // namespace
