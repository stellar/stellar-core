#include "simulation/TxGenerator.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "simulation/ApplyLoad.h"
#include "simulation/LoadGenerator.h"
#include "transactions/TransactionBridge.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include <cmath>
#include <crypto/SHA.h>

namespace stellar
{

using namespace std;
using namespace txtest;

// Definition of static const member
uint64_t const TxGenerator::ROOT_ACCOUNT_ID = UINT64_MAX;

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
    return values.at(distribution(getGlobalRandomEngine()));
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

TxGenerator::TxGenerator(Application& app, uint32_t prePopulatedArchivedEntries)
    : mApp(app)
    , mMinBalance(0)
    , mApplySorobanSuccess(
          mApp.getMetrics().NewCounter({"ledger", "apply-soroban", "success"}))
    , mApplySorobanFailure(
          mApp.getMetrics().NewCounter({"ledger", "apply-soroban", "failure"}))
    , mPrePopulatedArchivedEntries(prePopulatedArchivedEntries)
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

bool
TxGenerator::isLive(LedgerKey const& lk, uint32_t ledgerNum) const
{
    LedgerSnapshot lsg(mApp);
    auto ttlEntryPtr = lsg.load(getTTLKey(lk));

    return ttlEntryPtr && stellar::isLive(ttlEntryPtr.current(), ledgerNum);
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
        fee = static_cast<uint32_t>(opsCnt) *
                  feeRateDistr(getGlobalRandomEngine()) +
              fractionalFeeDistr(getGlobalRandomEngine());
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
        // Special handling for root account
        if (accountId == TxGenerator::ROOT_ACCOUNT_ID)
        {
            newAccountPtr = mApp.getRoot();
        }
        else
        {
            SequenceNumber sn = static_cast<SequenceNumber>(ledgerNum) << 32;
            auto name = "TestAccount-" + std::to_string(accountId);
            newAccountPtr = std::make_shared<TestAccount>(
                mApp, txtest::getAccount(name), sn);

            if (!mApp.getRunInOverlayOnlyMode() && !loadAccount(newAccountPtr))
            {
                throw std::runtime_error(fmt::format(
                    "Account {0} must exist in the DB.", accountId));
            }
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
    TxGenerator::TestAccountPtr from, std::vector<Operation> ops,
    std::optional<uint32_t> maxGeneratedFeeRate)
{
    auto txf = transactionFromOperations(
        mApp, from->getSecretKey(), from->nextSequenceNumber(), ops,
        generateFee(maxGeneratedFeeRate, ops.size()));

    return txf;
}

TransactionFrameBasePtr
TxGenerator::createTransactionFramePtr(
    TxGenerator::TestAccountPtr from, std::vector<Operation> ops,
    std::optional<uint32_t> maxGeneratedFeeRate, uint32_t byteCount)
{
    auto txf = paddedTransactionFromOperations(
        mApp, from->getSecretKey(), from->nextSequenceNumber(), ops,
        generateFee(maxGeneratedFeeRate, ops.size()), byteCount);

    return txf;
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBasePtr>
TxGenerator::paymentTransaction(uint32_t numAccounts, uint32_t offset,
                                uint32_t ledgerNum, uint64_t sourceAccount,
                                uint32_t byteCount,
                                std::optional<uint32_t> maxGeneratedFeeRate)
{
    TxGenerator::TestAccountPtr to, from;
    uint64_t amount = 1;
    std::tie(from, to) =
        pickAccountPair(numAccounts, offset, ledgerNum, sourceAccount);
    vector<Operation> paymentOps;
    paymentOps.emplace_back(txtest::payment(to->getPublicKey(), amount));

    return std::make_pair(from, createTransactionFramePtr(from, paymentOps,
                                                          maxGeneratedFeeRate,
                                                          byteCount));
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBasePtr>
TxGenerator::manageOfferTransaction(uint32_t ledgerNum, uint64_t accountId,
                                    uint32_t byteCount,
                                    std::optional<uint32_t> maxGeneratedFeeRate)
{
    auto account = findAccount(accountId, ledgerNum);
    Asset selling(ASSET_TYPE_NATIVE);
    Asset buying(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(buying.alphaNum4().assetCode, "USD");
    vector<Operation> ops;
    ops.emplace_back(txtest::manageBuyOffer(
        rand_uniform<int64_t>(1, 10000000), selling, buying,
        Price{rand_uniform<int32_t>(1, 100), rand_uniform<int32_t>(1, 100)},
        100));
    return std::make_pair(
        account, createTransactionFramePtr(account, ops, maxGeneratedFeeRate));
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
        uploadResources->diskReadBytes = wasm.size() + 500;
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
    resourceFee += 50'000'000;
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
    createResources.diskReadBytes = contractOverheadBytes;
    createResources.writeBytes = 300;

    auto contractIDPreimage = makeContractIDPreimage(*account, salt);

    auto tx = makeSorobanCreateContractTx(
        mApp, *account, contractIDPreimage,
        makeWasmExecutable(codeKey.contractCode().hash), createResources,
        generateFee(maxGeneratedFeeRate,
                    /* opsCnt */ 1));

    return std::make_pair(account, tx);
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
TxGenerator::createSACTransaction(uint32_t ledgerNum,
                                  std::optional<uint64_t> accountId,
                                  Asset const& asset,
                                  std::optional<uint32_t> maxGeneratedFeeRate)
{
    auto account =
        accountId ? findAccount(*accountId, ledgerNum) : mApp.getRoot();
    SorobanResources createResources{};
    createResources.instructions = 1'000'000;
    createResources.diskReadBytes = 200;
    createResources.writeBytes = 300;

    auto contractIDPreimage = makeContractIDPreimage(asset);

    auto tx =
        makeSorobanCreateContractTx(mApp, *account, contractIDPreimage,
                                    makeAssetExecutable(asset), createResources,
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

    resources.diskReadBytes =
        footprintSize(mApp, resources.footprint.readOnly) +
        footprintSize(mApp, resources.footprint.readWrite);
    resources.writeBytes = totalWriteBytes;

    increaseOpSize(op, paddingBytes);

    uint32_t instructionCount =
        baseInstructionCount + hostCycles * instructionsPerHostCycle +
        guestCycles * instructionsPerGuestCycle + instructionsForStorageAndAuth;
    resources.instructions = instructionCount;

    auto resourceFee =
        sorobanResourceFee(mApp, resources, txOverheadBytes + paddingBytes, 40);
    resourceFee += 10'000'000;

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

    // Simulate disk reads via autorestore
    // If nextKeyToRestore is null, skip archived entries entirely
    uint32_t archiveEntriesToRestore = 0;
    if (mPrePopulatedArchivedEntries != 0)
    {
        archiveEntriesToRestore = sampleDiscrete(
            appCfg.APPLY_LOAD_NUM_RO_ENTRIES_FOR_TESTING,
            appCfg.APPLY_LOAD_NUM_RO_ENTRIES_DISTRIBUTION_FOR_TESTING, 0u);
    }

    uint32_t rwEntries = sampleDiscrete(
        appCfg.APPLY_LOAD_NUM_RW_ENTRIES_FOR_TESTING,
        appCfg.APPLY_LOAD_NUM_RW_ENTRIES_DISTRIBUTION_FOR_TESTING, 0u);

    // Subtract the archive entries from rwEntries since restoration counts as a
    // write
    if (rwEntries >= archiveEntriesToRestore)
    {
        rwEntries -= archiveEntriesToRestore;
    }
    else
    {
        rwEntries = 0;
    }

    releaseAssert(dataEntryCount > rwEntries);
    std::unordered_set<uint64_t> usedEntries;
    stellar::uniform_int_distribution<uint64_t> entryDist(0,
                                                          dataEntryCount - 1);
    auto generateEntries = [&](uint32_t entryCount,
                               xdr::xvector<LedgerKey>& footprint) {
        for (uint32_t i = 0; i < entryCount; ++i)
        {
            uint64_t entryId = entryDist(getGlobalRandomEngine());
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
    // Generate regular RW entries
    generateEntries(rwEntries, resources.footprint.readWrite);

    // Vector to store indices of archived entries in the readWrite footprint
    std::vector<uint32_t> archivedIndexes;

    // Add archived entries to autorestore to the readWrite footprint
    if (archiveEntriesToRestore > 0)
    {
        auto endIndex = mNextKeyToRestore + archiveEntriesToRestore;
        if (endIndex > mPrePopulatedArchivedEntries)
        {
            throw std::runtime_error("Ran out of hot archive entries");
        }

        for (; mNextKeyToRestore < endIndex; ++mNextKeyToRestore)
        {
            auto lk = ApplyLoad::getKeyForArchivedEntry(mNextKeyToRestore);
            resources.footprint.readWrite.emplace_back(lk);
            archivedIndexes.push_back(resources.footprint.readWrite.size() - 1);
        }
    }

    uint32_t txOverheadBytes = baselineTxSizeBytes + xdr::xdr_size(resources);
    uint32_t desiredTxBytes = sampleDiscrete(
        appCfg.LOADGEN_TX_SIZE_BYTES_FOR_TESTING,
        appCfg.LOADGEN_TX_SIZE_BYTES_DISTRIBUTION_FOR_TESTING, 0u);
    uint32_t paddingBytes =
        txOverheadBytes > desiredTxBytes ? 0 : desiredTxBytes - txOverheadBytes;
    uint32_t entriesSize =
        dataEntrySize * (rwEntries + archiveEntriesToRestore);

    uint32_t eventCount = sampleDiscrete(
        appCfg.APPLY_LOAD_EVENT_COUNT_FOR_TESTING,
        appCfg.APPLY_LOAD_EVENT_COUNT_DISTRIBUTION_FOR_TESTING, 0u);

    // Pick random number of cycles between bounds
    uint32_t targetInstructions = sampleDiscrete(
        appCfg.LOADGEN_INSTRUCTIONS_FOR_TESTING,
        appCfg.LOADGEN_INSTRUCTIONS_DISTRIBUTION_FOR_TESTING, 0u);

    auto numEntries =
        (rwEntries + archiveEntriesToRestore + instance.readOnlyKeys.size());

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

    // Write bytes include both regular RW entries and restored entries
    resources.writeBytes =
        dataEntrySize * (rwEntries + archiveEntriesToRestore);
    resources.diskReadBytes =
        dataEntrySize * archiveEntriesToRestore + resources.writeBytes;

    increaseOpSize(op, paddingBytes);

    resources.instructions = instructionsWithoutCpuLoad +
                             hostCycles * instructionsPerHostCycle +
                             guestCycles * instructionsPerGuestCycle;

    auto resourceFee =
        sorobanResourceFee(mApp, resources, txOverheadBytes + paddingBytes,
                           eventSize * eventCount);
    // Add extra buffer for restoration operations
    resourceFee += 1'000'000 + (archiveEntriesToRestore * 100'000);

    // A tx created using this method may be discarded when creating the txSet,
    // so we need to refresh the TestAccount sequence number to avoid a
    // txBAD_SEQ.
    auto account = findAccount(accountId, ledgerNum);
    account->loadSequenceNumber();

    auto tx = sorobanTransactionFrameFromOps(
        mApp.getNetworkID(), *account, {op}, {}, resources,
        generateFee(maxGeneratedFeeRate,
                    /* opsCnt */ 1),
        resourceFee, std::nullopt, std::nullopt,
        archivedIndexes.empty() ? std::nullopt
                                : std::make_optional(archivedIndexes));
    return std::make_pair(account, tx);
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
TxGenerator::invokeSACPayment(uint32_t ledgerNum, uint64_t fromAccountId,
                              SCAddress const& toAddress,
                              ContractInstance const& instance, uint64_t amount,
                              std::optional<uint32_t> maxGeneratedFeeRate)
{
    auto fromAccount = findAccount(fromAccountId, ledgerNum);
    fromAccount->loadSequenceNumber();

    SCVal fromVal(SCV_ADDRESS);
    fromVal.address() = makeAccountAddress(fromAccount->getPublicKey());

    SCVal toVal(SCV_ADDRESS);
    toVal.address() = toAddress;

    Operation op;
    op.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
    ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
    ihf.invokeContract().contractAddress = instance.contractID;
    ihf.invokeContract().functionName = "transfer";

    ihf.invokeContract().args = {fromVal, toVal, makeI128(amount)};

    SorobanResources resources;
    // These are overestimated
    resources.writeBytes = 800;
    resources.diskReadBytes = 800;
    resources.instructions = 250'000;
    resources.footprint.readOnly = instance.readOnlyKeys;

    LedgerKey fromKey(ACCOUNT);
    fromKey.account().accountID = fromAccount->getPublicKey();
    resources.footprint.readWrite.emplace_back(fromKey);

    if (toAddress.type() == SC_ADDRESS_TYPE_CONTRACT)
    {
        LedgerKey balanceKey(CONTRACT_DATA);
        balanceKey.contractData().contract = instance.contractID;

        balanceKey.contractData().key =
            makeVecSCVal({makeSymbolSCVal("Balance"), toVal});
        balanceKey.contractData().durability =
            ContractDataDurability::PERSISTENT;

        resources.footprint.readWrite.emplace_back(balanceKey);
    }
    else if (toAddress.type() == SC_ADDRESS_TYPE_ACCOUNT)
    {
        LedgerKey toKey(ACCOUNT);
        toKey.account().accountID = toAddress.accountId();
        resources.footprint.readWrite.emplace_back(toKey);
    }
    else
    {
        throw std::runtime_error("Unsupported address type for SAC payment");
    }

    SorobanAuthorizedInvocation invocation;
    invocation.function.type(SOROBAN_AUTHORIZED_FUNCTION_TYPE_CONTRACT_FN);
    invocation.function.contractFn() =
        op.body.invokeHostFunctionOp().hostFunction.invokeContract();

    SorobanCredentials credentials(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
    op.body.invokeHostFunctionOp().auth.emplace_back(credentials, invocation);

    auto resourceFee = sorobanResourceFee(mApp, resources, 350, 200);
    resourceFee += 1'000'000;

    auto tx = sorobanTransactionFrameFromOps(mApp.getNetworkID(), *fromAccount,
                                             {op}, {}, resources,
                                             generateFee(maxGeneratedFeeRate,
                                                         /* opsCnt */ 1),
                                             resourceFee);
    return std::make_pair(fromAccount, tx);
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

TxGenerator::TestAccountPtr
TxGenerator::getAccount(uint64_t accountId) const
{
    auto res = mAccounts.find(accountId);
    if (res == mAccounts.end())
    {
        return nullptr;
    }
    return res->second;
}

void
TxGenerator::addAccount(uint64_t accountId, TxGenerator::TestAccountPtr account)
{
    mAccounts.emplace(accountId, account);
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
    for (auto t : xdr::xdr_traits<ConfigSettingID>::enum_values())
    {
        auto type = static_cast<ConfigSettingID>(t);
        if (SorobanNetworkConfig::isNonUpgradeableConfigSettingEntry(type))
        {
            continue;
        }

        auto entryPtr = lsg.load(configSettingKey(type));
        // This could happen if we have not yet upgraded
        if ((t == CONFIG_SETTING_CONTRACT_PARALLEL_COMPUTE_V0 ||
             t == CONFIG_SETTING_CONTRACT_LEDGER_COST_EXT_V0 ||
             t == CONFIG_SETTING_SCP_TIMING) &&
            !entryPtr)
        {
            continue;
        }
        auto entry = entryPtr.current();

        auto& setting = entry.data.configSetting();
        // TODO(https://github.com/stellar/stellar-core/issues/4812): We should
        // set `updated` to `false` here and only set it to `true` when the
        // respective setting actually needs to be updated. However, since
        // currently all the settings are always updated, we only set this to
        // `false` for the cost params settings in order to not exceed limits on
        // the initial settings upgrade.
        bool updated = true;
        switch (type)
        {
        case CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES:
            if (upgradeCfg.maxContractSizeBytes.has_value())
            {
                setting.contractMaxSizeBytes() =
                    *upgradeCfg.maxContractSizeBytes;
            }
            break;
        case CONFIG_SETTING_CONTRACT_COMPUTE_V0:
            if (upgradeCfg.ledgerMaxInstructions.has_value())
            {
                setting.contractCompute().ledgerMaxInstructions =
                    *upgradeCfg.ledgerMaxInstructions;
            }

            if (upgradeCfg.txMaxInstructions.has_value())
            {
                setting.contractCompute().txMaxInstructions =
                    *upgradeCfg.txMaxInstructions;
            }

            if (upgradeCfg.feeRatePerInstructionsIncrement.has_value())
            {
                setting.contractCompute().feeRatePerInstructionsIncrement =
                    *upgradeCfg.feeRatePerInstructionsIncrement;
            }

            if (upgradeCfg.txMemoryLimit.has_value())
            {
                setting.contractCompute().txMemoryLimit =
                    *upgradeCfg.txMemoryLimit;
            }
            break;
        case CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS:
            if (upgradeCfg.cpuCostParams.has_value())
            {
                setting.contractCostParamsCpuInsns() =
                    *upgradeCfg.cpuCostParams;
            }
            else
            {
                updated = false;
            }
            break;
        case CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES:
            if (upgradeCfg.memCostParams.has_value())
            {
                setting.contractCostParamsMemBytes() =
                    *upgradeCfg.memCostParams;
            }
            else
            {
                updated = false;
            }
            break;
        case CONFIG_SETTING_CONTRACT_LEDGER_COST_V0:
            if (upgradeCfg.ledgerMaxDiskReadEntries.has_value())
            {
                setting.contractLedgerCost().ledgerMaxDiskReadEntries =
                    *upgradeCfg.ledgerMaxDiskReadEntries;
            }

            if (upgradeCfg.ledgerMaxDiskReadBytes.has_value())
            {
                setting.contractLedgerCost().ledgerMaxDiskReadBytes =
                    *upgradeCfg.ledgerMaxDiskReadBytes;
            }

            if (upgradeCfg.ledgerMaxWriteLedgerEntries.has_value())
            {
                setting.contractLedgerCost().ledgerMaxWriteLedgerEntries =
                    *upgradeCfg.ledgerMaxWriteLedgerEntries;
            }

            if (upgradeCfg.ledgerMaxWriteBytes.has_value())
            {
                setting.contractLedgerCost().ledgerMaxWriteBytes =
                    *upgradeCfg.ledgerMaxWriteBytes;
            }

            if (upgradeCfg.txMaxDiskReadEntries.has_value())
            {
                setting.contractLedgerCost().txMaxDiskReadEntries =
                    *upgradeCfg.txMaxDiskReadEntries;
            }

            if (upgradeCfg.txMaxDiskReadBytes.has_value())
            {
                setting.contractLedgerCost().txMaxDiskReadBytes =
                    *upgradeCfg.txMaxDiskReadBytes;
            }

            if (upgradeCfg.txMaxWriteLedgerEntries.has_value())
            {
                setting.contractLedgerCost().txMaxWriteLedgerEntries =
                    *upgradeCfg.txMaxWriteLedgerEntries;
            }

            if (upgradeCfg.txMaxWriteBytes.has_value())
            {
                setting.contractLedgerCost().txMaxWriteBytes =
                    *upgradeCfg.txMaxWriteBytes;
            }

            if (upgradeCfg.feeDiskReadLedgerEntry.has_value())
            {
                setting.contractLedgerCost().feeDiskReadLedgerEntry =
                    *upgradeCfg.feeDiskReadLedgerEntry;
            }

            if (upgradeCfg.feeWriteLedgerEntry.has_value())
            {
                setting.contractLedgerCost().feeWriteLedgerEntry =
                    *upgradeCfg.feeWriteLedgerEntry;
            }

            if (upgradeCfg.feeDiskRead1KB.has_value())
            {
                setting.contractLedgerCost().feeDiskRead1KB =
                    *upgradeCfg.feeDiskRead1KB;
            }

            if (upgradeCfg.rentFee1KBSorobanStateSizeLow.has_value())
            {
                setting.contractLedgerCost().rentFee1KBSorobanStateSizeLow =
                    *upgradeCfg.rentFee1KBSorobanStateSizeLow;
            }

            if (upgradeCfg.rentFee1KBSorobanStateSizeHigh.has_value())
            {
                setting.contractLedgerCost().rentFee1KBSorobanStateSizeHigh =
                    *upgradeCfg.rentFee1KBSorobanStateSizeHigh;
            }

            break;
        case CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0:
            if (upgradeCfg.feeHistorical1KB.has_value())
            {
                setting.contractHistoricalData().feeHistorical1KB =
                    *upgradeCfg.feeHistorical1KB;
            }
            break;
        case CONFIG_SETTING_CONTRACT_EVENTS_V0:
            if (upgradeCfg.txMaxContractEventsSizeBytes.has_value())
            {
                setting.contractEvents().txMaxContractEventsSizeBytes =
                    *upgradeCfg.txMaxContractEventsSizeBytes;
            }
            break;
        case CONFIG_SETTING_CONTRACT_BANDWIDTH_V0:
            if (upgradeCfg.ledgerMaxTransactionsSizeBytes.has_value())
            {
                setting.contractBandwidth().ledgerMaxTxsSizeBytes =
                    *upgradeCfg.ledgerMaxTransactionsSizeBytes;
            }

            if (upgradeCfg.txMaxSizeBytes.has_value())
            {
                setting.contractBandwidth().txMaxSizeBytes =
                    *upgradeCfg.txMaxSizeBytes;
            }

            if (upgradeCfg.feeTransactionSize1KB.has_value())
            {
                setting.contractBandwidth().feeTxSize1KB =
                    *upgradeCfg.feeTransactionSize1KB;
            }
            break;
        case CONFIG_SETTING_CONTRACT_DATA_KEY_SIZE_BYTES:
            if (upgradeCfg.maxContractDataKeySizeBytes.has_value())
            {
                setting.contractDataKeySizeBytes() =
                    *upgradeCfg.maxContractDataKeySizeBytes;
            }
            break;
        case CONFIG_SETTING_CONTRACT_DATA_ENTRY_SIZE_BYTES:
            if (upgradeCfg.maxContractDataEntrySizeBytes.has_value())
            {
                setting.contractDataEntrySizeBytes() =
                    *upgradeCfg.maxContractDataEntrySizeBytes;
            }
            break;
        case CONFIG_SETTING_STATE_ARCHIVAL:
        {
            auto& ses = setting.stateArchivalSettings();
            if (upgradeCfg.maxEntryTTL.has_value())
            {
                ses.maxEntryTTL = *upgradeCfg.maxEntryTTL;
            }

            if (upgradeCfg.minTemporaryTTL.has_value())
            {
                ses.minTemporaryTTL = *upgradeCfg.minTemporaryTTL;
            }

            if (upgradeCfg.minPersistentTTL.has_value())
            {
                ses.minPersistentTTL = *upgradeCfg.minPersistentTTL;
            }

            if (upgradeCfg.persistentRentRateDenominator.has_value())
            {
                ses.persistentRentRateDenominator =
                    *upgradeCfg.persistentRentRateDenominator;
            }

            if (upgradeCfg.tempRentRateDenominator.has_value())
            {
                ses.tempRentRateDenominator =
                    *upgradeCfg.tempRentRateDenominator;
            }

            if (upgradeCfg.maxEntriesToArchive.has_value())
            {
                ses.maxEntriesToArchive = *upgradeCfg.maxEntriesToArchive;
            }

            if (upgradeCfg.liveSorobanStateSizeWindowSampleSize.has_value())
            {
                ses.liveSorobanStateSizeWindowSampleSize =
                    *upgradeCfg.liveSorobanStateSizeWindowSampleSize;
            }

            if (upgradeCfg.liveSorobanStateSizeWindowSamplePeriod.has_value())
            {
                ses.liveSorobanStateSizeWindowSamplePeriod =
                    *upgradeCfg.liveSorobanStateSizeWindowSamplePeriod;
            }

            if (upgradeCfg.evictionScanSize.has_value())
            {
                ses.evictionScanSize = *upgradeCfg.evictionScanSize;
            }

            if (upgradeCfg.startingEvictionScanLevel.has_value())
            {
                ses.startingEvictionScanLevel =
                    *upgradeCfg.startingEvictionScanLevel;
            }
        }
        break;
        case CONFIG_SETTING_CONTRACT_EXECUTION_LANES:
            if (upgradeCfg.ledgerMaxTxCount.has_value())
            {
                setting.contractExecutionLanes().ledgerMaxTxCount =
                    *upgradeCfg.ledgerMaxTxCount;
            }
            break;
        case CONFIG_SETTING_CONTRACT_PARALLEL_COMPUTE_V0:
            if (upgradeCfg.ledgerMaxDependentTxClusters.has_value())
            {
                setting.contractParallelCompute().ledgerMaxDependentTxClusters =
                    *upgradeCfg.ledgerMaxDependentTxClusters;
            }
            break;
        case CONFIG_SETTING_CONTRACT_LEDGER_COST_EXT_V0:
            if (upgradeCfg.txMaxFootprintEntries.has_value())
            {
                setting.contractLedgerCostExt().txMaxFootprintEntries =
                    *upgradeCfg.txMaxFootprintEntries;
            }

            if (upgradeCfg.feeFlatRateWrite1KB.has_value())
            {
                setting.contractLedgerCostExt().feeWrite1KB =
                    *upgradeCfg.feeFlatRateWrite1KB;
            }
            break;
        case CONFIG_SETTING_SCP_TIMING:
            if (upgradeCfg.ledgerTargetCloseTimeMilliseconds.has_value())
            {
                setting.contractSCPTiming().ledgerTargetCloseTimeMilliseconds =
                    *upgradeCfg.ledgerTargetCloseTimeMilliseconds;
            }

            if (upgradeCfg.nominationTimeoutInitialMilliseconds.has_value())
            {
                setting.contractSCPTiming()
                    .nominationTimeoutInitialMilliseconds =
                    *upgradeCfg.nominationTimeoutInitialMilliseconds;
            }

            if (upgradeCfg.nominationTimeoutIncrementMilliseconds.has_value())
            {
                setting.contractSCPTiming()
                    .nominationTimeoutIncrementMilliseconds =
                    *upgradeCfg.nominationTimeoutIncrementMilliseconds;
            }

            if (upgradeCfg.ballotTimeoutInitialMilliseconds.has_value())
            {
                setting.contractSCPTiming().ballotTimeoutInitialMilliseconds =
                    *upgradeCfg.ballotTimeoutInitialMilliseconds;
            }

            if (upgradeCfg.ballotTimeoutIncrementMilliseconds.has_value())
            {
                setting.contractSCPTiming().ballotTimeoutIncrementMilliseconds =
                    *upgradeCfg.ballotTimeoutIncrementMilliseconds;
            }
            break;
        default:
            releaseAssert(false);
            break;
        }
        if (updated)
        {
            updatedEntries.emplace_back(entry.data.configSetting());
        }
    }

    ConfigUpgradeSet upgradeSet;
    upgradeSet.updatedEntry = updatedEntries;

    return xdr::xdr_to_opaque(upgradeSet);
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
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
        resources->diskReadBytes = 3'100;
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
    resourceFee += 20'000'000;

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
    resourceFee += 200'000'000;
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
            .getLastClosedSorobanNetworkConfig()
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
}
