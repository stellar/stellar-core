// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ExtendFootprintTTLOpFrame.h"
#include "TransactionUtils.h"
#include "ledger/LedgerManagerImpl.h"
#include "ledger/LedgerTypeUtils.h"
#include "medida/meter.h"
#include "medida/timer.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/ParallelApplyUtils.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>

namespace stellar
{

static ExtendFootprintTTLResult&
innerResult(OperationResult& res)
{
    return res.tr().extendFootprintTTLResult();
}

struct ExtendFootprintTTLMetrics
{
    SorobanMetrics& mMetrics;

    uint32 mLedgerReadByte{0};

    ExtendFootprintTTLMetrics(SorobanMetrics& metrics) : mMetrics(metrics)
    {
    }

    ~ExtendFootprintTTLMetrics()
    {
        mMetrics.mExtFpTtlOpReadLedgerByte.Mark(mLedgerReadByte);
    }
    medida::TimerContext
    getExecTimer()
    {
        return mMetrics.mExtFpTtlOpExec.TimeScope();
    }
};

ExtendFootprintTTLOpFrame::ExtendFootprintTTLOpFrame(
    Operation const& op, TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
    , mExtendFootprintTTLOp(mOperation.body.extendFootprintTTLOp())
{
}

bool
ExtendFootprintTTLOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return protocolVersionStartsFrom(header.ledgerVersion,
                                     SOROBAN_PROTOCOL_VERSION);
}

class ExtendFootprintTTLApplyHelper : virtual public LedgerAccessHelper
{

  protected:
    AppConnector& mApp;
    OperationResult& mRes;
    std::optional<RefundableFeeTracker>& mRefundableFeeTracker;
    OperationMetaBuilder& mOpMeta;
    ExtendFootprintTTLOpFrame const& mOpFrame;

    SorobanResources const& mResources;
    SorobanNetworkConfig const& mSorobanConfig;
    Config const& mAppConfig;

    ExtendFootprintTTLMetrics mMetrics;
    DiagnosticEventManager& mDiagnosticEvents;

  public:
    ExtendFootprintTTLApplyHelper(
        AppConnector& app, OperationResult& res,
        std::optional<RefundableFeeTracker>& refundableFeeTracker,
        OperationMetaBuilder& opMeta, ExtendFootprintTTLOpFrame const& opFrame)
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

    virtual bool checkReadBytesResourceLimit(uint32_t entrySize) = 0;

    virtual bool
    apply()
    {
        ZoneNamedN(applyZone, "ExtendFootprintTTLOpFrame apply", true);
        releaseAssertOrThrow(mRefundableFeeTracker);

        auto timeScope = mMetrics.getExecTimer();

        auto const& footprint = mResources.footprint;

        rust::Vec<CxxLedgerEntryRentChange> rustEntryRentChanges;
        rustEntryRentChanges.reserve(footprint.readOnly.size());
        // Extend for `extendTo` more ledgers since the current
        // ledger. Current ledger has to be payed for in order for entry
        // to be extendable, hence don't include it.
        uint32_t newLiveUntilLedgerSeq =
            getLedgerSeq() + mOpFrame.mExtendFootprintTTLOp.extendTo;
        auto ledgerVersion = getLedgerVersion();
        for (auto const& lk : footprint.readOnly)
        {
            auto ttlKey = getTTLKey(lk);

            auto ttlLeOpt = getLedgerEntryOpt(ttlKey);

            if (!ttlLeOpt || !isLive(*ttlLeOpt, getLedgerSeq()))
            {
                // Skip archived entries, as those must be restored.
                //
                // Also skip the missing entries. Since this happens at apply
                // time and we refund the unspent fees, it is more beneficial
                // to extend as many entries as possible.
                continue;
            }

            auto currLiveUntilLedgerSeq =
                ttlLeOpt->data.ttl().liveUntilLedgerSeq;
            if (currLiveUntilLedgerSeq >= newLiveUntilLedgerSeq)
            {
                continue;
            }

            auto entryOpt = getLedgerEntryOpt(lk);
            // We checked for TTLEntry existence above
            releaseAssertOrThrow(entryOpt);

            // Load the ContractCode/ContractData entry for fee calculation.

            auto const& entryLe = *entryOpt;

            uint32_t entrySize = static_cast<uint32_t>(xdr::xdr_size(entryLe));

            if (!validateContractLedgerEntry(lk, entrySize, mSorobanConfig,
                                             mAppConfig, mOpFrame.mParentTx,
                                             mDiagnosticEvents))
            {
                innerResult(mRes).code(
                    EXTEND_FOOTPRINT_TTL_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }

            if (!checkReadBytesResourceLimit(entrySize))
            {
                return false;
            }

            // We already checked that the TTLEntry exists in the logic above
            auto ttlLe = *ttlLeOpt;

            rustEntryRentChanges.emplace_back(
                createEntryRentChangeWithoutModification(
                    entryLe, entrySize,
                    /*entryLiveUntilLedger=*/
                    ttlLe.data.ttl().liveUntilLedgerSeq,
                    /*newLiveUntilLedger=*/newLiveUntilLedgerSeq, ledgerVersion,
                    mAppConfig, mSorobanConfig));

            ttlLe.data.ttl().liveUntilLedgerSeq = newLiveUntilLedgerSeq;

            upsertLedgerEntry(ttlKey, ttlLe);
        }

        // This may throw, but only in case of the Core version
        // misconfiguration.
        int64_t rentFee = rust_bridge::compute_rent_fee(
            mAppConfig.CURRENT_LEDGER_PROTOCOL_VERSION, ledgerVersion,
            rustEntryRentChanges,
            mSorobanConfig.rustBridgeRentFeeConfiguration(), getLedgerSeq());
        if (!mRefundableFeeTracker->consumeRefundableSorobanResources(
                0, rentFee, getLedgerVersion(), mSorobanConfig, mAppConfig,
                mOpFrame.mParentTx, mDiagnosticEvents))
        {
            innerResult(mRes).code(
                EXTEND_FOOTPRINT_TTL_INSUFFICIENT_REFUNDABLE_FEE);
            return false;
        }
        innerResult(mRes).code(EXTEND_FOOTPRINT_TTL_SUCCESS);
        return true;
    }
};

class ExtendFootprintTTLPreV23ApplyHelper
    : virtual public ExtendFootprintTTLApplyHelper,
      virtual public PreV23LedgerAccessHelper
{
  public:
    ExtendFootprintTTLPreV23ApplyHelper(
        AppConnector& app, AbstractLedgerTxn& ltx, OperationResult& res,
        std::optional<RefundableFeeTracker>& refundableFeeTracker,
        OperationMetaBuilder& opMeta, ExtendFootprintTTLOpFrame const& opFrame)
        : ExtendFootprintTTLApplyHelper(app, res, refundableFeeTracker, opMeta,
                                        opFrame)
        , PreV23LedgerAccessHelper(ltx)
    {
    }
    virtual bool
    checkReadBytesResourceLimit(uint32_t entrySize) override
    {
        mMetrics.mLedgerReadByte += entrySize;
        if (mResources.diskReadBytes < mMetrics.mLedgerReadByte)
        {
            mDiagnosticEvents.pushError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation byte-read resources exceeds amount specified",
                {makeU64SCVal(mMetrics.mLedgerReadByte),
                 makeU64SCVal(mResources.diskReadBytes)});

            innerResult(mRes).code(
                EXTEND_FOOTPRINT_TTL_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }
        return true;
    }
};

class ExtendFootprintTTLParallelApplyHelper
    : virtual public ExtendFootprintTTLApplyHelper,
      virtual public ParallelLedgerAccessHelper
{
  public:
    ExtendFootprintTTLParallelApplyHelper(
        AppConnector& app, ThreadParallelApplyLedgerState const& threadState,
        ParallelLedgerInfo const& ledgerInfo, OperationResult& res,
        std::optional<RefundableFeeTracker>& refundableFeeTracker,
        OperationMetaBuilder& opMeta, ExtendFootprintTTLOpFrame const& opFrame)
        : ExtendFootprintTTLApplyHelper(app, res, refundableFeeTracker, opMeta,
                                        opFrame)
        , ParallelLedgerAccessHelper(threadState, ledgerInfo,
                                     app.copySearchableLiveBucketListSnapshot())
    {
    }
    virtual bool
    checkReadBytesResourceLimit(uint32_t entrySize) override
    {
        return true;
    }

    OpModifiedEntryMap
    takeOpEntryMap()
    {
        return std::move(mOpState.takeSuccess().getModifiedEntryMap());
    }
};

ParallelTxReturnVal
ExtendFootprintTTLOpFrame::doParallelApply(
    AppConnector& app, ThreadParallelApplyLedgerState const& threadState,
    Config const& appConfig, SorobanNetworkConfig const& sorobanConfig,
    Hash const& _txPrngSeed, ParallelLedgerInfo const& ledgerInfo,
    SorobanMetrics& sorobanMetrics, OperationResult& res,
    std::optional<RefundableFeeTracker>& refundableFeeTracker,
    OperationMetaBuilder& opMeta) const
{
    ZoneNamedN(applyZone, "ExtendFootprintTTLOpFrame doParallelApply", true);
    releaseAssertOrThrow(
        protocolVersionStartsFrom(ledgerInfo.getLedgerVersion(),
                                  PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION));
    ExtendFootprintTTLParallelApplyHelper helper(
        app, threadState, ledgerInfo, res, refundableFeeTracker, opMeta, *this);
    if (helper.apply())
    {
        return {true, helper.takeOpEntryMap()};
    }
    else
    {
        return {false, {}};
    }
}

bool
ExtendFootprintTTLOpFrame::doApply(
    AppConnector& app, AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
    OperationResult& res,
    std::optional<RefundableFeeTracker>& refundableFeeTracker,
    OperationMetaBuilder& opMeta) const
{
    ZoneNamedN(applyZone, "ExtendFootprintTTLOpFrame apply", true);
    releaseAssertOrThrow(
        protocolVersionIsBefore(ltx.loadHeader().current().ledgerVersion,
                                PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION));
    ExtendFootprintTTLPreV23ApplyHelper helper(
        app, ltx, res, refundableFeeTracker, opMeta, *this);
    return helper.apply();
}

bool
ExtendFootprintTTLOpFrame::doCheckValidForSoroban(
    SorobanNetworkConfig const& networkConfig, Config const& appConfig,
    uint32_t ledgerVersion, OperationResult& res,
    DiagnosticEventManager& diagnosticEvents) const
{
    auto const& footprint = mParentTx.sorobanResources().footprint;
    if (!footprint.readWrite.empty())
    {
        innerResult(res).code(EXTEND_FOOTPRINT_TTL_MALFORMED);
        diagnosticEvents.pushError(
            SCE_STORAGE, SCEC_INVALID_INPUT,
            "read-write footprint must be empty for ExtendFootprintTTL "
            "operation",
            {});
        return false;
    }

    for (auto const& lk : footprint.readOnly)
    {
        if (!isSorobanEntry(lk))
        {
            innerResult(res).code(EXTEND_FOOTPRINT_TTL_MALFORMED);
            diagnosticEvents.pushError(
                SCE_STORAGE, SCEC_INVALID_INPUT,
                "only entries with TTL (contract data or code entries) can "
                "have it extended",
                {});
            return false;
        }
    }

    if (mExtendFootprintTTLOp.extendTo >
        networkConfig.stateArchivalSettings().maxEntryTTL - 1)
    {
        innerResult(res).code(EXTEND_FOOTPRINT_TTL_MALFORMED);
        diagnosticEvents.pushError(
            SCE_STORAGE, SCEC_INVALID_INPUT,
            "TTL extension is too large: {} > {}",
            {
                makeU64SCVal(mExtendFootprintTTLOp.extendTo),
                makeU64SCVal(networkConfig.stateArchivalSettings().maxEntryTTL -
                             1),
            });
        return false;
    }

    return true;
}

bool
ExtendFootprintTTLOpFrame::doCheckValid(uint32_t ledgerVersion,
                                        OperationResult& res) const
{
    throw std::runtime_error(
        "ExtendFootprintTTLOpFrame::doCheckValid needs Config");
}

void
ExtendFootprintTTLOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
}

bool
ExtendFootprintTTLOpFrame::isSoroban() const
{
    return true;
}

ThresholdLevel
ExtendFootprintTTLOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::LOW;
}

}
