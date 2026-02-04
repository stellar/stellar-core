// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

/**
 * TxFuzzTarget - Transaction application fuzz target
 *
 * This fuzz target exercises transaction application by:
 * 1. Decoding fuzz input bytes into a compact XDR representation of operations
 * 2. Creating a transaction with those operations
 * 3. Attempting to apply the transaction to a pre-configured ledger state
 *
 * The compact XDR format (xdr_fuzzer_unpacker) provides stable byte expansion
 * where small input changes cause small semantic changes in the generated
 * operations. This is critical for effective fuzzing.
 */

#include "ledger/LedgerTxn.h"
#include "test/fuzz/FuzzTargetRegistry.h"
#include "util/Timer.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-types.h"

#include <memory>

namespace stellar
{

class Application;

namespace FuzzUtils
{
// Forward declarations of types defined in FuzzUtils.h
size_t static constexpr NUM_STORED_LEDGER_KEYS = 0x100U;
using StoredLedgerKeys = std::array<LedgerKey, NUM_STORED_LEDGER_KEYS>;
size_t static constexpr NUM_STORED_POOL_IDS = 0x7U;
using StoredPoolIDs = std::array<PoolID, NUM_STORED_POOL_IDS>;
}

class TxFuzzTarget : public FuzzTarget
{
  public:
    TxFuzzTarget() = default;

    std::string name() const override;
    std::string description() const override;
    void initialize() override;
    FuzzResultCode run(uint8_t const* data, size_t size) override;
    void shutdown() override;
    size_t maxInputSize() const override;
    void generateSeedCorpus(std::string const& outputDir,
                            size_t count) override;

  private:
    // Initialization helpers
    void storeSetupLedgerKeysAndPoolIDs(AbstractLedgerTxn& ltx);
    void storeSetupPoolIDs(AbstractLedgerTxn& ltx,
                           std::vector<LedgerEntry> const& entries);
    void initializeAccounts(AbstractLedgerTxn& ltxOuter);
    void initializeTrustLines(AbstractLedgerTxn& ltxOuter);
    void initializeClaimableBalances(AbstractLedgerTxn& ltxOuter);
    void initializeOffers(AbstractLedgerTxn& ltxOuter);
    void initializeLiquidityPools(AbstractLedgerTxn& ltxOuter);
    void reduceNativeBalancesAfterSetup(AbstractLedgerTxn& ltxOuter);
    void adjustTrustLineBalancesAfterSetup(AbstractLedgerTxn& ltxOuter);
    void reduceTrustLineLimitsAfterSetup(AbstractLedgerTxn& ltxOuter);

    VirtualClock mClock;
    std::shared_ptr<Application> mApp;
    PublicKey mSourceAccountID;
    FuzzUtils::StoredLedgerKeys mStoredLedgerKeys;
    FuzzUtils::StoredPoolIDs mStoredPoolIDs;
};

} // namespace stellar
