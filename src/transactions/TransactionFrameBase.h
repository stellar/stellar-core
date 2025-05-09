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

class MutableTransactionResultBase;
using MutableTxResultPtr = std::shared_ptr<MutableTransactionResultBase>;

class TxEventManager;
struct DiagnosticEventBuffer;

class TransactionFrameBase;
using TransactionFrameBasePtr = std::shared_ptr<TransactionFrameBase const>;
using TransactionFrameBaseConstPtr =
    std::shared_ptr<TransactionFrameBase const>;

using ModifiedEntryMap = UnorderedMap<LedgerKey, std::optional<LedgerEntry>>;

struct ThreadEntry
{
    // Will not be set if the entry doesn't exist, or if no tx was able to load
    // it due to hitting read limits.
    std::optional<LedgerEntry> mLedgerEntry;
    bool isDirty;
};

using ThreadEntryMap = UnorderedMap<LedgerKey, ThreadEntry>;

class ParallelTxReturnVal
{
  public:
    ParallelTxReturnVal(bool success, ModifiedEntryMap const&& modifiedEntryMap)
        : mSuccess(success), mModifiedEntryMap(std::move(modifiedEntryMap))
    {
    }
    ParallelTxReturnVal(bool success, ModifiedEntryMap const&& modifiedEntryMap,
                        RestoredKeys const&& restoredKeys)
        : mSuccess(success)
        , mModifiedEntryMap(std::move(modifiedEntryMap))
        , mRestoredKeys(std::move(restoredKeys))
    {
    }

    bool
    getSuccess() const
    {
        return mSuccess;
    }
    ModifiedEntryMap const&
    getModifiedEntryMap() const
    {
        return mModifiedEntryMap;
    }
    RestoredKeys const&
    getRestoredKeys() const
    {
        return mRestoredKeys;
    }

  private:
    bool mSuccess;
    // This will contain a key for every entry modified by a transaction
    ModifiedEntryMap mModifiedEntryMap;
    RestoredKeys mRestoredKeys;
};

class ParallelLedgerInfo
{

  public:
    ParallelLedgerInfo(uint32_t version, uint32_t seq, uint32_t reserve,
                       TimePoint time, Hash id)
        : ledgerVersion(version)
        , ledgerSeq(seq)
        , baseReserve(reserve)
        , closeTime(time)
        , networkID(id)
    {
    }

    uint32_t
    getLedgerVersion() const
    {
        return ledgerVersion;
    }
    uint32_t
    getLedgerSeq() const
    {
        return ledgerSeq;
    }
    uint32_t
    getBaseReserve() const
    {
        return baseReserve;
    }
    TimePoint
    getCloseTime() const
    {
        return closeTime;
    }
    Hash
    getNetworkID() const
    {
        return networkID;
    }

  private:
    uint32_t ledgerVersion;
    uint32_t ledgerSeq;
    uint32_t baseReserve;
    TimePoint closeTime;
    Hash networkID;
};

class TxEffects
{
  public:
    TxEffects(uint32_t ledgerVersion, Config const& config,
              Hash const& networkID)
        : mMeta(ledgerVersion, config)
        , mEventManager(ledgerVersion, networkID, config)
    {
    }

    TransactionMetaFrame&
    getMeta()
    {
        return mMeta;
    }
    LedgerTxnDelta&
    getDelta()
    {
        return mDelta;
    }
    TxEventManager&
    getTxEventManager()
    {
        return mEventManager;
    }

  private:
    TransactionMetaFrame mMeta;
    LedgerTxnDelta mDelta;
    TxEventManager mEventManager;
};

class TxBundle
{
  public:
    TxBundle(TransactionFrameBasePtr tx, MutableTxResultPtr resPayload,
             Config const& config, Hash const& networkID,
             uint32_t ledgerVersion, uint64_t txNum)
        : tx(tx)
        , resPayload(resPayload)
        , txNum(txNum)
        , mEffects(new TxEffects(ledgerVersion, config, networkID))
    {
    }

    TransactionFrameBasePtr
    getTx() const
    {
        return tx;
    }
    MutableTxResultPtr
    getResPayload() const
    {
        return resPayload;
    }
    uint64_t
    getTxNum() const
    {
        return txNum;
    }
    TxEffects&
    getEffects() const
    {
        return *mEffects;
    }

  private:
    TransactionFrameBasePtr tx;
    MutableTxResultPtr resPayload;
    uint64_t txNum;
    std::unique_ptr<TxEffects> mEffects;
};

typedef std::vector<TxBundle> Cluster;
typedef UnorderedMap<LedgerKey, TTLEntry> TTLs;

class ApplyStage
{
  public:
    ApplyStage(std::vector<Cluster>&& clusters) : mClusters(std::move(clusters))
    {
    }

    class Iterator
    {
      public:
        using value_type = TxBundle;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type&;
        using iterator_category = std::forward_iterator_tag;

        TxBundle const& operator*() const;

        Iterator& operator++();
        Iterator operator++(int);

        bool operator==(Iterator const& other) const;
        bool operator!=(Iterator const& other) const;

      private:
        friend class ApplyStage;

        Iterator(std::vector<Cluster> const& clusters, size_t clusterIndex);
        std::vector<Cluster> const& mClusters;
        size_t mClusterIndex = 0;
        size_t mTxIndex = 0;
    };
    Iterator begin() const;
    Iterator end() const;

    Cluster const& getCluster(size_t i) const;
    size_t numClusters() const;

  private:
    std::vector<Cluster> mClusters;
};

class TransactionFrameBase
{
  public:
    static TransactionFrameBasePtr
    makeTransactionFromWire(Hash const& networkID,
                            TransactionEnvelope const& env);

    virtual bool apply(AppConnector& app, AbstractLedgerTxn& ltx,
                       TransactionMetaFrame& meta, MutableTxResultPtr txResult,
                       TxEventManager& txEventManager,
                       Hash const& sorobanBasePrngSeed = Hash{}) const = 0;

    virtual void preloadEntriesForParallelApply(
        AppConnector& app, SorobanMetrics& sorobanMetrics,
        AbstractLedgerTxn& ltx, ThreadEntryMap& entryMap,
        MutableTxResultPtr txResult, DiagnosticEventBuffer& buffer) const = 0;
    virtual void preParallelApply(AppConnector& app, AbstractLedgerTxn& ltx,
                                  TransactionMetaFrame& meta,
                                  MutableTxResultPtr resPayload,
                                  TxEventManager& txEventManager) const = 0;

    virtual ParallelTxReturnVal parallelApply(
        AppConnector& app,
        ThreadEntryMap const& entryMap, // Must not be shared between threads!,
        Config const& config, SorobanNetworkConfig const& sorobanConfig,
        ParallelLedgerInfo const& ledgerInfo, MutableTxResultPtr resPayload,
        SorobanMetrics& sorobanMetrics, Hash const& sorobanBasePrngSeed,
        TxEffects& effects) const = 0;

    virtual MutableTxResultPtr
    checkValid(AppConnector& app, LedgerSnapshot const& ls,
               SequenceNumber current, uint64_t lowerBoundCloseTimeOffset,
               uint64_t upperBoundCloseTimeOffset,
               DiagnosticEventBuffer* diagnosticEvents = nullptr) const = 0;
    virtual bool checkSorobanResourceAndSetError(
        AppConnector& app, SorobanNetworkConfig const& cfg,
        uint32_t ledgerVersion, MutableTxResultPtr txResult,
        DiagnosticEventBuffer* diagnosticEvents) const = 0;

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
                                  MutableTxResultPtr txResult,
                                  TxEventManager& txEventManager) const = 0;

    virtual std::shared_ptr<StellarMessage const> toStellarMessage() const = 0;

    virtual bool hasDexOperations() const = 0;

    virtual bool isSoroban() const = 0;
    virtual SorobanResources const& sorobanResources() const = 0;
    virtual int64 declaredSorobanResourceFee() const = 0;
    virtual bool XDRProvidesValidFee() const = 0;
};
}
