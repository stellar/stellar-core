// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/RestoreFootprintOpFrame.h"
#include "TransactionUtils.h"
#include "bucket/BucketUtils.h"
#include "bucket/HotArchiveBucket.h"
#include "ledger/LedgerManagerImpl.h"
#include "ledger/LedgerTypeUtils.h"
#include "medida/meter.h"
#include "medida/timer.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/ParallelApplyUtils.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>

namespace stellar
{

static RestoreFootprintResult&
innerResult(OperationResult& res)
{
    return res.tr().restoreFootprintResult();
}

struct RestoreFootprintMetrics
{
    SorobanMetrics& mMetrics;

    uint32_t mLedgerReadByte{0};
    uint32_t mLedgerWriteByte{0};

    RestoreFootprintMetrics(SorobanMetrics& metrics) : mMetrics(metrics)
    {
    }

    ~RestoreFootprintMetrics()
    {
        mMetrics.mRestoreFpOpReadLedgerByte.Mark(mLedgerReadByte);
        mMetrics.mRestoreFpOpWriteLedgerByte.Mark(mLedgerWriteByte);
    }
    medida::TimerContext
    getExecTimer()
    {
        return mMetrics.mRestoreFpOpExec.TimeScope();
    }
};

RestoreFootprintOpFrame::RestoreFootprintOpFrame(
    Operation const& op, TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
    , mRestoreFootprintOp(mOperation.body.restoreFootprintOp())
{
}

class RestoreFootprintApplyHelper : virtual public LedgerAccessHelper
{

  protected:
    AppConnector& mApp;
    OperationResult& mRes;
    std::optional<RefundableFeeTracker>& mRefundableFeeTracker;
    OperationMetaBuilder& mOpMeta;
    RestoreFootprintOpFrame const& mOpFrame;

    SorobanResources const& mResources;
    SorobanNetworkConfig const& mSorobanConfig;
    Config const& mAppConfig;

    RestoreFootprintMetrics mMetrics;
    DiagnosticEventManager& mDiagnosticEvents;

  public:
    RestoreFootprintApplyHelper(
        AppConnector& app, OperationResult& res,
        std::optional<RefundableFeeTracker>& refundableFeeTracker,
        OperationMetaBuilder& opMeta, RestoreFootprintOpFrame const& opFrame)
        : mApp(app)
        , mRes(res)
        , mRefundableFeeTracker(refundableFeeTracker)
        , mOpMeta(opMeta)
        , mOpFrame(opFrame)
        , mResources(mOpFrame.mParentTx.sorobanResources())
        , mSorobanConfig(app.getSorobanNetworkConfigForApply())
        , mAppConfig(app.getConfig())
        , mMetrics(app.getSorobanMetrics())
        , mDiagnosticEvents(mOpMeta.getDiagnosticEventManager())
    {
    }

    virtual std::optional<LedgerEntry>
    getHotArchiveEntry(LedgerKey const& key) = 0;
    virtual bool entryWasRestored(LedgerKey const& key) = 0;
    virtual void restoreEntry(LedgerKey const& lk, LedgerEntry const& entry,
                              LedgerKey const& ttlKey,
                              std::optional<LedgerEntry> const& ttlEntryOpt,
                              uint32_t restoredLiveUntilLedger) = 0;

    virtual bool
    apply()
    {
        ZoneNamedN(applyZone, "RestoreFootprintOpFrame apply", true);
        auto timeScope = mMetrics.getExecTimer();

        auto const& resources = mOpFrame.mParentTx.sorobanResources();
        auto const& footprint = resources.footprint;
        auto ledgerSeq = getLedgerSeq();
        auto const& archivalSettings = mSorobanConfig.stateArchivalSettings();
        rust::Vec<CxxLedgerEntryRentChange> rustEntryRentChanges;
        // Extend the TTL on the restored entry to minimum TTL, including
        // the current ledger.
        uint32_t restoredLiveUntilLedger =
            ledgerSeq + archivalSettings.minPersistentTTL - 1;
        uint32_t ledgerVersion = getLedgerVersion();
        rustEntryRentChanges.reserve(footprint.readWrite.size());
        auto& diagnosticEvents = mOpMeta.getDiagnosticEventManager();

        for (auto const& lk : footprint.readWrite)
        {
            std::optional<LedgerEntry> hotArchiveEntryOpt = std::nullopt;
            auto ttlKey = getTTLKey(lk);
            auto ttlLeOpt = getLedgerEntryOpt(ttlKey);
            if (!ttlLeOpt)
            {
                // This entry has already been restored and then deleted.
                if (entryWasRestored(lk))
                {
                    continue;
                }
                hotArchiveEntryOpt = getHotArchiveEntry(lk);
                if (!hotArchiveEntryOpt)
                {
                    // Neither live nor hot entry exists, skip.
                    // (note: hot entry never exists in pre-23)
                    continue;
                }
            }
            else if (isLive(ttlLeOpt.value(), ledgerSeq))
            {
                // Skip entry if it's already live.
                continue;
            }

            // We should _either_ have an entry to restore from hot or live BL.
            releaseAssertOrThrow(hotArchiveEntryOpt || ttlLeOpt);

            // We must load the ContractCode/ContractData entry for fee
            // purposes, as restore is considered a write
            uint32_t entrySize = 0;
            LedgerEntry entry;
            if (hotArchiveEntryOpt)
            {
                entry = hotArchiveEntryOpt.value();

                // Update last modified ledger seq to the current ledger seq
                // since we're rewriting this entry. ltx will update this for
                // us, but we need to process the meta before ltx has a chance
                // for the update.
                entry.lastModifiedLedgerSeq = ledgerSeq;
                entrySize = static_cast<uint32>(xdr::xdr_size(entry));
            }
            else
            {
                auto entryLeOpt = getLedgerEntryOpt(lk);

                // We checked for TTLEntry existence above
                releaseAssertOrThrow(entryLeOpt);

                entry = *entryLeOpt;
                entrySize = static_cast<uint32>(xdr::xdr_size(entry));
            }

            mMetrics.mLedgerReadByte += entrySize;
            if (resources.diskReadBytes < mMetrics.mLedgerReadByte)
            {
                diagnosticEvents.pushError(
                    SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                    "operation byte-read resources exceeds amount specified",
                    {makeU64SCVal(mMetrics.mLedgerReadByte),
                     makeU64SCVal(resources.diskReadBytes)});
                innerResult(mRes).code(
                    RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }

            // To maintain consistency with InvokeHostFunction, TTLEntry
            // writes come out of refundable fee, so only add entrySize
            mMetrics.mLedgerWriteByte += entrySize;
            if (!validateContractLedgerEntry(lk, entrySize, mSorobanConfig,
                                             mAppConfig, mOpFrame.mParentTx,
                                             diagnosticEvents))
            {
                innerResult(mRes).code(
                    RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }

            if (resources.writeBytes < mMetrics.mLedgerWriteByte)
            {
                diagnosticEvents.pushError(
                    SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                    "operation byte-write resources exceeds amount specified",
                    {makeU64SCVal(mMetrics.mLedgerWriteByte),
                     makeU64SCVal(resources.writeBytes)});
                innerResult(mRes).code(
                    RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }

            rustEntryRentChanges.emplace_back(
                createEntryRentChangeWithoutModification(
                    entry, entrySize,
                    /*entryLiveUntilLedger=*/std::nullopt,
                    /*newLiveUntilLedger=*/restoredLiveUntilLedger,
                    ledgerVersion, mAppConfig, mSorobanConfig));

            restoreEntry(lk, entry, ttlKey, ttlLeOpt, restoredLiveUntilLedger);
        }

        int64_t rentFee = rust_bridge::compute_rent_fee(
            mAppConfig.CURRENT_LEDGER_PROTOCOL_VERSION, ledgerVersion,
            rustEntryRentChanges,
            mSorobanConfig.rustBridgeRentFeeConfiguration(), ledgerSeq);
        if (!mRefundableFeeTracker->consumeRefundableSorobanResources(
                0, rentFee, getLedgerVersion(), mSorobanConfig, mAppConfig,
                mOpFrame.mParentTx, mDiagnosticEvents))
        {
            innerResult(mRes).code(
                RESTORE_FOOTPRINT_INSUFFICIENT_REFUNDABLE_FEE);
            return false;
        }
        innerResult(mRes).code(RESTORE_FOOTPRINT_SUCCESS);
        return true;
    }
};

class RestoreFootprintPreV23ApplyHelper
    : virtual public RestoreFootprintApplyHelper,
      virtual public PreV23LedgerAccessHelper
{
  public:
    RestoreFootprintPreV23ApplyHelper(
        AppConnector& app, AbstractLedgerTxn& ltx, OperationResult& res,
        std::optional<RefundableFeeTracker>& refundableFeeTracker,
        OperationMetaBuilder& opMeta, RestoreFootprintOpFrame const& opFrame)
        : RestoreFootprintApplyHelper(app, res, refundableFeeTracker, opMeta,
                                      opFrame)
        , PreV23LedgerAccessHelper(ltx)
    {
    }

    std::optional<LedgerEntry>
    getHotArchiveEntry(LedgerKey const& key) override
    {
        // There is no hot archive pre-23.
        return std::nullopt;
    }
    bool
    entryWasRestored(LedgerKey const& key) override
    {
        // NB: even though pre-23 can answer this question precisely, the code
        // in pre-23 wasn't sensitive to it, so we preserve that functionality.
        return false;
    }
    void
    restoreEntry(LedgerKey const& lk, LedgerEntry const& entry_,
                 LedgerKey const& ttlKey,
                 std::optional<LedgerEntry> const& ttlEntryOpt,
                 uint32_t restoredLiveUntilLedger) override
    {
        // Entry exists in the live BucketList if we get to this point due to
        // the constTTLLtxe loadWithoutRecord logic above. Get the actual ledger
        // entry (since we know it exists already at this point)
        //
        // NB: this method is given a parameter (entry_) that is the result of
        // loadWithoutRecord, which is apparently not quite good enough: we do
        // _exactly_ what the old code did here and make an additional call to
        // mLtx->getNewestVersion, presumably to actually activate the restored
        // entry in the current ltx (uncertain, it might just have been a quirk
        // in the original code).
        auto entry = mLtx.getNewestVersion(lk);
        releaseAssertOrThrow(entry);
        mLtx.restoreFromLiveBucketList(entry->ledgerEntry(),
                                       restoredLiveUntilLedger);
    }
};

class RestoreFootprintParallelApplyHelper
    : virtual public RestoreFootprintApplyHelper,
      virtual public ParallelLedgerAccessHelper
{
    SearchableHotArchiveSnapshotConstPtr mHotArchive;

  public:
    RestoreFootprintParallelApplyHelper(
        AppConnector& app, ThreadParallelApplyLedgerState const& threadState,
        ParallelLedgerInfo const& ledgerInfo, OperationResult& res,
        std::optional<RefundableFeeTracker>& refundableFeeTracker,
        OperationMetaBuilder& opMeta, RestoreFootprintOpFrame const& opFrame)
        : RestoreFootprintApplyHelper(app, res, refundableFeeTracker, opMeta,
                                      opFrame)
        , ParallelLedgerAccessHelper(threadState, ledgerInfo,
                                     app.copySearchableLiveBucketListSnapshot())
        , mHotArchive(app.copySearchableHotArchiveBucketListSnapshot())
    {
    }

    std::optional<LedgerEntry>
    getHotArchiveEntry(LedgerKey const& key) override
    {
        auto ptr = mHotArchive->load(key);
        if (ptr)
        {
            return ptr->archivedEntry();
        }
        else
        {
            return std::nullopt;
        }
    }

    bool
    entryWasRestored(LedgerKey const& key) override
    {
        return mOpState.entryWasRestored(key);
    }

    void
    restoreEntry(LedgerKey const& lk, LedgerEntry const& entry,
                 LedgerKey const& ttlKey,
                 std::optional<LedgerEntry> const& ttlEntryOpt,
                 uint32_t restoredLiveUntilLedger) override
    {
        if (!ttlEntryOpt)
        {
            // No TTL entry opt
            //   => we are _not_ doing a live restore
            //   => we _are_ doing a hot archive restore
            mOpState.upsertEntry(lk, entry);
            LedgerEntry ttlEntry =
                getTTLEntryForTTLKey(ttlKey, restoredLiveUntilLedger);
            mOpState.upsertEntry(ttlKey, ttlEntry);
            mOpState.addHotArchiveRestore(lk, entry, ttlKey, ttlEntry);
        }
        else
        {
            // TTL entry opt
            //   => a live restore
            //   => just upsert the updated TTL
            LedgerEntry ttlEntry = ttlEntryOpt.value();
            ttlEntry.data.ttl().liveUntilLedgerSeq = restoredLiveUntilLedger;
            mOpState.upsertEntry(ttlKey, ttlEntry);
            mOpState.addLiveBucketlistRestore(lk, entry, ttlKey, ttlEntry);
        }
    }

    ParallelTxReturnVal
    takeResults(bool applySucceeded)
    {
        if (applySucceeded)
        {
            return mOpState.takeSuccess();
        }
        else
        {
            return mOpState.takeFailure();
        }
    }
};

bool
RestoreFootprintOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return protocolVersionStartsFrom(header.ledgerVersion,
                                     SOROBAN_PROTOCOL_VERSION);
}

ParallelTxReturnVal
RestoreFootprintOpFrame::doParallelApply(
    AppConnector& app, ThreadParallelApplyLedgerState const& threadState,
    Config const& appConfig, SorobanNetworkConfig const& sorobanConfig,
    Hash const& txPrngSeed, ParallelLedgerInfo const& ledgerInfo,
    SorobanMetrics& sorobanMetrics, OperationResult& res,
    std::optional<RefundableFeeTracker>& refundableFeeTracker,
    OperationMetaBuilder& opMeta) const
{

    releaseAssertOrThrow(
        protocolVersionStartsFrom(ledgerInfo.getLedgerVersion(),
                                  PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION));
    releaseAssertOrThrow(refundableFeeTracker);
    RestoreFootprintParallelApplyHelper helper(
        app, threadState, ledgerInfo, res, refundableFeeTracker, opMeta, *this);
    bool success = helper.apply();
    return helper.takeResults(success);
}

bool
RestoreFootprintOpFrame::doApply(
    AppConnector& app, AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
    OperationResult& res,
    std::optional<RefundableFeeTracker>& refundableFeeTracker,
    OperationMetaBuilder& opMeta) const
{
    releaseAssertOrThrow(
        protocolVersionIsBefore(ltx.loadHeader().current().ledgerVersion,
                                PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION));
    releaseAssertOrThrow(refundableFeeTracker);
    RestoreFootprintPreV23ApplyHelper helper(
        app, ltx, res, refundableFeeTracker, opMeta, *this);
    return helper.apply();
}

bool
RestoreFootprintOpFrame::doCheckValidForSoroban(
    SorobanNetworkConfig const& networkConfig, Config const& appConfig,
    uint32_t ledgerVersion, OperationResult& res,
    DiagnosticEventManager& diagnosticEvents) const
{
    auto const& footprint = mParentTx.sorobanResources().footprint;
    if (!footprint.readOnly.empty())
    {
        innerResult(res).code(RESTORE_FOOTPRINT_MALFORMED);
        diagnosticEvents.pushError(
            SCE_STORAGE, SCEC_INVALID_INPUT,
            "read-only footprint must be empty for RestoreFootprint operation",
            {});
        return false;
    }

    for (auto const& lk : footprint.readWrite)
    {
        if (!isPersistentEntry(lk))
        {
            innerResult(res).code(RESTORE_FOOTPRINT_MALFORMED);
            diagnosticEvents.pushError(
                SCE_STORAGE, SCEC_INVALID_INPUT,
                "only persistent Soroban entries can be restored", {});
            return false;
        }
    }

    return true;
}

bool
RestoreFootprintOpFrame::doCheckValid(uint32_t ledgerVersion,
                                      OperationResult& res) const
{
    throw std::runtime_error(
        "RestoreFootprintOpFrame::doCheckValid needs Config");
}

void
RestoreFootprintOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
}

bool
RestoreFootprintOpFrame::isSoroban() const
{
    return true;
}

ThresholdLevel
RestoreFootprintOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::LOW;
}
}
