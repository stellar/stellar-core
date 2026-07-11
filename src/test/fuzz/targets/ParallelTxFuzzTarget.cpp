// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/fuzz/FuzzTargetRegistry.h"

#include "ledger/LedgerTxn.h"
#include "test/Catch2.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/fuzz/FuzzUtils.h"
#include "transactions/TransactionMeta.h"
#include "transactions/test/SorobanTxTestUtils.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace stellar
{
namespace
{

// This harness fuzzes Soroban intra-ledger parallel apply at the ledger-close
// level. It builds a fixed-size deterministic ledger of real storage-contract
// transactions, runs the same transaction set with different
// mLedgerMaxDependentTxClusters settings, and compares the serial and parallel
// outcomes. The oracle checks order-independent result codes, normalized write
// effects in transaction metadata, final contract-storage snapshots, and the
// values returned by HAS/GET operations in actual apply order.
//
// The target is meant to catch applyParallelPhase bugs such as bad dependency
// clustering, stale reads between transactions, lost or misordered writes,
// divergent metadata, or different final storage state under parallel apply. It
// deliberately keeps the workload narrow: current-protocol Soroban storage
// operations generated from deterministic fuzz bytes. It does not try to fuzz
// transaction validation, fee/resource accounting in general, arbitrary host
// functions, protocol-upgrade behavior, or non-Soroban ledger entries. Those are
// either above this comparison point or would make the differential oracle much
// less precise.

constexpr size_t MIN_TX_COUNT = 32;
constexpr size_t MAX_TX_COUNT = 32;
constexpr uint32_t SHARED_STORAGE_KEY_COUNT = 16;
constexpr uint32_t TOTAL_STORAGE_KEY_COUNT =
    SHARED_STORAGE_KEY_COUNT + static_cast<uint32_t>(MAX_TX_COUNT);
constexpr size_t ENCODED_TX_PLAN_SIZE = 15;
constexpr int SERIAL_CONFIG_INSTANCE = 701;
constexpr int PARALLEL_CONFIG_INSTANCE = 702;

class ByteCursor
{
  public:
    ByteCursor(uint8_t const* data, size_t size) : mData(data), mSize(size)
    {
    }

    uint8_t
    nextU8()
    {
        auto out = mData[mPos % mSize];
        ++mPos;
        return out;
    }

    uint64_t
    nextU64()
    {
        uint64_t out = 0;
        for (size_t i = 0; i < 8; ++i)
        {
            out = (out << 8) | nextU8();
        }
        return out;
    }

  private:
    uint8_t const* mData;
    size_t mSize;
    size_t mPos{0};
};

enum class DependencyShape : uint8_t
{
    INDEPENDENT = 0,
    READ_READ = 1,
    READ_WRITE = 2,
    WRITE_WRITE = 3
};

enum class StorageAction : uint8_t
{
    HAS = 0,
    GET = 1,
    PUT = 2,
    DEL = 3,
    RESIZE = 4
};

struct TxPlan
{
    DependencyShape dependencyShape;
    StorageAction preferredAction;
    uint32_t sharedKeyId;
    uint64_t value;
    uint32_t resizeKiloBytes;
    uint32_t instructions;
    uint32_t readBytes;
    uint32_t writeBytes;
};

struct ScenarioPlan
{
    std::vector<TxPlan> txPlans;
};

struct RunResult
{
    std::vector<Hash> submittedTxHashes;
    std::vector<Hash> applyOrderTxHashes;
    TransactionResultSet applyResults;
    std::vector<xdr::opaque_vec<>> txMeta;
    std::vector<xdr::opaque_vec<>> storageSnapshot;
};

struct ModeledStorageValue
{
    bool exists{true};
    uint64_t value{0};
};

void
appendOpaque(xdr::opaque_vec<>& out, xdr::opaque_vec<> const& opaque)
{
    out.insert(out.end(), opaque.begin(), opaque.end());
}

std::vector<xdr::opaque_vec<>>
sortedResultCodes(TransactionResultSet const& results)
{
    // Different dependent-cluster limits can produce different generalized
    // tx-set layouts, so result order and Soroban success hashes are not stable
    // enough to compare directly. Compare only the order-independent pieces
    // that say whether each transaction/operation succeeded in the same way.
    std::vector<xdr::opaque_vec<>> out;
    out.reserve(results.results.size());
    for (auto const& result : results.results)
    {
        auto opaque = xdr::xdr_to_opaque(result.transactionHash);
        appendOpaque(opaque, xdr::xdr_to_opaque(result.result.result.code()));
        if (result.result.result.code() == txSUCCESS ||
            result.result.result.code() == txFAILED)
        {
            for (auto const& opResult : result.result.result.results())
            {
                appendOpaque(opaque, xdr::xdr_to_opaque(opResult.code()));
                if (opResult.code() == opINNER)
                {
                    appendOpaque(opaque,
                                 xdr::xdr_to_opaque(opResult.tr().type()));
                    switch (opResult.tr().type())
                    {
                    case INVOKE_HOST_FUNCTION:
                        appendOpaque(
                            opaque,
                            xdr::xdr_to_opaque(
                                opResult.tr()
                                    .invokeHostFunctionResult()
                                    .code()));
                        break;
                    case EXTEND_FOOTPRINT_TTL:
                        appendOpaque(
                            opaque,
                            xdr::xdr_to_opaque(
                                opResult.tr()
                                    .extendFootprintTTLResult()
                                    .code()));
                        break;
                    case RESTORE_FOOTPRINT:
                        appendOpaque(
                            opaque,
                            xdr::xdr_to_opaque(
                                opResult.tr().restoreFootprintResult().code()));
                        break;
                    default:
                        break;
                    }
                }
            }
        }
        out.emplace_back(std::move(opaque));
    }
    std::sort(out.begin(), out.end());
    return out;
}

xdr::opaque_vec<>
normalizedChange(LedgerEntryChange const& change)
{
    xdr::opaque_vec<> out;
    auto isStorageDataEntry = [](LedgerEntry const& entry) {
        return entry.data.type() == CONTRACT_DATA &&
               entry.data.contractData().key.type() == SCV_SYMBOL;
    };
    auto isStorageDataKey = [](LedgerKey const& key) {
        return key.type() == CONTRACT_DATA &&
               key.contractData().key.type() == SCV_SYMBOL;
    };

    switch (change.type())
    {
    case LEDGER_ENTRY_CREATED:
        if (!isStorageDataEntry(change.created()))
        {
            break;
        }
        out.emplace_back(1);
        appendOpaque(out, xdr::xdr_to_opaque(change.created()));
        break;
    case LEDGER_ENTRY_UPDATED:
        if (!isStorageDataEntry(change.updated()))
        {
            break;
        }
        out.emplace_back(1);
        appendOpaque(out, xdr::xdr_to_opaque(change.updated()));
        break;
    case LEDGER_ENTRY_REMOVED:
        if (!isStorageDataKey(change.removed()))
        {
            break;
        }
        out.emplace_back(2);
        appendOpaque(out, xdr::xdr_to_opaque(change.removed()));
        break;
    case LEDGER_ENTRY_RESTORED:
        if (!isStorageDataEntry(change.restored()))
        {
            break;
        }
        out.emplace_back(1);
        appendOpaque(out, xdr::xdr_to_opaque(change.restored()));
        break;
    case LEDGER_ENTRY_STATE:
        break;
    }
    return out;
}

std::vector<xdr::opaque_vec<>>
sortedTxMetaPairs(TransactionResultSet const& results,
                  std::vector<TransactionMetaFrame> const& txMeta)
{
    if (results.results.size() != txMeta.size())
    {
        throw std::runtime_error("parallel_tx metadata count mismatch");
    }

    std::vector<xdr::opaque_vec<>> out;
    out.reserve(txMeta.size());
    for (size_t i = 0; i < txMeta.size(); ++i)
    {
        auto opaque = xdr::xdr_to_opaque(results.results.at(i).transactionHash);
        if (txtest::isSuccessResult(results.results.at(i).result))
        {
            // Keep this oracle focused on write effects. Return values for
            // shared-key reads are order-sensitive when legal cluster packing
            // changes transaction order, and getLastClosedLedgerTxMeta() does
            // not give us a transaction hash to pair metadata independently of
            // result ordering. Storage-entry changes still catch lost writes
            // without turning legal shared-key ordering into fuzz failures.
            std::vector<xdr::opaque_vec<>> changes;
            for (size_t opIdx = 0; opIdx < txMeta.at(i).getNumOperations();
                 ++opIdx)
            {
                for (auto const& change :
                     txMeta.at(i).getLedgerEntryChangesAtOp(opIdx))
                {
                    auto normalized = normalizedChange(change);
                    if (!normalized.empty())
                    {
                        changes.emplace_back(std::move(normalized));
                    }
                }
            }
            std::sort(changes.begin(), changes.end());
            for (auto const& change : changes)
            {
                appendOpaque(opaque, change);
            }
        }
        out.emplace_back(std::move(opaque));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<size_t>
findTxIndex(std::vector<Hash> const& hashes, Hash const& hash)
{
    auto it = std::find(hashes.begin(), hashes.end(), hash);
    if (it == hashes.end())
    {
        return std::nullopt;
    }
    return static_cast<size_t>(std::distance(hashes.begin(), it));
}

bool
successfulResultByHash(TransactionResultSet const& results, Hash const& hash)
{
    for (auto const& result : results.results)
    {
        if (result.transactionHash == hash)
        {
            return txtest::isSuccessResult(result.result);
        }
    }
    throw std::runtime_error("parallel_tx missing apply result");
}

std::string storageKeyName(uint32_t keyId);

std::vector<xdr::opaque_vec<>>
snapshotStorage(Application& app,
                txtest::ContractStorageTestClient& storageClient)
{
    // Shared keys intentionally create READ_WRITE/WRITE_WRITE dependencies.
    // Their final values can depend on legal transaction order choices, so the
    // final-state oracle checks only independent keys. Shared-key write effects
    // are covered above through per-transaction metadata changes.
    std::vector<xdr::opaque_vec<>> out;
    out.reserve(MAX_TX_COUNT);

    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto& contract = storageClient.getContract();
    for (uint32_t keyId = SHARED_STORAGE_KEY_COUNT;
         keyId < TOTAL_STORAGE_KEY_COUNT; ++keyId)
    {
        auto lk = contract.getDataKey(makeSymbolSCVal(storageKeyName(keyId)),
                                      ContractDataDurability::PERSISTENT);
        auto entry = ltx.load(lk);
        xdr::opaque_vec<> opaque;
        if (entry)
        {
            opaque = xdr::xdr_to_opaque(entry.current());
            opaque.insert(opaque.begin(), 1);
        }
        else
        {
            opaque.emplace_back(0);
        }
        out.emplace_back(std::move(opaque));
    }

    return out;
}

ScenarioPlan
decodeScenario(uint8_t const* data, size_t size)
{
    ByteCursor cursor(data, size);
    ScenarioPlan scenario;
    auto nTx = static_cast<size_t>(
        MIN_TX_COUNT + (cursor.nextU8() % (MAX_TX_COUNT - MIN_TX_COUNT + 1)));
    scenario.txPlans.reserve(nTx);

    for (size_t i = 0; i < nTx; ++i)
    {
        TxPlan plan;
        plan.dependencyShape =
            static_cast<DependencyShape>(cursor.nextU8() % 4);
        plan.preferredAction = static_cast<StorageAction>(cursor.nextU8() % 5);
        plan.sharedKeyId = cursor.nextU8() % SHARED_STORAGE_KEY_COUNT;
        plan.value = cursor.nextU64();
        plan.resizeKiloBytes = 1 + (cursor.nextU8() % 3);
        plan.instructions = 5'000'000 +
                            static_cast<uint32_t>(cursor.nextU8()) * 20'000;
        plan.readBytes = 20'000 + static_cast<uint32_t>(cursor.nextU8()) * 128;
        plan.writeBytes =
            20'000 + static_cast<uint32_t>(cursor.nextU8()) * 512;

        scenario.txPlans.emplace_back(plan);
    }

    return scenario;
}

size_t
requiredInputSize(uint8_t nTxByte)
{
    auto nTx = static_cast<size_t>(MIN_TX_COUNT +
                                   (nTxByte %
                                    (MAX_TX_COUNT - MIN_TX_COUNT + 1)));
    return 1 + nTx * ENCODED_TX_PLAN_SIZE;
}

uint32_t
storageKeyId(TxPlan const& plan, size_t txIndex)
{
    if (plan.dependencyShape == DependencyShape::INDEPENDENT)
    {
        return SHARED_STORAGE_KEY_COUNT + static_cast<uint32_t>(txIndex);
    }
    return plan.sharedKeyId;
}

std::string
storageKeyName(uint32_t keyId)
{
    return "key" + std::to_string(keyId);
}

uint64_t
initialStorageValue(uint32_t keyId)
{
    return 10'000 + keyId;
}

uint64_t
resizeStorageValue(TxPlan const& plan)
{
    return plan.resizeKiloBytes;
}

bool
shouldReadOnly(TxPlan const& plan, size_t txIndex)
{
    switch (plan.dependencyShape)
    {
    case DependencyShape::READ_READ:
        return true;
    case DependencyShape::READ_WRITE:
        return (txIndex % 2) == 0;
    case DependencyShape::INDEPENDENT:
    case DependencyShape::WRITE_WRITE:
        return false;
    }
    return false;
}

StorageAction
storageAction(TxPlan const& plan, size_t txIndex)
{
    if (shouldReadOnly(plan, txIndex))
    {
        return plan.preferredAction == StorageAction::HAS ? StorageAction::HAS
                                                          : StorageAction::GET;
    }

    if (plan.dependencyShape == DependencyShape::READ_WRITE ||
        plan.dependencyShape == DependencyShape::WRITE_WRITE)
    {
        return StorageAction::PUT;
    }

    switch (plan.preferredAction)
    {
    case StorageAction::DEL:
    case StorageAction::RESIZE:
        return plan.preferredAction;
    case StorageAction::HAS:
    case StorageAction::GET:
    case StorageAction::PUT:
        return StorageAction::PUT;
    }
    return StorageAction::PUT;
}

void
checkExpectedReturnValue(TransactionMetaFrame const& meta,
                         StorageAction action,
                         ModeledStorageValue const& expectedValue)
{
    auto const& returnValue = meta.getReturnValue();
    switch (action)
    {
    case StorageAction::HAS:
        if (returnValue.type() != SCV_BOOL ||
            returnValue.b() != expectedValue.exists)
        {
            throw std::runtime_error("parallel_tx stale HAS result");
        }
        break;
    case StorageAction::GET:
        if (!expectedValue.exists || returnValue.type() != SCV_U64 ||
            returnValue.u64() != expectedValue.value)
        {
            throw std::runtime_error("parallel_tx stale GET result");
        }
        break;
    case StorageAction::PUT:
    case StorageAction::DEL:
    case StorageAction::RESIZE:
        break;
    }
}

void
checkReadReturnOracle(ScenarioPlan const& scenario,
                      std::vector<Hash> const& submittedTxHashes,
                      std::vector<Hash> const& applyOrderTxHashes,
                      TransactionResultSet const& results,
                      std::vector<TransactionMetaFrame> const& txMeta)
{
    if (applyOrderTxHashes.size() != txMeta.size() ||
        results.results.size() != txMeta.size())
    {
        throw std::runtime_error("parallel_tx apply order count mismatch");
    }

    std::vector<ModeledStorageValue> storage(TOTAL_STORAGE_KEY_COUNT);
    for (uint32_t keyId = 0; keyId < TOTAL_STORAGE_KEY_COUNT; ++keyId)
    {
        storage.at(keyId).value = initialStorageValue(keyId);
    }

    for (size_t applyIndex = 0; applyIndex < applyOrderTxHashes.size();
         ++applyIndex)
    {
        auto txIndex = findTxIndex(submittedTxHashes,
                                  applyOrderTxHashes.at(applyIndex));
        if (!txIndex)
        {
            throw std::runtime_error("parallel_tx unknown apply-order tx");
        }

        if (!successfulResultByHash(results, applyOrderTxHashes.at(applyIndex)))
        {
            continue;
        }

        auto const& plan = scenario.txPlans.at(*txIndex);
        auto action = storageAction(plan, *txIndex);
        auto keyId = storageKeyId(plan, *txIndex);
        auto const expectedValue = storage.at(keyId);
        checkExpectedReturnValue(txMeta.at(applyIndex), action, expectedValue);

        switch (action)
        {
        case StorageAction::PUT:
            storage.at(keyId).exists = true;
            storage.at(keyId).value = plan.value;
            break;
        case StorageAction::DEL:
            storage.at(keyId).exists = false;
            break;
        case StorageAction::RESIZE:
            storage.at(keyId).exists = true;
            storage.at(keyId).value = resizeStorageValue(plan);
            break;
        case StorageAction::HAS:
        case StorageAction::GET:
            break;
        }
    }
}

txtest::SorobanInvocationSpec
withCommonResources(txtest::SorobanInvocationSpec spec, TxPlan const& plan,
                    uint32_t writeBytes, size_t txIndex)
{
    return spec.setInstructions(plan.instructions)
        .setReadBytes(plan.readBytes)
        .setWriteBytes(writeBytes)
        .setInclusionFee(1000 + static_cast<uint32_t>(txIndex))
        .setRefundableResourceFee(50'000'000);
}

TransactionFrameBasePtr
makeStorageTx(txtest::ContractStorageTestClient& storageClient,
              TxPlan const& plan, StorageAction action, std::string const& key,
              TestAccount& source, size_t txIndex)
{
    auto const durability = ContractDataDurability::PERSISTENT;
    auto& contract = storageClient.getContract();

    switch (action)
    {
    case StorageAction::HAS:
    {
        auto spec = withCommonResources(storageClient.readKeySpec(key, durability),
                                        plan, 0, txIndex);
        return contract.prepareInvocation("has_persistent",
                                          {makeSymbolSCVal(key)}, spec)
            .withExactNonRefundableResourceFee()
            .createTx(&source);
    }
    case StorageAction::GET:
    {
        auto spec = withCommonResources(storageClient.readKeySpec(key, durability),
                                        plan, 0, txIndex);
        return contract.prepareInvocation("get_persistent",
                                          {makeSymbolSCVal(key)}, spec)
            .withExactNonRefundableResourceFee()
            .createTx(&source);
    }
    case StorageAction::PUT:
    {
        auto spec = withCommonResources(
            storageClient.writeKeySpec(key, durability), plan, plan.writeBytes,
            txIndex);
        return storageClient
            .putInvocation(key, durability, plan.value, spec)
            .withExactNonRefundableResourceFee()
            .createTx(&source);
    }
    case StorageAction::DEL:
    {
        auto spec = withCommonResources(storageClient.writeKeySpec(key, durability),
                                        plan, 0, txIndex);
        return storageClient.delInvocation(key, durability, spec)
            .withExactNonRefundableResourceFee()
            .createTx(&source);
    }
    case StorageAction::RESIZE:
    {
        auto extendTo = 100 + static_cast<uint32_t>(plan.value % 1000);
        auto threshold = 1 + static_cast<uint32_t>(plan.value % extendTo);
        auto writeBytes = std::max(plan.writeBytes,
                                   plan.resizeKiloBytes * 1024 + 200);
        auto spec = withCommonResources(storageClient.writeKeySpec(key, durability),
                                        plan, writeBytes, txIndex);
        return storageClient
            .resizeStorageAndExtendInvocation(key, plan.resizeKiloBytes,
                                              threshold, extendTo, spec)
            .withExactNonRefundableResourceFee()
            .createTx(&source);
    }
    }

    throw std::runtime_error("unknown storage action");
}

std::vector<TestAccount>
getSourceAccounts(Application& app, size_t count)
{
    std::vector<TestAccount> accounts;
    accounts.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        accounts.push_back(txtest::getGenesisAccount(app, i));
    }
    return accounts;
}

void
seedStorage(txtest::SorobanTest& test,
            txtest::ContractStorageTestClient& storageClient,
            std::vector<TestAccount>& accounts)
{
    std::vector<TransactionFrameBasePtr> setupTxs;
    setupTxs.reserve(TOTAL_STORAGE_KEY_COUNT);

    for (uint32_t keyId = 0; keyId < TOTAL_STORAGE_KEY_COUNT; ++keyId)
    {
        auto key = storageKeyName(keyId);
        auto spec = storageClient
                        .writeKeySpec(key, ContractDataDurability::PERSISTENT)
                        .setInclusionFee(1000)
                        .setRefundableResourceFee(50'000'000);
        setupTxs.push_back(
            storageClient
                .putInvocation(key, ContractDataDurability::PERSISTENT,
                               initialStorageValue(keyId), spec)
                .withExactNonRefundableResourceFee()
                .createTx(&accounts.at(keyId)));
    }

    auto results = txtest::closeLedger(test.getApp(), setupTxs, true);
    if (results.results.size() != setupTxs.size())
    {
        throw std::runtime_error("parallel_tx setup result count mismatch");
    }

    for (auto const& result : results.results)
    {
        if (!txtest::isSuccessResult(result.result))
        {
            throw std::runtime_error("parallel_tx setup transaction failed");
        }
    }
}

RunResult
runScenario(uint32_t dependentClusterLimit, int configInstance,
            ScenarioPlan const& scenario)
{
    auto cfg = getFuzzConfig(configInstance);
    cfg.GENESIS_TEST_ACCOUNT_COUNT = TOTAL_STORAGE_KEY_COUNT + MAX_TX_COUNT;
    cfg.LEDGER_PROTOCOL_VERSION = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE = true;

    txtest::SorobanTest test(cfg, false);
    modifySorobanNetworkConfig(
        test.getApp(),
        [dependentClusterLimit](SorobanNetworkConfig& sorobanCfg) {
            sorobanCfg.mLedgerMaxDependentTxClusters = dependentClusterLimit;
            sorobanCfg.mLedgerMaxTxCount = 1000;
        });

    txtest::ContractStorageTestClient storageClient(test, 10'000'000);

    auto accounts = getSourceAccounts(
        test.getApp(), TOTAL_STORAGE_KEY_COUNT + scenario.txPlans.size());
    seedStorage(test, storageClient, accounts);

    std::vector<TransactionFrameBasePtr> txs;
    txs.reserve(scenario.txPlans.size());
    for (size_t i = 0; i < scenario.txPlans.size(); ++i)
    {
        auto const& plan = scenario.txPlans.at(i);
        auto action = storageAction(plan, i);
        auto& source = accounts.at(TOTAL_STORAGE_KEY_COUNT + i);
        auto keyId = storageKeyId(plan, i);
        auto key = storageKeyName(keyId);
        txs.push_back(
            makeStorageTx(storageClient, plan, action, key, source, i));
    }

    RunResult out;
    out.submittedTxHashes.reserve(txs.size());
    for (auto const& tx : txs)
    {
        out.submittedTxHashes.push_back(tx->getContentsHash());
    }

    out.applyOrderTxHashes.reserve(txs.size());
    auto applyOrderTxCallback =
        [&out](size_t txIndex, TransactionFrameBaseConstPtr const& tx) {
            if (txIndex != out.applyOrderTxHashes.size())
            {
                throw std::runtime_error("parallel_tx apply order index gap");
            }
            out.applyOrderTxHashes.push_back(tx->getContentsHash());
        };

    out.applyResults = txtest::closeLedger(test.getApp(), txs, false,
                                           emptyUpgradeSteps,
                                           applyOrderTxCallback);
    checkReadReturnOracle(
        scenario, out.submittedTxHashes, out.applyOrderTxHashes,
        out.applyResults,
        test.getApp().getLedgerManager().getLastClosedLedgerTxMeta());
    out.txMeta = sortedTxMetaPairs(
        out.applyResults,
        test.getApp().getLedgerManager().getLastClosedLedgerTxMeta());
    out.storageSnapshot = snapshotStorage(test.getApp(), storageClient);
    return out;
}

class ParallelTxFuzzTarget : public FuzzTarget
{
  public:
    std::string
    name() const override
    {
        return "parallel_tx";
    }

    std::string
    description() const override
    {
        return "Fuzz Soroban applyParallelPhase equivalence across real storage-contract workloads";
    }

    void
    initialize() override
    {
        reinitializeAllGlobalStateForFuzzing(1);
    }

    FuzzResultCode
    run(uint8_t const* data, size_t size) override
    {
        if (data == nullptr || size == 0 || size > maxInputSize())
        {
            return FuzzResultCode::FUZZ_REJECTED;
        }

        if (size < requiredInputSize(data[0]))
        {
            return FuzzResultCode::FUZZ_REJECTED;
        }
        auto scenario = decodeScenario(data, size);

        auto serial = runScenario(/* dependentClusterLimit */ 1,
                                  SERIAL_CONFIG_INSTANCE, scenario);
        auto parallel = runScenario(/* dependentClusterLimit */ 8,
                                    PARALLEL_CONFIG_INSTANCE, scenario);

        if (serial.submittedTxHashes != parallel.submittedTxHashes)
        {
            throw std::runtime_error("parallel_tx submitted tx order mismatch");
        }

        if (sortedResultCodes(serial.applyResults) !=
            sortedResultCodes(parallel.applyResults))
        {
            throw std::runtime_error("parallel_tx apply result mismatch");
        }

        if (serial.txMeta != parallel.txMeta)
        {
            throw std::runtime_error("parallel_tx metadata mismatch");
        }

        if (serial.storageSnapshot != parallel.storageSnapshot)
        {
            throw std::runtime_error("parallel_tx storage snapshot mismatch");
        }

        return FuzzResultCode::FUZZ_SUCCESS;
    }

    void
    shutdown() override
    {
    }

    size_t
    maxInputSize() const override
    {
        return 8 * 1024;
    }

    std::vector<uint8_t>
    generateSeedInput() override
    {
        std::vector<uint8_t> seed;
        seed.reserve(1 + MAX_TX_COUNT * 15);
        seed.push_back(0);

        auto appendU64 = [&seed](uint64_t v) {
            for (int shift = 56; shift >= 0; shift -= 8)
            {
                seed.push_back(static_cast<uint8_t>(v >> shift));
            }
        };

        auto appendPlan = [&seed, &appendU64](DependencyShape shape,
                                             StorageAction action,
                                             uint8_t sharedKeyId,
                                             uint64_t value,
                                             uint8_t resizeKiloBytes,
                                             uint8_t instructions,
                                             uint8_t readBytes,
                                             uint8_t writeBytes) {
            seed.push_back(static_cast<uint8_t>(shape));
            seed.push_back(static_cast<uint8_t>(action));
            seed.push_back(sharedKeyId);
            appendU64(value);
            seed.push_back(resizeKiloBytes - 1);
            seed.push_back(instructions);
            seed.push_back(readBytes);
            seed.push_back(writeBytes);
        };

        for (size_t i = 0; i < MAX_TX_COUNT; ++i)
        {
            switch (i % 8)
            {
            case 0:
                appendPlan(DependencyShape::READ_READ, StorageAction::HAS,
                           static_cast<uint8_t>(i % SHARED_STORAGE_KEY_COUNT),
                           100 + i, 1, i, i, i);
                break;
            case 1:
                appendPlan(DependencyShape::READ_READ, StorageAction::GET,
                           static_cast<uint8_t>((i - 1) %
                                                SHARED_STORAGE_KEY_COUNT),
                           100 + i, 1, i, i, i);
                break;
            case 2:
                appendPlan(DependencyShape::READ_WRITE, StorageAction::GET,
                           static_cast<uint8_t>(i % SHARED_STORAGE_KEY_COUNT),
                           100 + i, 1, i, i, i);
                break;
            case 3:
                appendPlan(DependencyShape::READ_WRITE, StorageAction::PUT,
                           static_cast<uint8_t>((i - 1) %
                                                SHARED_STORAGE_KEY_COUNT),
                           100 + i, 1, i, i, i);
                break;
            case 4:
                appendPlan(DependencyShape::WRITE_WRITE, StorageAction::PUT,
                           static_cast<uint8_t>(i % SHARED_STORAGE_KEY_COUNT),
                           100 + i, 1, i, i, i);
                break;
            case 5:
                appendPlan(DependencyShape::WRITE_WRITE, StorageAction::PUT,
                           static_cast<uint8_t>((i - 1) %
                                                SHARED_STORAGE_KEY_COUNT),
                           100 + i, 1, i, i, i);
                break;
            case 6:
                appendPlan(DependencyShape::INDEPENDENT, StorageAction::PUT, 0,
                           100 + i, 1, i, i, i);
                break;
            case 7:
                appendPlan(DependencyShape::INDEPENDENT, StorageAction::RESIZE,
                           0, 100 + i, 2, i, i, i);
                break;
            }
        }

        return seed;
    }
};

REGISTER_FUZZ_TARGET(
    ParallelTxFuzzTarget, "parallel_tx",
    "Fuzz Soroban applyParallelPhase equivalence across real storage-contract workloads");

} // namespace
} // namespace stellar