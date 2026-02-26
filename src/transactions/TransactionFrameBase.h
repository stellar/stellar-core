// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <optional>

#include "ledger/LedgerEntryScope.h"
#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerStateSnapshot.h"
#include "ledger/NetworkConfig.h"
#include "main/Config.h"
#include "overlay/StellarXDR.h"
#include "util/TxResource.h"
#include "util/UnorderedSet.h"
#include "util/types.h"
#include <optional>

#include "ledger/SorobanMetrics.h"

namespace stellar
{
class AbstractLedgerTxn;
class Application;
class Database;
class OperationFrame;
class TransactionFrame;
class FeeBumpTransactionFrame;
class AppConnector;
class SignatureChecker;
class ParallelLedgerInfo;
class TxEffects;
class ThreadParallelApplyLedgerState;

class MutableTransactionResultBase;
using MutableTxResultPtr = std::unique_ptr<MutableTransactionResultBase>;

class TransactionMetaBuilder;
class DiagnosticEventManager;

class TransactionFrameBase;
using TransactionFrameBasePtr = std::shared_ptr<TransactionFrameBase const>;
using TransactionFrameBaseConstPtr =
    std::shared_ptr<TransactionFrameBase const>;

// Tracks entry updates within a transaction during parallel apply phases. If
// the transaction succeeds, the thread's ParallelApplyEntryMap should be
// updated with the entries from the TxModifiedEntryMap.
using TxParApplyLedgerEntry =
    ScopedLedgerEntry<StaticLedgerEntryScope::TxParApply>;
using TxModifiedEntryMap = UnorderedMap<LedgerKey, TxParApplyLedgerEntryOpt>;

// Used to track the current state of an entry during parallel apply phases. Can
// be updated by successful transactions.
template <StaticLedgerEntryScope S> struct ParallelApplyEntry
{
    // Will not be set if the entry doesn't exist, or if no tx was able to load
    // it due to hitting read limits.
    ScopedLedgerEntryOpt<S> mLedgerEntry;
    bool mIsDirty;
    static ParallelApplyEntry
    clean(ScopedLedgerEntryOpt<S> const& e)
    {
        return ParallelApplyEntry{e, false};
    }
    static ParallelApplyEntry
    dirty(ScopedLedgerEntryOpt<S> const& e)
    {
        return ParallelApplyEntry{e, true};
    }
    template <StaticLedgerEntryScope S2>
    ParallelApplyEntry<S2>
    rescope(LedgerEntryScope<S> const& s1, LedgerEntryScope<S2> const& s2) const
    {
        auto adoptedEntry = s2.scopeAdoptEntryOptFrom(mLedgerEntry, s1);
        return ParallelApplyEntry<S2>{adoptedEntry, mIsDirty};
    }
};
using GlobalParallelApplyEntry =
    ParallelApplyEntry<StaticLedgerEntryScope::GlobalParApply>;
using ThreadParallelApplyEntry =
    ParallelApplyEntry<StaticLedgerEntryScope::ThreadParApply>;
using TxParallelApplyEntry =
    ParallelApplyEntry<StaticLedgerEntryScope::TxParApply>;

// This is a map of all entries that will be read and/or written during parallel
// apply phases: there is one such "global" map which disjoint per-thread maps
// get split off of, modified during applyThread, and merged back into. Once all
// threads return, the updates from each threads entry map should be committed
// to LedgerTxn.
template <StaticLedgerEntryScope S>
using ParallelApplyEntryMap = UnorderedMap<LedgerKey, ParallelApplyEntry<S>>;
using GlobalParallelApplyEntryMap =
    ParallelApplyEntryMap<StaticLedgerEntryScope::GlobalParApply>;
using ThreadParallelApplyEntryMap =
    ParallelApplyEntryMap<StaticLedgerEntryScope::ThreadParApply>;
using TxParallelApplyEntryMap =
    ParallelApplyEntryMap<StaticLedgerEntryScope::TxParApply>;

// Returned by each parallel transaction on success. It will contain the entries
// modified by the transaction and the keys restored.
class ParallelTxSuccessVal
    : public LedgerEntryScope<StaticLedgerEntryScope::TxParApply>
{
  public:
    ParallelTxSuccessVal(TxModifiedEntryMap&& modifiedEntryMap,
                         ScopeIdT txScopeID)
        : LedgerEntryScope(txScopeID)
        , mModifiedEntryMap(std::move(modifiedEntryMap))
    {
        // The ModifiedEntryMap should not be used for reading entries, only
        // to serve as a source for thread state to scopeAdoptEntryFrom. So
        // we deactivate ourselves as a LedgerEntryScope on construction, to
        // prevent accidental reads.
        scopeDeactivate();
    }
    ParallelTxSuccessVal(TxModifiedEntryMap&& modifiedEntryMap,
                         RestoredEntries&& restoredEntries, ScopeIdT txScopeID)
        : LedgerEntryScope(txScopeID)
        , mModifiedEntryMap(std::move(modifiedEntryMap))
        , mRestoredEntries(std::move(restoredEntries))
    {
        scopeDeactivate();
    }

    TxModifiedEntryMap const&
    getModifiedEntryMap() const
    {
        return mModifiedEntryMap;
    }
    RestoredEntries const&
    getRestoredEntries() const
    {
        return mRestoredEntries;
    }

    friend class TxParallelApplyLedgerState;

  private:
    // This will contain a key for every entry modified by a transaction
    TxModifiedEntryMap mModifiedEntryMap;
    RestoredEntries mRestoredEntries;
};

class TransactionFrameBase
{
  public:
    static TransactionFrameBasePtr
    makeTransactionFromWire(Hash const& networkID,
                            TransactionEnvelope const& env);

    virtual bool
    apply(AppConnector& app, AbstractLedgerTxn& ltx,
          TransactionMetaBuilder& meta, MutableTransactionResultBase& txResult,
          std::optional<SorobanNetworkConfig const> const& sorobanConfig,
          Hash const& sorobanBasePrngSeed) const = 0;

    virtual void
    preParallelApply(AppConnector& app, AbstractLedgerTxn& ltx,
                     TransactionMetaBuilder& meta,
                     MutableTransactionResultBase& txResult,
                     SorobanNetworkConfig const& sorobanConfig) const = 0;

    // If the transaction fails during parallel apply, returns std::nullopt.
    // Otherwise returns a ParallelTxSuccessVal containing the modified entries
    // and restored keys.
    virtual std::optional<ParallelTxSuccessVal> parallelApply(
        AppConnector& app, ThreadParallelApplyLedgerState const& threadState,
        Config const& config, ParallelLedgerInfo const& ledgerInfo,
        MutableTransactionResultBase& resPayload,
        SorobanMetrics& sorobanMetrics, Hash const& sorobanBasePrngSeed,
        TxEffects& effects) const = 0;

    virtual MutableTxResultPtr
    checkValid(AppConnector& app, LedgerSnapshot const& ls,
               SequenceNumber current, uint64_t lowerBoundCloseTimeOffset,
               uint64_t upperBoundCloseTimeOffset,
               DiagnosticEventManager& diagnosticEvents) const = 0;
    virtual bool
    checkSorobanResources(SorobanNetworkConfig const& cfg,
                          uint32_t ledgerVersion,
                          DiagnosticEventManager& diagnosticEvents) const = 0;

    virtual MutableTxResultPtr
    createTxErrorResult(TransactionResultCode txErrorCode) const = 0;

    virtual MutableTxResultPtr createValidationSuccessResult() const = 0;

    virtual TransactionEnvelope const& getEnvelope() const = 0;

    virtual bool checkSignature(SignatureChecker& signatureChecker,
                                LedgerEntryWrapper const& account,
                                int32_t neededWeight) const = 0;

    // Validate signatures for all operations in this transaction. Callers may
    // set `txResult` to `nullptr` if they don't need results (e.g. when
    // populating signature cache in the background).
    virtual bool
    checkOperationSignatures(SignatureChecker& signatureChecker,
                             LedgerSnapshot const& ls,
                             MutableTransactionResultBase* txResult) const = 0;

    // Validate all transaction-level signatures
    virtual bool
    checkAllTransactionSignatures(SignatureChecker& signatureChecker,
                                  LedgerEntryWrapper const& sourceAccount,
                                  uint32_t ledgerVersion) const = 0;

#ifdef BUILD_TESTS
    virtual TransactionEnvelope& getMutableEnvelope() const = 0;
    virtual void clearCached() const = 0;
    virtual bool isTestTx() const = 0;
#endif

    virtual bool validateSorobanTxForFlooding(
        UnorderedSet<LedgerKey> const& keysToFilter) const = 0;
    virtual bool validateSorobanMemo() const = 0;
    virtual bool validateHostFn() const = 0;

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
    virtual std::vector<std::shared_ptr<OperationFrame const>> const&
    getOperationFrames() const = 0;
    virtual Resource getResources(bool useByteLimitInClassic,
                                  uint32_t ledgerVersion) const = 0;

    virtual std::vector<Operation> const& getRawOperations() const = 0;

    virtual SequenceNumber getSeqNum() const = 0;
    virtual AccountID getFeeSourceID() const = 0;
    virtual AccountID getSourceID() const = 0;
    virtual std::optional<SequenceNumber const> const getMinSeqNum() const = 0;
    virtual Duration getMinSeqAge() const = 0;
    virtual uint32 getMinSeqLedgerGap() const = 0;

    virtual void
    insertKeysForFeeProcessing(UnorderedSet<LedgerKey>& keys) const = 0;
    virtual void insertKeysForTxApply(UnorderedSet<LedgerKey>& keys) const = 0;

    virtual MutableTxResultPtr
    processFeeSeqNum(AbstractLedgerTxn& ltx,
                     std::optional<int64_t> baseFee) const = 0;

    // After this transaction has been applied
    virtual void
    processPostApply(AppConnector& app, AbstractLedgerTxn& ltx,
                     TransactionMetaBuilder& meta,
                     MutableTransactionResultBase& txResult) const = 0;

    // After all transactions have been applied
    virtual void
    processPostTxSetApply(AppConnector& app, AbstractLedgerTxn& ltx,
                          MutableTransactionResultBase& txResult,
                          TxEventManager& txEventManager) const = 0;

    virtual std::shared_ptr<StellarMessage const> toStellarMessage() const = 0;

    virtual bool hasDexOperations() const = 0;

    virtual bool isSoroban() const = 0;
    virtual SorobanResources const& sorobanResources() const = 0;
    virtual SorobanTransactionData::_ext_t const& getResourcesExt() const = 0;
    virtual int64 declaredSorobanResourceFee() const = 0;
    virtual bool XDRProvidesValidFee() const = 0;

    // Invoke `fn` with the transaction's inner transaction, if any exists.
    // Otherwise this function does nothing.
    virtual void
    withInnerTx(std::function<void(TransactionFrameBaseConstPtr)> fn) const = 0;

    // Returns true if this TX is a soroban transaction with a
    // RestoreFootprintOp.
    virtual bool isRestoreFootprintTx() const = 0;

    virtual ~TransactionFrameBase() = default;
};
}
