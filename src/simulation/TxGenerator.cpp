#include "simulation/TxGenerator.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "transactions/TransactionBridge.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include <cmath>
#include <crypto/SHA.h>

namespace stellar
{

using namespace std;
using namespace txtest;

namespace
{
// Default distribution settings, largely based on averages seen on testnet
constexpr uint32_t DEFAULT_WASM_BYTES = 35 * 1024;
constexpr uint32_t DEFAULT_NUM_DATA_ENTRIES = 2;
constexpr uint32_t DEFAULT_IO_KILOBYTES = 1;
constexpr uint32_t DEFAULT_TX_SIZE_BYTES = 256;
constexpr uint32_t DEFAULT_INSTRUCTIONS = 28'000'000;

// Sample from a discrete distribution of `values` with weights `weights`.
// Returns `defaultValue` if `values` is empty.
template <typename T>
T
sampleDiscrete(std::vector<T> const& values,
               std::vector<uint32_t> const& weights, T defaultValue)
{
    if (values.empty())
    {
        return defaultValue;
    }

    std::discrete_distribution<uint32_t> distribution(weights.begin(),
                                                      weights.end());
    return values.at(distribution(gRandomEngine));
}
} // namespace

uint64_t
footprintSize(Application& app, xdr::xvector<stellar::LedgerKey> const& keys)
{
    LedgerSnapshot lsg(app);
    uint64_t total = 0;
    for (auto const& key : keys)
    {
        auto entry = lsg.load(key);
        if (entry)
        {
            total += xdr::xdr_size(entry.current());
        }
    }
    return total;
}

TxGenerator::TxGenerator(Application& app)
    : mApp(app)
    , mMinBalance(0)
    , mApplySorobanSuccess(
          mApp.getMetrics().NewCounter({"ledger", "apply-soroban", "success"}))
    , mApplySorobanFailure(
          mApp.getMetrics().NewCounter({"ledger", "apply-soroban", "failure"}))
{
    updateMinBalance();
}

void
TxGenerator::updateMinBalance()
{
    auto b = mApp.getLedgerManager().getLastMinBalance(0);
    if (b > mMinBalance)
    {
        mMinBalance = b;
    }
}

int
TxGenerator::generateFee(std::optional<uint32_t> maxGeneratedFeeRate,
                         size_t opsCnt)
{
    int fee = 0;
    auto baseFee = mApp.getLedgerManager().getLastTxFee();

    if (maxGeneratedFeeRate)
    {
        auto feeRateDistr =
            uniform_int_distribution<uint32_t>(baseFee, *maxGeneratedFeeRate);
        // Add a bit more fee to get non-integer fee rates, such that
        // `floor(fee / opsCnt) == feeRate`, but
        // `fee / opsCnt >= feeRate`.
        // This is to create a bit more realistic fee structure: in reality not
        // every transaction would necessarily have the `fee == ops_count *
        // some_int`. This also would exercise more code paths/logic during the
        // transaction comparisons.
        auto fractionalFeeDistr = uniform_int_distribution<uint32_t>(
            0, static_cast<uint32_t>(opsCnt) - 1);
        fee = static_cast<uint32_t>(opsCnt) * feeRateDistr(gRandomEngine) +
              fractionalFeeDistr(gRandomEngine);
    }
    else
    {
        fee = static_cast<int>(opsCnt * baseFee);
    }

    return fee;
}

bool
TxGenerator::loadAccount(TestAccount& account)
{
    LedgerSnapshot lsg(mApp);
    auto const entry = lsg.getAccount(account.getPublicKey());
    if (!entry)
    {
        return false;
    }
    account.setSequenceNumber(entry.current().data.account().seqNum);
    return true;
}

bool
TxGenerator::loadAccount(TxGenerator::TestAccountPtr acc)
{
    if (acc)
    {
        return loadAccount(*acc);
    }
    return false;
}

std::pair<TxGenerator::TestAccountPtr, TxGenerator::TestAccountPtr>
TxGenerator::pickAccountPair(uint32_t numAccounts, uint32_t offset,
                             uint32_t ledgerNum, uint64_t sourceAccountId)
{
    auto sourceAccount = findAccount(sourceAccountId, ledgerNum);
    releaseAssert(
        !mApp.getHerder().sourceAccountPending(sourceAccount->getPublicKey()));

    auto destAccountId = rand_uniform<uint64_t>(0, numAccounts - 1) + offset;

    auto destAccount = findAccount(destAccountId, ledgerNum);

    CLOG_DEBUG(LoadGen, "Generated pair for payment tx - {} and {}",
               sourceAccountId, destAccountId);
    return std::pair<TxGenerator::TestAccountPtr, TxGenerator::TestAccountPtr>(
        sourceAccount, destAccount);
}

TxGenerator::TestAccountPtr
TxGenerator::findAccount(uint64_t accountId, uint32_t ledgerNum)
{
    // Load account and cache it.
    TxGenerator::TestAccountPtr newAccountPtr;

    auto res = mAccounts.find(accountId);
    if (res == mAccounts.end())
    {
        SequenceNumber sn = static_cast<SequenceNumber>(ledgerNum) << 32;
        auto name = "TestAccount-" + std::to_string(accountId);
        newAccountPtr =
            std::make_shared<TestAccount>(mApp, txtest::getAccount(name), sn);

        if (!loadAccount(newAccountPtr))
        {
            throw std::runtime_error(
                fmt::format("Account {0} must exist in the DB.", accountId));
        }
        mAccounts.insert(std::pair<uint64_t, TxGenerator::TestAccountPtr>(
            accountId, newAccountPtr));
    }
    else
    {
        newAccountPtr = res->second;
    }

    return newAccountPtr;
}

std::vector<Operation>
TxGenerator::createAccounts(uint64_t start, uint64_t count, uint32_t ledgerNum,
                            bool initialAccounts)
{
    vector<Operation> ops;
    SequenceNumber sn = static_cast<SequenceNumber>(ledgerNum) << 32;
    auto balance = initialAccounts ? mMinBalance * 10000000 : mMinBalance * 100;
    for (uint64_t i = start; i < start + count; i++)
    {
        auto name = "TestAccount-" + to_string(i);
        auto account = TestAccount{mApp, txtest::getAccount(name.c_str()), sn};
        ops.push_back(txtest::createAccount(account.getPublicKey(), balance));

        // Cache newly created account
        auto acc = make_shared<TestAccount>(account);
        mAccounts.emplace(i, acc);
    }
    return ops;
}

TransactionFrameBasePtr
TxGenerator::createTransactionFramePtr(
    TxGenerator::TestAccountPtr from, std::vector<Operation> ops, bool pretend,
    std::optional<uint32_t> maxGeneratedFeeRate)
{
    auto txf = transactionFromOperations(
        mApp, from->getSecretKey(), from->nextSequenceNumber(), ops,
        generateFee(maxGeneratedFeeRate, ops.size()));
    if (pretend)
    {
        Memo memo(MEMO_TEXT);
        memo.text() = std::string(28, ' ');
        txbridge::setMemo(txf, memo);

        txbridge::setMinTime(txf, 0);
        txbridge::setMaxTime(txf, UINT64_MAX);
    }

    txbridge::getSignatures(txf).clear();
    txf->addSignature(from->getSecretKey());
    return txf;
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBasePtr>
TxGenerator::paymentTransaction(uint32_t numAccounts, uint32_t offset,
                                uint32_t ledgerNum, uint64_t sourceAccount,
                                uint32_t opCount,
                                std::optional<uint32_t> maxGeneratedFeeRate)
{
    TxGenerator::TestAccountPtr to, from;
    uint64_t amount = 1;
    std::tie(from, to) =
        pickAccountPair(numAccounts, offset, ledgerNum, sourceAccount);
    vector<Operation> paymentOps;
    paymentOps.reserve(opCount);
    for (uint32_t i = 0; i < opCount; ++i)
    {
        paymentOps.emplace_back(txtest::payment(to->getPublicKey(), amount));
    }

    return std::make_pair(from,
                          createTransactionFramePtr(from, paymentOps, false,
                                                    maxGeneratedFeeRate));
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBasePtr>
TxGenerator::manageOfferTransaction(uint32_t ledgerNum, uint64_t accountId,
                                    uint32_t opCount,
                                    std::optional<uint32_t> maxGeneratedFeeRate)
{
    auto account = findAccount(accountId, ledgerNum);
    Asset selling(ASSET_TYPE_NATIVE);
    Asset buying(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(buying.alphaNum4().assetCode, "USD");
    vector<Operation> ops;
    for (uint32_t i = 0; i < opCount; ++i)
    {
        ops.emplace_back(txtest::manageBuyOffer(
            rand_uniform<int64_t>(1, 10000000), selling, buying,
            Price{rand_uniform<int32_t>(1, 100), rand_uniform<int32_t>(1, 100)},
            100));
    }
    return std::make_pair(
        account,
        createTransactionFramePtr(account, ops, false, maxGeneratedFeeRate));
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
TxGenerator::createUploadWasmTransaction(
    uint32_t ledgerNum, uint64_t accountId, xdr::opaque_vec<> const& wasm,
    LedgerKey const& contractCodeLedgerKey,
    std::optional<uint32_t> maxGeneratedFeeRate,
    std::optional<SorobanResources> uploadResources)
{
    auto account = findAccount(accountId, ledgerNum);

    if (!uploadResources)
    {
        uploadResources = SorobanResources{};
        uploadResources->instructions = 2'500'000;
        uploadResources->readBytes = wasm.size() + 500;
        uploadResources->writeBytes = wasm.size() + 500;
    }

    Operation uploadOp;
    uploadOp.body.type(INVOKE_HOST_FUNCTION);
    auto& uploadHF = uploadOp.body.invokeHostFunctionOp().hostFunction;
    uploadHF.type(HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);
    uploadHF.wasm() = wasm;

    uploadResources->footprint.readWrite = {contractCodeLedgerKey};

    int64_t resourceFee =
        sorobanResourceFee(mApp, *uploadResources, 5000 + wasm.size(), 100);
    resourceFee += 1'000'000;
    auto tx = sorobanTransactionFrameFromOps(mApp.getNetworkID(), *account,
                                             {uploadOp}, {}, *uploadResources,
                                             generateFee(maxGeneratedFeeRate,
                                                         /* opsCnt */ 1),
                                             resourceFee);

    return std::make_pair(account, tx);
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
TxGenerator::createContractTransaction(
    uint32_t ledgerNum, uint64_t accountId, LedgerKey const& codeKey,
    uint64_t contractOverheadBytes, uint256 const& salt,
    std::optional<uint32_t> maxGeneratedFeeRate)
{
    auto account = findAccount(accountId, ledgerNum);
    SorobanResources createResources{};
    createResources.instructions = 1'000'000;
    createResources.readBytes = contractOverheadBytes;
    createResources.writeBytes = 300;

    auto contractIDPreimage = makeContractIDPreimage(*account, salt);

    auto tx = makeSorobanCreateContractTx(
        mApp, *account, contractIDPreimage,
        makeWasmExecutable(codeKey.contractCode().hash), createResources,
        generateFee(maxGeneratedFeeRate,
                    /* opsCnt */ 1));

    return std::make_pair(account, tx);
}

static void
increaseOpSize(Operation& op, uint32_t increaseUpToBytes)
{
    if (increaseUpToBytes == 0)
    {
        return;
    }

    SorobanAuthorizationEntry auth;
    auth.credentials.type(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
    auth.rootInvocation.function.type(
        SOROBAN_AUTHORIZED_FUNCTION_TYPE_CONTRACT_FN);
    SCVal val(SCV_BYTES);

    auto const overheadBytes = xdr::xdr_size(auth) + xdr::xdr_size(val);
    if (overheadBytes > increaseUpToBytes)
    {
        increaseUpToBytes = 0;
    }
    else
    {
        increaseUpToBytes -= overheadBytes;
    }

    val.bytes().resize(increaseUpToBytes);
    auth.rootInvocation.function.contractFn().args = {val};
    op.body.invokeHostFunctionOp().auth = {auth};
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
TxGenerator::invokeSorobanLoadTransaction(
    uint32_t ledgerNum, uint64_t accountId, ContractInstance const& instance,
    uint64_t contractOverheadBytes, std::optional<uint32_t> maxGeneratedFeeRate)
{
    auto const& appCfg = mApp.getConfig();

    auto account = findAccount(accountId, ledgerNum);

    // Approximate instruction measurements from loadgen contract. While the
    // guest and host cycle counts are exact, and we can predict the cost of the
    // guest and host loops correctly, it is difficult to estimate the CPU cost
    // of storage given that the number and size of keys is variable. We have
    // rough estimates for storage and padding (because it uses an auth
    // payload). baseInstructionCount is for vm instantiation and additional
    // cushion to make sure transactions will succeed, but this means that the
    // instruction count is not perfect. Some TXs will fail due to exceeding
    // resource limitations, but failures will be rare and those failures
    // will happen at apply time, so they will still generate significant load.
    uint32_t const baseInstructionCount = 2'500'000;
    uint32_t const instructionsPerGuestCycle = 80;
    uint32_t const instructionsPerHostCycle = 5030;

    // Very rough estimates.
    uint32_t const instructionsPerKbWritten = 50000;

    // instructionsPerPaddingByte is just a value we know works. We use an auth
    // payload as padding, so it consumes instructions on the host side.
    uint32_t const instructionsPerPaddingByte = 100;

    SorobanResources resources;
    resources.footprint.readOnly = instance.readOnlyKeys;

    auto numEntries =
        sampleDiscrete(appCfg.LOADGEN_NUM_DATA_ENTRIES_FOR_TESTING,
                       appCfg.LOADGEN_NUM_DATA_ENTRIES_DISTRIBUTION_FOR_TESTING,
                       DEFAULT_NUM_DATA_ENTRIES);
    for (uint32_t i = 0; i < numEntries; ++i)
    {
        auto lk = contractDataKey(instance.contractID, makeU32(i),
                                  ContractDataDurability::PERSISTENT);
        resources.footprint.readWrite.emplace_back(lk);
    }

    std::vector<uint32_t> const& ioKilobytesValues =
        appCfg.LOADGEN_IO_KILOBYTES_FOR_TESTING;
    auto totalKbWriteBytes = sampleDiscrete(
        ioKilobytesValues, appCfg.LOADGEN_IO_KILOBYTES_DISTRIBUTION_FOR_TESTING,
        DEFAULT_IO_KILOBYTES);

    // Make sure write bytes is sufficient for number of entries written
    if (totalKbWriteBytes < numEntries)
    {
        totalKbWriteBytes = numEntries;
    }

    auto totalWriteBytes = totalKbWriteBytes * 1024;

    totalWriteBytes += numEntries * 100 /*Entry overhead*/;

    if (numEntries == 0)
    {
        totalWriteBytes = 0;
        numEntries = 0;
    }

    uint32_t kiloBytesPerEntry = 0;

    if (numEntries > 0)
    {
        kiloBytesPerEntry =
            (totalWriteBytes - contractOverheadBytes) / numEntries / 1024;

        // If numEntries > 0, we can't write a 0 byte entry
        if (kiloBytesPerEntry == 0)
        {
            kiloBytesPerEntry = 1;
        }
    }

    // Approximate TX size before padding and footprint, slightly over estimated
    // by `baselineTxOverheadBytes` so we stay below limits, plus footprint size
    uint32_t constexpr baselineTxOverheadBytes = 260;
    uint32_t const txOverheadBytes =
        baselineTxOverheadBytes + xdr::xdr_size(resources);
    uint32_t desiredTxBytes =
        sampleDiscrete(appCfg.LOADGEN_TX_SIZE_BYTES_FOR_TESTING,
                       appCfg.LOADGEN_TX_SIZE_BYTES_DISTRIBUTION_FOR_TESTING,
                       DEFAULT_TX_SIZE_BYTES);
    auto paddingBytes =
        txOverheadBytes > desiredTxBytes ? 0 : desiredTxBytes - txOverheadBytes;

    auto instructionsForStorageAndAuth =
        ((numEntries + kiloBytesPerEntry) * instructionsPerKbWritten) +
        instructionsPerPaddingByte * paddingBytes;

    // Pick random number of cycles between bounds
    uint32_t targetInstructions =
        sampleDiscrete(appCfg.LOADGEN_INSTRUCTIONS_FOR_TESTING,
                       appCfg.LOADGEN_INSTRUCTIONS_DISTRIBUTION_FOR_TESTING,
                       DEFAULT_INSTRUCTIONS);

    // Factor in instructions for storage
    targetInstructions = baseInstructionCount + instructionsForStorageAndAuth >=
                                 targetInstructions
                             ? 0
                             : targetInstructions - baseInstructionCount -
                                   instructionsForStorageAndAuth;

    // Randomly select a number of guest cycles
    uint32_t guestCyclesMax = targetInstructions / instructionsPerGuestCycle;
    uint32_t guestCycles = rand_uniform<uint64_t>(0, guestCyclesMax);

    // Rest of instructions consumed by host cycles
    targetInstructions -= guestCycles * instructionsPerGuestCycle;
    uint32_t hostCycles = targetInstructions / instructionsPerHostCycle;

    auto guestCyclesU64 = makeU64(guestCycles);
    auto hostCyclesU64 = makeU64(hostCycles);
    auto numEntriesU32 = makeU32(numEntries);
    auto kiloBytesPerEntryU32 = makeU32(kiloBytesPerEntry);

    Operation op;
    op.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
    ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
    ihf.invokeContract().contractAddress = instance.contractID;
    ihf.invokeContract().functionName = "do_work";
    ihf.invokeContract().args = {guestCyclesU64, hostCyclesU64, numEntriesU32,
                                 kiloBytesPerEntryU32};

    resources.readBytes = footprintSize(mApp, resources.footprint.readOnly) +
                          footprintSize(mApp, resources.footprint.readWrite);
    resources.writeBytes = totalWriteBytes;

    increaseOpSize(op, paddingBytes);

    uint32_t instructionCount =
        baseInstructionCount + hostCycles * instructionsPerHostCycle +
        guestCycles * instructionsPerGuestCycle + instructionsForStorageAndAuth;
    resources.instructions = instructionCount;

    auto resourceFee =
        sorobanResourceFee(mApp, resources, txOverheadBytes + paddingBytes, 40);
    resourceFee += 1'000'000;

    // A tx created using this method may be discarded when creating the txSet,
    // so we need to refresh the TestAccount sequence number to avoid a
    // txBAD_SEQ.
    account->loadSequenceNumber();

    auto tx = sorobanTransactionFrameFromOps(mApp.getNetworkID(), *account,
                                             {op}, {}, resources,
                                             generateFee(maxGeneratedFeeRate,
                                                         /* opsCnt */ 1),
                                             resourceFee);

    return std::make_pair(account, tx);
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
TxGenerator::invokeSorobanLoadTransactionV2(
    uint32_t ledgerNum, uint64_t accountId, ContractInstance const& instance,
    uint64_t dataEntryCount, size_t dataEntrySize,
    std::optional<uint32_t> maxGeneratedFeeRate)
{
    auto const& appCfg = mApp.getConfig();

    // The estimates below are fairly tight as they depend on linear
    // functions (maybe with a small constant factor as well).
    uint32_t const baseInstructionCount = 737'119;
    uint32_t const baselineTxSizeBytes = 256;
    uint32_t const eventSize = 80;
    uint32_t const instructionsPerGuestCycle = 40;
    uint32_t const instructionsPerHostCycle = 4'875;
    uint32_t const instructionsPerAuthByte = 35;
    uint32_t const instructionsPerEvent = 8'500;

    SorobanResources resources;
    resources.footprint.readOnly = instance.readOnlyKeys;
    uint32_t roEntries = sampleDiscrete(
        appCfg.APPLY_LOAD_NUM_RO_ENTRIES_FOR_TESTING,
        appCfg.APPLY_LOAD_NUM_RO_ENTRIES_DISTRIBUTION_FOR_TESTING, 0u);
    uint32_t rwEntries = sampleDiscrete(
        appCfg.APPLY_LOAD_NUM_RW_ENTRIES_FOR_TESTING,
        appCfg.APPLY_LOAD_NUM_RW_ENTRIES_DISTRIBUTION_FOR_TESTING, 0u);

    releaseAssert(dataEntryCount > roEntries + rwEntries);
    if (roEntries >= instance.readOnlyKeys.size())
    {
        roEntries -= instance.readOnlyKeys.size();
    }
    else
    {
        roEntries = 0;
    }
    std::unordered_set<uint64_t> usedEntries;
    stellar::uniform_int_distribution<uint64_t> entryDist(0,
                                                          dataEntryCount - 1);
    auto generateEntries = [&](uint32_t entryCount,
                               xdr::xvector<LedgerKey>& footprint) {
        for (uint32_t i = 0; i < entryCount; ++i)
        {
            uint64_t entryId = entryDist(gRandomEngine);
            if (usedEntries.emplace(entryId).second)
            {
                auto lk = contractDataKey(instance.contractID, makeU64(entryId),
                                          ContractDataDurability::PERSISTENT);
                footprint.emplace_back(lk);
            }
            else
            {
                --i;
            }
        }
    };
    generateEntries(roEntries, resources.footprint.readOnly);
    generateEntries(rwEntries, resources.footprint.readWrite);

    uint32_t txOverheadBytes = baselineTxSizeBytes + xdr::xdr_size(resources);
    uint32_t desiredTxBytes = sampleDiscrete(
        appCfg.LOADGEN_TX_SIZE_BYTES_FOR_TESTING,
        appCfg.LOADGEN_TX_SIZE_BYTES_DISTRIBUTION_FOR_TESTING, 0u);
    uint32_t paddingBytes =
        txOverheadBytes > desiredTxBytes ? 0 : desiredTxBytes - txOverheadBytes;
    uint32_t entriesSize = dataEntrySize * (roEntries + rwEntries);

    uint32_t eventCount = sampleDiscrete(
        appCfg.APPLY_LOAD_EVENT_COUNT_FOR_TESTING,
        appCfg.APPLY_LOAD_EVENT_COUNT_DISTRIBUTION_FOR_TESTING, 0u);

    // Pick random number of cycles between bounds
    uint32_t targetInstructions = sampleDiscrete(
        appCfg.LOADGEN_INSTRUCTIONS_FOR_TESTING,
        appCfg.LOADGEN_INSTRUCTIONS_DISTRIBUTION_FOR_TESTING, 0u);

    auto numEntries = (roEntries + rwEntries + instance.readOnlyKeys.size());

    // The entry encoding estimates are somewhat loose because we're
    // unfortunately building storage with O(n^2) complexity.

    // Figuring out the number of instructions for storage is difficult because
    // we build storage with O(n^2) complexity, so instead, I graphed the
    // instruction count provided in the diagnostics as the invocation starts
    // against different entry counts, and got the equation below from that. The
    // estimate is pretty close (usually off by 50,000 to 200,000 instructions).
    //
    // The instructionsPerEntryByte should probably be taken into account here
    // but I left the linear calculation for that because the estimate is
    // already close.
    uint32_t instructionsForEntries =
        (205 * std::pow(numEntries, 2)) + (12000 * numEntries) + 65485;

    uint32_t const instructionsPerEntryByte = 44;

    uint32_t instructionsWithoutCpuLoad =
        baseInstructionCount + instructionsPerAuthByte * paddingBytes +
        instructionsPerEntryByte * entriesSize + instructionsForEntries +
        eventCount * instructionsPerEvent;
    if (targetInstructions > instructionsWithoutCpuLoad)
    {
        targetInstructions -= instructionsWithoutCpuLoad;
    }
    else
    {
        targetInstructions = 0;
    }

    // Instead of mixing both guest and host cycles using the commented out code
    // above, we just use guestCycles because there's an issue with how the U256
    // add host function is modeled in the host. The instruction count is
    // greatly overestimated relative to actual time spent.
    uint64_t guestCycles = targetInstructions / instructionsPerGuestCycle;
    targetInstructions -= guestCycles * instructionsPerGuestCycle;
    uint64_t hostCycles = 0;

    Operation op;
    op.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
    ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
    ihf.invokeContract().contractAddress = instance.contractID;
    ihf.invokeContract().functionName = "do_cpu_only_work";
    ihf.invokeContract().args = {makeU32(guestCycles), makeU32(hostCycles),
                                 makeU32(eventCount)};
    resources.writeBytes = dataEntrySize * rwEntries;
    resources.readBytes = dataEntrySize * roEntries +
                          instance.contractEntriesSize + resources.writeBytes;

    increaseOpSize(op, paddingBytes);

    resources.instructions = instructionsWithoutCpuLoad +
                             hostCycles * instructionsPerHostCycle +
                             guestCycles * instructionsPerGuestCycle;

    auto resourceFee =
        sorobanResourceFee(mApp, resources, txOverheadBytes + paddingBytes,
                           eventSize * eventCount);
    resourceFee += 1'000'000;

    // A tx created using this method may be discarded when creating the txSet,
    // so we need to refresh the TestAccount sequence number to avoid a
    // txBAD_SEQ.
    auto account = findAccount(accountId, ledgerNum);
    account->loadSequenceNumber();

    auto tx = sorobanTransactionFrameFromOps(mApp.getNetworkID(), *account,
                                             {op}, {}, resources,
                                             generateFee(maxGeneratedFeeRate,
                                                         /* opsCnt */ 1),
                                             resourceFee);
    return std::make_pair(account, tx);
}

std::map<uint64_t, TxGenerator::TestAccountPtr> const&
TxGenerator::getAccounts()
{
    return mAccounts;
}

medida::Counter const&
TxGenerator::getApplySorobanSuccess()
{
    return mApplySorobanSuccess;
}

medida::Counter const&
TxGenerator::getApplySorobanFailure()
{
    return mApplySorobanFailure;
}

void
TxGenerator::reset()
{
    mAccounts.clear();
}

ConfigUpgradeSetKey
TxGenerator::getConfigUpgradeSetKey(SorobanUpgradeConfig const& upgradeCfg,
                                    Hash const& contractId) const
{
    SCBytes upgradeBytes = getConfigUpgradeSetFromLoadConfig(upgradeCfg);
    auto upgradeHash = sha256(upgradeBytes);

    ConfigUpgradeSetKey upgradeSetKey;
    upgradeSetKey.contentHash = upgradeHash;
    upgradeSetKey.contractID = contractId;
    return upgradeSetKey;
}

SCBytes
TxGenerator::getConfigUpgradeSetFromLoadConfig(
    SorobanUpgradeConfig const& upgradeCfg) const
{
    xdr::xvector<ConfigSettingEntry> updatedEntries;

    LedgerSnapshot lsg(mApp);
    for (uint32_t i = 0;
         i < static_cast<uint32_t>(CONFIG_SETTING_BUCKETLIST_SIZE_WINDOW); ++i)
    {
        auto entry = lsg.load(configSettingKey(static_cast<ConfigSettingID>(i)))
                         .current();
        auto& setting = entry.data.configSetting();
        switch (static_cast<ConfigSettingID>(i))
        {
        case CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES:
            if (upgradeCfg.maxContractSizeBytes > 0)
            {
                setting.contractMaxSizeBytes() =
                    upgradeCfg.maxContractSizeBytes;
            }
            break;
        case CONFIG_SETTING_CONTRACT_COMPUTE_V0:
            if (upgradeCfg.ledgerMaxInstructions > 0)
            {
                setting.contractCompute().ledgerMaxInstructions =
                    upgradeCfg.ledgerMaxInstructions;
            }

            if (upgradeCfg.txMaxInstructions > 0)
            {
                setting.contractCompute().txMaxInstructions =
                    upgradeCfg.txMaxInstructions;
            }

            if (upgradeCfg.txMemoryLimit > 0)
            {
                setting.contractCompute().txMemoryLimit =
                    upgradeCfg.txMemoryLimit;
            }
            break;
        case CONFIG_SETTING_CONTRACT_LEDGER_COST_V0:
            if (upgradeCfg.ledgerMaxReadLedgerEntries > 0)
            {
                setting.contractLedgerCost().ledgerMaxReadLedgerEntries =
                    upgradeCfg.ledgerMaxReadLedgerEntries;
            }

            if (upgradeCfg.ledgerMaxReadBytes > 0)
            {
                setting.contractLedgerCost().ledgerMaxReadBytes =
                    upgradeCfg.ledgerMaxReadBytes;
            }

            if (upgradeCfg.ledgerMaxWriteLedgerEntries > 0)
            {
                setting.contractLedgerCost().ledgerMaxWriteLedgerEntries =
                    upgradeCfg.ledgerMaxWriteLedgerEntries;
            }

            if (upgradeCfg.ledgerMaxWriteBytes > 0)
            {
                setting.contractLedgerCost().ledgerMaxWriteBytes =
                    upgradeCfg.ledgerMaxWriteBytes;
            }

            if (upgradeCfg.txMaxReadLedgerEntries > 0)
            {
                setting.contractLedgerCost().txMaxReadLedgerEntries =
                    upgradeCfg.txMaxReadLedgerEntries;
            }

            if (upgradeCfg.txMaxReadBytes > 0)
            {
                setting.contractLedgerCost().txMaxReadBytes =
                    upgradeCfg.txMaxReadBytes;
            }

            if (upgradeCfg.txMaxWriteLedgerEntries > 0)
            {
                setting.contractLedgerCost().txMaxWriteLedgerEntries =
                    upgradeCfg.txMaxWriteLedgerEntries;
            }

            if (upgradeCfg.txMaxWriteBytes > 0)
            {
                setting.contractLedgerCost().txMaxWriteBytes =
                    upgradeCfg.txMaxWriteBytes;
            }
            break;
        case CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0:
            break;
        case CONFIG_SETTING_CONTRACT_EVENTS_V0:
            if (upgradeCfg.txMaxContractEventsSizeBytes > 0)
            {
                setting.contractEvents().txMaxContractEventsSizeBytes =
                    upgradeCfg.txMaxContractEventsSizeBytes;
            }
            break;
        case CONFIG_SETTING_CONTRACT_BANDWIDTH_V0:
            if (upgradeCfg.ledgerMaxTransactionsSizeBytes > 0)
            {
                setting.contractBandwidth().ledgerMaxTxsSizeBytes =
                    upgradeCfg.ledgerMaxTransactionsSizeBytes;
            }

            if (upgradeCfg.txMaxSizeBytes > 0)
            {
                setting.contractBandwidth().txMaxSizeBytes =
                    upgradeCfg.txMaxSizeBytes;
            }
            break;
        case CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS:
        case CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES:
            break;
        case CONFIG_SETTING_CONTRACT_DATA_KEY_SIZE_BYTES:
            if (upgradeCfg.maxContractDataKeySizeBytes > 0)
            {
                setting.contractDataKeySizeBytes() =
                    upgradeCfg.maxContractDataKeySizeBytes;
            }
            break;
        case CONFIG_SETTING_CONTRACT_DATA_ENTRY_SIZE_BYTES:
            if (upgradeCfg.maxContractDataEntrySizeBytes > 0)
            {
                setting.contractDataEntrySizeBytes() =
                    upgradeCfg.maxContractDataEntrySizeBytes;
            }
            break;
        case CONFIG_SETTING_STATE_ARCHIVAL:
        {
            auto& ses = setting.stateArchivalSettings();
            if (upgradeCfg.maxEntryTTL > 0)
            {
                ses.maxEntryTTL = upgradeCfg.maxEntryTTL;
            }

            if (upgradeCfg.minTemporaryTTL > 0)
            {
                ses.minTemporaryTTL = upgradeCfg.minTemporaryTTL;
            }

            if (upgradeCfg.minPersistentTTL > 0)
            {
                ses.minPersistentTTL = upgradeCfg.minPersistentTTL;
            }

            if (upgradeCfg.persistentRentRateDenominator > 0)
            {
                ses.persistentRentRateDenominator =
                    upgradeCfg.persistentRentRateDenominator;
            }

            if (upgradeCfg.tempRentRateDenominator > 0)
            {
                ses.tempRentRateDenominator =
                    upgradeCfg.tempRentRateDenominator;
            }

            if (upgradeCfg.maxEntriesToArchive > 0)
            {
                ses.maxEntriesToArchive = upgradeCfg.maxEntriesToArchive;
            }

            if (upgradeCfg.bucketListSizeWindowSampleSize > 0)
            {
                ses.bucketListSizeWindowSampleSize =
                    upgradeCfg.bucketListSizeWindowSampleSize;
            }

            if (upgradeCfg.bucketListWindowSamplePeriod > 0)
            {
                ses.bucketListWindowSamplePeriod =
                    upgradeCfg.bucketListWindowSamplePeriod;
            }

            if (upgradeCfg.evictionScanSize > 0)
            {
                ses.evictionScanSize = upgradeCfg.evictionScanSize;
            }

            if (upgradeCfg.startingEvictionScanLevel > 0)
            {
                ses.startingEvictionScanLevel =
                    upgradeCfg.startingEvictionScanLevel;
            }
        }
        break;
        case CONFIG_SETTING_CONTRACT_EXECUTION_LANES:
            if (upgradeCfg.ledgerMaxTxCount > 0)
            {
                setting.contractExecutionLanes().ledgerMaxTxCount =
                    upgradeCfg.ledgerMaxTxCount;
            }
            break;
        default:
            releaseAssert(false);
            break;
        }

        // These two definitely aren't changing, and including both will hit the
        // contractDataEntrySizeBytes limit
        if (entry.data.configSetting().configSettingID() !=
                CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS &&
            entry.data.configSetting().configSettingID() !=
                CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES)
        {
            updatedEntries.emplace_back(entry.data.configSetting());
        }
    }

    ConfigUpgradeSet upgradeSet;
    upgradeSet.updatedEntry = updatedEntries;

    return xdr::xdr_to_opaque(upgradeSet);
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBasePtr>
TxGenerator::invokeSorobanCreateUpgradeTransaction(
    uint32_t ledgerNum, uint64_t accountId, SCBytes const& upgradeBytes,
    LedgerKey const& codeKey, LedgerKey const& instanceKey,
    std::optional<uint32_t> maxGeneratedFeeRate,
    std::optional<SorobanResources> resources)
{
    auto account = findAccount(accountId, ledgerNum);
    auto const& contractID = instanceKey.contractData().contract;

    LedgerKey upgradeLK(CONTRACT_DATA);
    upgradeLK.contractData().durability = TEMPORARY;
    upgradeLK.contractData().contract = contractID;

    SCVal upgradeHashBytes(SCV_BYTES);
    auto upgradeHash = sha256(upgradeBytes);
    upgradeHashBytes.bytes() = xdr::xdr_to_opaque(upgradeHash);
    upgradeLK.contractData().key = upgradeHashBytes;

    ConfigUpgradeSetKey upgradeSetKey;
    upgradeSetKey.contentHash = upgradeHash;
    upgradeSetKey.contractID = contractID.contractId();

    if (!resources)
    {
        resources = SorobanResources{};
        resources->instructions = 2'500'000;
        resources->readBytes = 3'100;
        resources->writeBytes = 3'100;
    }

    resources->footprint.readOnly = {instanceKey, codeKey};
    resources->footprint.readWrite = {upgradeLK};

    SCVal b(SCV_BYTES);
    b.bytes() = upgradeBytes;

    Operation op;
    op.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
    ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
    ihf.invokeContract().contractAddress = contractID;
    ihf.invokeContract().functionName = "write";
    ihf.invokeContract().args.emplace_back(b);

    auto resourceFee = sorobanResourceFee(mApp, *resources, 1'000, 40);
    resourceFee += 1'000'000;

    auto tx = sorobanTransactionFrameFromOps(mApp.getNetworkID(), *account,
                                             {op}, {}, *resources,
                                             generateFee(maxGeneratedFeeRate,
                                                         /* opsCnt */ 1),
                                             resourceFee);

    return std::make_pair(account, tx);
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
TxGenerator::sorobanRandomWasmTransaction(uint32_t ledgerNum,
                                          uint64_t accountId,
                                          uint32_t inclusionFee)
{
    auto [resources, wasmSize] = sorobanRandomUploadResources();

    auto account = findAccount(accountId, ledgerNum);
    Operation uploadOp = createUploadWasmOperation(wasmSize);
    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash =
        sha256(uploadOp.body.invokeHostFunctionOp().hostFunction.wasm());
    resources.footprint.readWrite.push_back(contractCodeLedgerKey);

    int64_t resourceFee = sorobanResourceFee(
        mApp, resources, 5000 + static_cast<size_t>(wasmSize), 100);
    // Roughly cover the rent fee.
    resourceFee += 1'000'000;
    auto tx = sorobanTransactionFrameFromOps(mApp.getNetworkID(), *account,
                                             {uploadOp}, {}, resources,
                                             inclusionFee, resourceFee);
    return std::make_pair(account, tx);
}

std::pair<SorobanResources, uint32_t>
TxGenerator::sorobanRandomUploadResources()
{
    auto const& cfg = mApp.getConfig();
    SorobanResources resources{};

    // Sample a random Wasm size
    uint32_t wasmSize = sampleDiscrete(
        cfg.LOADGEN_WASM_BYTES_FOR_TESTING,
        cfg.LOADGEN_WASM_BYTES_DISTRIBUTION_FOR_TESTING, DEFAULT_WASM_BYTES);

    // Estimate VM instantiation cost, with some additional buffer to increase
    // the chance that this instruction count is sufficient.
    ContractCostParamEntry vmInstantiationCosts =
        mApp.getLedgerManager()
            .getSorobanNetworkConfigReadOnly()
            .cpuCostParams()[VmInstantiation];
    // Amount to right shift `vmInstantiationCosts.linearTerm * wasmSize` by
    uint32_t constexpr vmShiftTerm = 7;
    // Additional buffer per byte to increase the chance that this instruction
    // count is sufficient
    uint32_t constexpr vmBufferPerByte = 100;
    // Perform multiplication as int64_t to avoid overflow
    uint32_t linearResult = static_cast<uint32_t>(
        (vmInstantiationCosts.linearTerm * static_cast<int64_t>(wasmSize)) >>
        vmShiftTerm);
    resources.instructions = vmInstantiationCosts.constTerm + linearResult +
                             (vmBufferPerByte * wasmSize);

    // Double instruction estimate because wasm parse price is charged twice (as
    // of protocol 21).
    resources.instructions *= 2;

    // Allocate enough write bytes to write the whole Wasm plus the 40 bytes of
    // the key with some additional buffer to increase the chance that this
    // write size is sufficient
    uint32_t constexpr keyOverhead = 40;
    uint32_t constexpr writeBuffer = 128;
    resources.writeBytes = wasmSize + keyOverhead + writeBuffer;

    return {resources, wasmSize};
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
TxGenerator::pretendTransaction(uint32_t numAccounts, uint32_t offset,
                                uint32_t ledgerNum, uint64_t sourceAccount,
                                uint32_t opCount,
                                std::optional<uint32_t> maxGeneratedFeeRate)
{
    vector<Operation> ops;
    ops.reserve(opCount);
    auto acc = findAccount(sourceAccount, ledgerNum);
    for (uint32 i = 0; i < opCount; i++)
    {
        auto args = SetOptionsArguments{};

        // We make SetOptionsOps such that we end up
        // with a n-op transaction that is exactly 100n + 240 bytes.
        args.inflationDest = std::make_optional<AccountID>(acc->getPublicKey());
        args.homeDomain = std::make_optional<std::string>(std::string(16, '*'));
        if (i == 0)
        {
            // The first operation needs to be bigger to achieve
            // 100n + 240 bytes.
            args.homeDomain->append(std::string(8, '*'));
            args.signer = std::make_optional<Signer>(Signer{});
        }
        ops.push_back(txtest::setOptions(args));
    }
    return std::make_pair(
        acc, createTransactionFramePtr(acc, ops, true, maxGeneratedFeeRate));
}

}
