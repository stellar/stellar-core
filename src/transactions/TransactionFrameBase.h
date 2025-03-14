// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <optional>

#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerStateSnapshot.h"
#include "ledger/NetworkConfig.h"
#include "main/Config.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionMetaFrame.h"
#include "util/TxResource.h"
#include "util/UnorderedSet.h"
#include "util/types.h"
#include <optional>

namespace stellar
{
class AbstractLedgerTxn;
class Application;
class Database;
class OperationFrame;
class TransactionFrame;
class FeeBumpTransactionFrame;
class AppConnector;

class MutableTransactionResultBase;
using MutableTxResultPtr = std::shared_ptr<MutableTransactionResultBase>;

class TransactionFrameBase;
using TransactionFrameBasePtr = std::shared_ptr<TransactionFrameBase const>;
using TransactionFrameBaseConstPtr =
    std::shared_ptr<TransactionFrameBase const>;

// TODO: Explain why this exists. Allows common validation flows between "apply"
// and "validate" to work whether given an immutable snapshot (for use in
// "validate") or a wrapper around the AppConnector (for use in "apply").
// AppValidationWrapper is also usable outside of "apply" by setting "forApply"
// to false. This is useful for some of our testing infrastructure.
class ValidationConnector
{
  public:
    virtual ~ValidationConnector() = default;
    virtual Config const& getConfig() const = 0;
    virtual SorobanNetworkConfig const& getSorobanNetworkConfig() const = 0;
    virtual uint32_t getCurrentProtocolVersion() const = 0;
};

// TODO: Docs
class AppValidationWrapper : public ValidationConnector
{
  public:
    explicit AppValidationWrapper(AppConnector const& app, bool forApply,
                                  std::optional<uint32_t> protocolVersion);
    ~AppValidationWrapper() override = default;

    Config const& getConfig() const override;
    SorobanNetworkConfig const& getSorobanNetworkConfig() const override;
    uint32_t getCurrentProtocolVersion() const override;

  private:
    AppConnector const& mApp;
    bool const mForApply;
    std::optional<uint32_t> const mProtocolVersion;
};

// TODO: Docs
class ImmutableValidationSnapshot : public ValidationConnector
{
  public:
    explicit ImmutableValidationSnapshot(AppConnector const& app);
    ~ImmutableValidationSnapshot() override = default;

    Config const& getConfig() const override;
    SorobanNetworkConfig const& getSorobanNetworkConfig() const override;
    uint32_t getCurrentProtocolVersion() const override;

  private:
    std::shared_ptr<const Config> const mConfig;
    std::optional<const SorobanNetworkConfig> const mSorobanNetworkConfig;
    uint32_t const mCurrentProtocolVersion;
};

// TODO: Rename to indicate constness / Immutableness?
using ImmutableValidationSnapshotPtr =
    std::shared_ptr<ImmutableValidationSnapshot const>;

class TransactionFrameBase
{
  public:
    static TransactionFrameBasePtr
    makeTransactionFromWire(Hash const& networkID,
                            TransactionEnvelope const& env);

    virtual bool apply(AppConnector& app, AbstractLedgerTxn& ltx,
                       TransactionMetaFrame& meta, MutableTxResultPtr txResult,
                       Hash const& sorobanBasePrngSeed = Hash{}) const = 0;
    virtual MutableTxResultPtr
    checkValid(ValidationConnector const& vc, LedgerSnapshot const& ls,
               SequenceNumber current, uint64_t lowerBoundCloseTimeOffset,
               uint64_t upperBoundCloseTimeOffset) const = 0;
    virtual bool
    checkSorobanResourceAndSetError(ValidationConnector const& vc,
                                    MutableTxResultPtr txResult) const = 0;

    virtual MutableTxResultPtr createSuccessResult() const = 0;

    virtual MutableTxResultPtr
    createSuccessResultWithFeeCharged(LedgerHeader const& header,
                                      std::optional<int64_t> baseFee,
                                      bool applying) const = 0;

    virtual TransactionEnvelope const& getEnvelope() const = 0;

#ifdef BUILD_TESTS
    virtual TransactionEnvelope& getMutableEnvelope() const = 0;
    virtual void clearCached() const = 0;
    virtual bool isTestTx() const = 0;
#endif

    // Returns the total fee of this transaction, including the 'flat',
    // non-market part.
    virtual int64_t getFullFee() const = 0;
    // Returns the part of the full fee used to make decisions as to
    // whether this transaction should be included into ledger.
    virtual int64_t getInclusionFee() const = 0;
    virtual int64_t getFee(LedgerHeader const& header,
                           std::optional<int64_t> baseFee,
                           bool applying) const = 0;

    virtual Hash const& getContentsHash() const = 0;
    virtual Hash const& getFullHash() const = 0;

    virtual uint32_t getNumOperations() const = 0;
    virtual Resource getResources(bool useByteLimitInClassic) const = 0;

    virtual std::vector<Operation> const& getRawOperations() const = 0;

    virtual SequenceNumber getSeqNum() const = 0;
    virtual AccountID getFeeSourceID() const = 0;
    virtual AccountID getSourceID() const = 0;
    virtual std::optional<SequenceNumber const> const getMinSeqNum() const = 0;
    virtual Duration getMinSeqAge() const = 0;
    virtual uint32 getMinSeqLedgerGap() const = 0;

    virtual void
    insertKeysForFeeProcessing(UnorderedSet<LedgerKey>& keys) const = 0;
    virtual void insertKeysForTxApply(UnorderedSet<LedgerKey>& keys,
                                      LedgerKeyMeter* lkMeter) const = 0;

    virtual MutableTxResultPtr
    processFeeSeqNum(AbstractLedgerTxn& ltx,
                     std::optional<int64_t> baseFee) const = 0;

    virtual void processPostApply(AppConnector& app, AbstractLedgerTxn& ltx,
                                  TransactionMetaFrame& meta,
                                  MutableTxResultPtr txResult) const = 0;

    virtual std::shared_ptr<StellarMessage const> toStellarMessage() const = 0;

    virtual bool hasDexOperations() const = 0;

    virtual bool isSoroban() const = 0;
    virtual SorobanResources const& sorobanResources() const = 0;
    virtual int64 declaredSorobanResourceFee() const = 0;
    virtual bool XDRProvidesValidFee() const = 0;
};
}
