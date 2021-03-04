#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "test/Fuzzer.h"
#include "util/Timer.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-types.h"

namespace stellar
{

class XDRInputFileStream;
class Simulation;
class Application;
struct StellarMessage;
struct Operation;

namespace FuzzUtils
{
size_t static constexpr NUM_STORED_LEDGER_KEYS = 0x100U;
size_t static constexpr NUM_UNVALIDATED_LEDGER_KEYS = 0x40U;
size_t static constexpr NUM_VALIDATED_LEDGER_KEYS =
    NUM_STORED_LEDGER_KEYS - NUM_UNVALIDATED_LEDGER_KEYS;

using StoredLedgerKeys = std::array<LedgerKey, NUM_STORED_LEDGER_KEYS>;
}

class TransactionFuzzer : public Fuzzer
{
  public:
    TransactionFuzzer()
    {
    }
    void inject(std::string const& filename) override;
    void initialize() override;
    void shutdown() override;
    void genFuzz(std::string const& filename) override;
    int xdrSizeLimit() override;

  private:
    void storeSetupLedgerKeys(AbstractLedgerTxn& ltx);
    void initializeAccounts(AbstractLedgerTxn& ltxOuter);
    void initializeTrustLines(AbstractLedgerTxn& ltxOuter);
    void initializeClaimableBalances(AbstractLedgerTxn& ltxOuter);
    void initializeOffers(AbstractLedgerTxn& ltxOuter);
    void reduceNativeBalancesAfterSetup(AbstractLedgerTxn& ltxOuter);
    void reduceTrustLineBalancesAfterSetup(AbstractLedgerTxn& ltxOuter);
    void reduceTrustLineLimitsAfterSetup(AbstractLedgerTxn& ltxOuter);
    VirtualClock mClock;
    std::shared_ptr<Application> mApp;
    PublicKey mSourceAccountID;
    FuzzUtils::StoredLedgerKeys mStoredLedgerKeys;
};

class OverlayFuzzer : public Fuzzer
{
    int const ACCEPTOR_INDEX = 0;
    int const INITIATOR_INDEX = 1;

  public:
    OverlayFuzzer()
    {
    }
    void inject(std::string const& filename) override;
    void initialize() override;
    void shutdown() override;
    void genFuzz(std::string const& filename) override;
    int xdrSizeLimit() override;

  private:
    std::shared_ptr<Simulation> mSimulation;
    FuzzUtils::StoredLedgerKeys mStoredLedgerKeys;
};
}