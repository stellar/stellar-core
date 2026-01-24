// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

/**
 * TxFuzzTarget.h - Transaction fuzzing target
 *
 * This target fuzzes transaction application by:
 * 1. Decoding fuzz input bytes into a compact XDR representation of operations
 * 2. Creating a transaction with those operations
 * 3. Attempting to apply the transaction to a pre-configured ledger state
 *
 * The compact XDR format (xdr_fuzzer_unpacker) provides stable byte expansion
 * where small input changes cause small semantic changes in the generated
 * operations. This is critical for effective fuzzing.
 */

#include "test/fuzz/FuzzTargetRegistry.h"
#include "test/fuzz/FuzzUtils.h"
#include "transactions/TransactionFrame.h"
#include "util/Timer.h"

#include <memory>

namespace stellar
{

class Application;
class AbstractLedgerTxn;
class LedgerTxn;
class MutableTransactionResultBase;

// MutableTxResultPtr type alias
using MutableTxResultPtr = std::unique_ptr<MutableTransactionResultBase>;

// ============================================================================
// TX-specific functions (defined in TxFuzzTarget.cpp)
// ============================================================================

// Reset internal fuzzer state (caches, RNG seed, etc.)
void resetTxInternalState(Application& app);

// ============================================================================
// FuzzTransactionFrame - specialized TransactionFrame for fuzzing
// ============================================================================

class FuzzTransactionFrame : public TransactionFrame
{
  private:
    MutableTxResultPtr mTxResult;

  public:
    FuzzTransactionFrame(Hash const& networkID,
                         TransactionEnvelope const& envelope);

    void attemptApplication(Application& app, AbstractLedgerTxn& ltx);
    TransactionResult const& getResult() const;
    TransactionResultCode getResultCode() const;
};

// ============================================================================
// Transaction creation and application helpers
// ============================================================================

std::shared_ptr<FuzzTransactionFrame> createFuzzTransactionFrame(
    AbstractLedgerTxn& ltx, PublicKey const& sourceAccountID,
    std::vector<Operation>::const_iterator begin,
    std::vector<Operation>::const_iterator end, Hash const& networkID);

// Apply setup operations (throws on failure)
void applySetupOperations(LedgerTxn& ltx, PublicKey const& sourceAccount,
                          xdr::xvector<Operation>::const_iterator begin,
                          xdr::xvector<Operation>::const_iterator end,
                          Application& app);

// Apply fuzz operations (tolerates failures)
void applyFuzzOperations(LedgerTxn& ltx, PublicKey const& sourceAccount,
                         xdr::xvector<Operation>::const_iterator begin,
                         xdr::xvector<Operation>::const_iterator end,
                         Application& app);

// ============================================================================
// TxFuzzTarget class
// ============================================================================

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
    std::vector<uint8_t> generateSeedInput() override;

  private:
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
