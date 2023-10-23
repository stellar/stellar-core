#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{

namespace txtest
{

SCVal makeWasmRefScContractCode(Hash const& hash);
SCAddress makeContractAddress(Hash const& hash);
SCVal makeContractAddressSCVal(SCAddress const& address);
SCVal makeI32(int32_t i32);
SCVal makeI128(uint64_t u64);
SCSymbol makeSymbol(std::string const& str);
SCVal makeU32(uint32_t u32);
SCVal makeBytes(SCBytes bytes);

void submitTxToUploadWasm(Application& app, Operation const& op,
                          SorobanResources const& resources,
                          Hash const& expectedWasmHash,
                          xdr::opaque_vec<> const& expectedWasm,
                          uint32_t inclusionFee, uint32_t resourceFee);

void submitTxToCreateContract(Application& app, Operation const& op,
                              SorobanResources const& resources,
                              Hash const& contractID,
                              SCVal const& executableKey,
                              Hash const& expectedWasmHash,
                              uint32_t inclusionFee, uint32_t resourceFee);

class ContractInvocationTest
{
    // Fee constants from rs-soroban-env/soroban-env-host/src/fees.rs
    static constexpr int64_t INSTRUCTION_INCREMENT = 10000;
    static constexpr int64_t DATA_SIZE_1KB_INCREMENT = 1024;
    static constexpr int64_t TX_BASE_RESULT_SIZE = 300;

    static int64_t computeFeePerIncrement(int64_t resourceVal, int64_t feeRate,
                                          int64_t increment);

    void invokeArchivalOp(TransactionFrameBasePtr tx,
                          int64_t expectedRefundableFeeCharged,
                          bool expectSuccess);

    void deployContractWithSourceAccountWithResources(
        SorobanResources uploadResources, SorobanResources createResources);

  protected:
    VirtualClock mClock;
    Application::pointer mApp;
    TestAccount mRoot;
    TestAccount mDummyAccount;
    RustBuf const mWasm;

    xdr::xvector<LedgerKey> mContractKeys{};
    SCAddress mContractID{};

  public:
    ContractInvocationTest(
        RustBuf const& wasm, bool deployContract = true,
        Config cfg = getTestConfig(), bool useTestLimits = true,
        std::function<void(SorobanNetworkConfig&)> cfgModifyFn =
            [](SorobanNetworkConfig&) {});

    Application::pointer
    getApp()
    {
        return mApp;
    }

    xdr::xvector<LedgerKey>&
    getContractKeys()
    {
        return mContractKeys;
    }

    SCAddress&
    getContractID()
    {
        return mContractID;
    }

    TestAccount& getRoot();
    SorobanNetworkConfig const& getNetworkCfg();
    uint32_t getLedgerSeq();

    void deployWithResources(SorobanResources const& uploadResources,
                             SorobanResources const& createResources);
    TransactionFramePtr getDeployTxForMetaTest(TestAccount& acc);

    // The following computations are copies of compute_transaction_resource_fee
    // in rs-soroban-env/soroban-env-host/src/fees.rs. This is reimplemented
    // here so that we can check if the Cxx bridge introduced any fee related
    // bugs.
    int64_t getRentFeeForBytes(int64_t entrySize, uint32_t extendTo,
                               bool isPersistent);
    int64_t getTTLEntryWriteFee();
    int64_t getRentFeeForExtension(LedgerKey const& key, uint32_t newLifetime);

    // Fees that depend on TX size, historicalFee and bandwidthFee
    int64_t getTxSizeFees(TransactionFrameBasePtr tx);

    int64_t getComputeFee(SorobanResources const& resources);
    int64_t getEntryReadFee(SorobanResources const& resources);
    int64_t getEntryWriteFee(SorobanResources const& resources);
    int64_t getReadBytesFee(SorobanResources const& resources);
    int64_t getWriteBytesFee(SorobanResources const& resources);

    // Compute tx frame size before finalizing the resource fee via a dummy TX.
    // This is a bit hacky, but we need the exact tx size in order to
    // enable tests that rely on the exact refundable fee value.
    uint32_t computeResourceFee(SorobanResources const& resources,
                                SCSymbol const& functionName,
                                std::vector<SCVal> const& args);

    TransactionFrameBasePtr createUploadWasmTx(TestAccount& source,
                                               uint32_t resourceFee);
    TransactionFrameBasePtr createInvokeTx(SorobanResources const& resources,
                                           SCSymbol const& functionName,
                                           std::vector<SCVal> const& args,
                                           uint32_t inclusionFee,
                                           uint32_t resourceFee);
    TransactionFramePtr createInvokeTxForMetaTest(
        TestAccount& source, SorobanResources const& resources,
        SCSymbol const& functionName, std::vector<SCVal> const& args,
        uint32_t inclusionFee, uint32_t resourceFee);
    TransactionFrameBasePtr createExtendOpTx(SorobanResources const& resources,
                                             uint32_t extendTo, uint32_t fee,
                                             uint32_t refundableFee);
    TransactionFramePtr createExtendOpTxForMetaTest(
        TestAccount& source, SorobanResources const& resources,
        uint32_t extendTo, uint32_t fee, uint32_t refundableFee);
    TransactionFrameBasePtr createRestoreTx(SorobanResources const& resources,
                                            uint32_t fee,
                                            uint32_t refundableFee);
    TransactionFramePtr
    createRestoreTxForMetaTest(TestAccount& source,
                               SorobanResources const& resources, uint32_t fee,
                               uint32_t refundableFee);

    void txCheckValid(TransactionFrameBasePtr tx);
    bool isTxValid(TransactionFrameBasePtr tx);

    std::shared_ptr<TransactionMetaFrame>
    invokeTx(TransactionFrameBasePtr tx, bool expectSuccess,
             bool processPostApply = true);

    void checkTTL(LedgerKey const& k, uint32_t expectedLiveUntilLedger);
    bool isEntryLive(LedgerKey const& k, uint32_t ledgerSeq);

    void restoreOp(xdr::xvector<LedgerKey> const& readWrite,
                   int64_t expectedRefundableFeeCharged,
                   bool expectSuccess = true);
    void extendOp(xdr::xvector<LedgerKey> const& readOnly, uint32_t extendTo,
                  bool expectSuccess = true,
                  std::optional<uint32_t> expectedRefundableChargeOverride =
                      std::nullopt);
};

class ContractStorageInvocationTest : public ContractInvocationTest
{
  public:
    ContractStorageInvocationTest(Config cfg = getTestConfig())
        : ContractInvocationTest(rust_bridge::get_test_wasm_contract_data(),
                                 /*deployContract=*/true, cfg)
    {
    }

    void put(std::string const& key, ContractDataDurability type, uint64_t val,
             bool expectSuccess = true);
    void putWithFootprint(std::string const& key, ContractDataDurability type,
                          uint64_t val, xdr::xvector<LedgerKey> const& readOnly,
                          xdr::xvector<LedgerKey> const& readWrite,
                          bool expectSuccess, uint32_t writeBytes = 1000,
                          uint32_t refundableFee = 40'000);

    uint64_t get(std::string const& key, ContractDataDurability type,
                 bool expectSuccess = true);
    uint64_t getWithFootprint(std::string const& key,
                              ContractDataDurability type,
                              xdr::xvector<LedgerKey> const& readOnly,
                              xdr::xvector<LedgerKey> const& readWrite,
                              bool expectSuccess, uint32_t readBytes = 10'000);

    bool has(std::string const& key, ContractDataDurability type,
             bool expectSuccess = true);
    bool hasWithFootprint(std::string const& key, ContractDataDurability type,
                          xdr::xvector<LedgerKey> const& readOnly,
                          xdr::xvector<LedgerKey> const& readWrite,
                          bool expectSuccess);

    void del(std::string const& key, ContractDataDurability type);
    void delWithFootprint(std::string const& key, ContractDataDurability type,
                          xdr::xvector<LedgerKey> const& readOnly,
                          xdr::xvector<LedgerKey> const& readWrite,
                          bool expectSuccess);

    using ContractInvocationTest::checkTTL;
    void checkTTL(std::string const& key, ContractDataDurability type,
                  uint32_t expectedLiveUntilLedger);

    using ContractInvocationTest::isEntryLive;
    bool isEntryLive(std::string const& key, ContractDataDurability type,
                     uint32_t ledgerSeq);

    void extendHostFunction(std::string const& key, ContractDataDurability type,
                            uint32_t threshold, uint32_t extendTo,
                            bool expectSuccess = true);

    void resizeStorageAndExtend(std::string const& key, uint32_t numKiloBytes,
                                uint32_t thresh, uint32_t extendTo,
                                uint32_t writeBytes, uint32_t refundableFee,
                                bool expectSuccess);
};
}
}