// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <iterator>
#include <type_traits>
#include <variant>
#include <xdrpp/xdrpp/marshal.h>

#include "crypto/SHA.h"
#include "ledger/LedgerTypeUtils.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/OperationFrame.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionMeta.h"
#include "util/GlobalChecks.h"
#include "util/MetaUtils.h"
#include "util/ProtocolVersion.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

namespace
{

template <typename T>
void
vecAppend(xdr::xvector<T>& a, xdr::xvector<T>&& b)
{
    std::move(b.begin(), b.end(), std::back_inserter(a));
}

// Starting in protocol 23, some operation meta needs to be modified
// to be consumed by downstream systems. In particular, restoration is
// (mostly) logically a new entry creation from the perspective of ltx and
// stellar-core as a whole, but this change type is reclassified to
// LEDGER_ENTRY_RESTORED for easier consumption downstream.
LedgerEntryChanges
processOpLedgerEntryChanges(Config const& cfg, OperationFrame const& op,
                            AbstractLedgerTxn& ltx, uint32_t protocolVersion)
{
    auto changes = ltx.getChanges();
    bool needToProcess =
        op.getOperation().body.type() == OperationType::RESTORE_FOOTPRINT &&
        (protocolVersionStartsFrom(protocolVersion,
                                   AUTO_RESTORE_PROTOCOL_VERSION) ||
         cfg.BACKFILL_RESTORE_META);
    if (!needToProcess)
    {
        return changes;
    }

    auto const& hotArchiveRestores = ltx.getRestoredHotArchiveKeys();
    auto const& liveRestores = ltx.getRestoredLiveBucketListKeys();

    // Depending on whether the restored entry is still in the live
    // BucketList (has not yet been evicted), or has been evicted and is in
    // the hot archive, meta will be handled differently as follows:
    //
    // Entry restore from Hot Archive:
    // Meta before changes:
    //     Data/Code: LEDGER_ENTRY_CREATED
    //     TTL: LEDGER_ENTRY_CREATED
    // Meta after changes:
    //     Data/Code: LEDGER_ENTRY_RESTORED
    //     TTL: LEDGER_ENTRY_RESTORED
    //
    // Entry restore from Live BucketList:
    // Meta before changes:
    //     Data/Code: no meta
    //     TTL: LEDGER_ENTRY_STATE(oldValue), LEDGER_ENTRY_UPDATED(newValue)
    // Meta after changes:
    //     Data/Code: LEDGER_ENTRY_RESTORED
    //     TTL: LEDGER_ENTRY_STATE(oldValue), LEDGER_ENTRY_RESTORED(newValue)
    //
    // First, iterate through existing meta and change everything we need to
    // update.
    for (auto& change : changes)
    {
        // For entry creation meta, we only need to check for Hot Archive
        // restores
        if (change.type() == LEDGER_ENTRY_CREATED)
        {
            auto le = change.created();
            if (hotArchiveRestores.find(LedgerEntryKey(le)) !=
                hotArchiveRestores.end())
            {
                releaseAssertOrThrow(isPersistentEntry(le.data) ||
                                     le.data.type() == TTL);
                change.type(LEDGER_ENTRY_RESTORED);
                change.restored() = le;
            }
        }
        // Update meta only applies to TTL meta
        else if (change.type() == LEDGER_ENTRY_UPDATED)
        {
            if (change.updated().data.type() == TTL)
            {
                auto ttlLe = change.updated();
                if (liveRestores.find(LedgerEntryKey(ttlLe)) !=
                    liveRestores.end())
                {
                    // Update the TTL change from LEDGER_ENTRY_UPDATED to
                    // LEDGER_ENTRY_RESTORED.
                    change.type(LEDGER_ENTRY_RESTORED);
                    change.restored() = ttlLe;
                }
            }
        }
    }

    // Now we need to insert all the LEDGER_ENTRY_RESTORED changes for the
    // data entries that were not created but already existed on the live
    // BucketList. These data/code entries have not been modified (only the TTL
    // is updated), so ltx doesn't have any meta. However this is still useful
    // for downstream so we manually insert restore meta here.
    for (auto const& key : liveRestores)
    {
        if (key.type() == TTL)
        {
            continue;
        }
        releaseAssertOrThrow(isPersistentEntry(key));

        // Note: this is already in the cache since the RestoreOp loaded
        // all data keys for size calculation during apply already
        auto entry = ltx.getNewestVersion(key);

        // If TTL already exists and is just being updated, the
        // data entry must also already exist
        releaseAssertOrThrow(entry);

        LedgerEntryChange change;
        change.type(LEDGER_ENTRY_RESTORED);
        change.restored() = entry->ledgerEntry();
        changes.push_back(change);
    }

    return changes;
}

} // namespace

void
OperationMetaBuilder::setLedgerChanges(AbstractLedgerTxn& opLtx)
{
    if (!mEnabled)
    {
        return;
    }
    std::visit(
        [&opLtx, this](auto&& meta) {
            meta.get().changes = processOpLedgerEntryChanges(
                mConfig, mOp, opLtx, mProtocolVersion);
        },
        mMeta);
}

void
OperationMetaBuilder::setSorobanReturnValue(SCVal const& val)
{
    if (!mEnabled)
    {
        return;
    }
    releaseAssertOrThrow(!mSorobanReturnValue);
    mSorobanReturnValue.emplace(val);
}

OpEventManager&
OperationMetaBuilder::getEventManager()
{
    return mEventManager;
}

DiagnosticEventManager&
OperationMetaBuilder::getDiagnosticEventManager()
{
    return mDiagnosticEventManager;
}

OperationMetaBuilder::OperationMetaBuilder(
    Config const& cfg, bool metaEnabled, OperationMeta& meta,
    OperationFrame const& op, uint32_t protocolVersion, Hash const& networkID,
    Config const& config, DiagnosticEventManager& diagnosticEventManager)
    : mEnabled(metaEnabled)
    , mProtocolVersion(protocolVersion)
    , mOp(op)
    , mMeta(meta)
    , mEventManager(metaEnabled, op.isSoroban(), protocolVersion, networkID,
                    op.getTxMemo(), config)
    , mDiagnosticEventManager(diagnosticEventManager)
    , mConfig(cfg)
{
}

OperationMetaBuilder::OperationMetaBuilder(
    Config const& cfg, bool metaEnabled, OperationMetaV2& meta,
    OperationFrame const& op, uint32_t protocolVersion, Hash const& networkID,
    Config const& config, DiagnosticEventManager& diagnosticEventManager)
    : mEnabled(metaEnabled)
    , mProtocolVersion(protocolVersion)
    , mOp(op)
    , mMeta(meta)
    , mEventManager(metaEnabled, op.isSoroban(), protocolVersion, networkID,
                    op.getTxMemo(), config)
    , mDiagnosticEventManager(diagnosticEventManager)
    , mConfig(cfg)
{
}

bool
OperationMetaBuilder::maybeFinalizeOpEvents()
{
    return std::visit(
        [this](auto&& meta) {
            using T = std::decay_t<decltype(meta)>;
            if constexpr (std::is_same_v<
                              T, std::reference_wrapper<OperationMetaV2>>)
            {
                meta.get().events = mEventManager.finalize();
                return true;
            }
            return false;
        },
        mMeta);
}

TransactionMetaBuilder::TransactionMetaWrapper::TransactionMetaWrapper(
    uint32_t protocolVersion, Config const& config)
{
    int version = 0;
    // The TransactionMeta v() switch can be in 5 positions 0, 1, 2, 3, 4. We do
    // not support 0 or 1 at all -- core does not produce it anymore and we have
    // no obligation to consume it under any circumstance -- so this class just
    // switches between cases 2, 3 and 4.
    if (protocolVersionStartsFrom(protocolVersion, ProtocolVersion::V_23) ||
        config.BACKFILL_STELLAR_ASSET_EVENTS)
    {
        version = 4;
    }
    else if (protocolVersionStartsFrom(protocolVersion,
                                       SOROBAN_PROTOCOL_VERSION))
    {
        version = 3;
    }
    else
    {
        version = 2;
    }
    mTransactionMeta.v(version);
}

LedgerEntryChanges&
TransactionMetaBuilder::TransactionMetaWrapper::getChangesBefore()
{
    switch (mTransactionMeta.v())
    {
    case 2:
        return mTransactionMeta.v2().txChangesBefore;
    case 3:
        return mTransactionMeta.v3().txChangesBefore;
    case 4:
        return mTransactionMeta.v4().txChangesBefore;
        break;
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaBuilder::TransactionMetaWrapper::setOperationMetas(
    xdr::xvector<OperationMeta>&& opMetas)
{
    switch (mTransactionMeta.v())
    {
    case 2:
        mTransactionMeta.v2().operations = std::move(opMetas);
        break;
    case 3:
        mTransactionMeta.v3().operations = std::move(opMetas);
        break;
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaBuilder::TransactionMetaWrapper::setOperationMetas(
    xdr::xvector<OperationMetaV2>&& opMetas)
{
    switch (mTransactionMeta.v())
    {
    case 4:
        mTransactionMeta.v4().operations = std::move(opMetas);
        break;
    default:
        releaseAssert(false);
    }
}

LedgerEntryChanges&
TransactionMetaBuilder::TransactionMetaWrapper::getChangesAfter()
{
    switch (mTransactionMeta.v())
    {
    case 2:
        return mTransactionMeta.v2().txChangesAfter;
    case 3:
        return mTransactionMeta.v3().txChangesAfter;
    case 4:
        return mTransactionMeta.v4().txChangesAfter;
    default:
        releaseAssert(false);
    }
}

SorobanTransactionMetaExt&
TransactionMetaBuilder::TransactionMetaWrapper::getSorobanMetaExt()
{
    switch (mTransactionMeta.v())
    {
    case 3:
        return mTransactionMeta.v3().sorobanMeta.activate().ext;
    case 4:
        return mTransactionMeta.v4().sorobanMeta.activate().ext;
    // Calling this before Soroban meta ext is available is a bug.
    case 2:
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaBuilder::TransactionMetaWrapper::setDiagnosticEvents(
    xdr::xvector<DiagnosticEvent>&& events)
{
    switch (mTransactionMeta.v())
    {
    case 3:
        mTransactionMeta.v3().sorobanMeta.activate().diagnosticEvents =
            std::move(events);
        break;
    case 4:
        mTransactionMeta.v4().diagnosticEvents = std::move(events);
        break;
    // It's a bug to call this when diagnostic events are not supported.
    case 2:
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaBuilder::TransactionMetaWrapper::maybeSetContractEventsAtTxLevel(
    xdr::xvector<ContractEvent>&& events)
{
    switch (mTransactionMeta.v())
    {
    case 2:
        // Do nothing, until v3 we don't create events.
        break;
    case 3:
        mTransactionMeta.v3().sorobanMeta.activate().events = std::move(events);
        break;
    case 4:
        // Do nothing, v4 contract events live in the operation meta
        break;
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaBuilder::TransactionMetaWrapper::maybeActivateSorobanMeta(
    bool success)
{
    switch (mTransactionMeta.v())
    {
    case 3:
        // In v3 meta we used to always have Soroban meta activated,
        // even in case of failure.
        mTransactionMeta.v3().sorobanMeta.activate();
        break;
    case 4:
        // From v4 we omit the meta for failed txs (it only contains
        // info related to success).
        if (success)
        {
            mTransactionMeta.v4().sorobanMeta.activate();
        }
        break;
    // It's a bug to call this when Soroban meta is not supported.
    case 2:
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaBuilder::TransactionMetaWrapper::setReturnValue(
    SCVal const& returnValue)
{
    switch (mTransactionMeta.v())
    {
    case 3:
        mTransactionMeta.v3().sorobanMeta.activate().returnValue = returnValue;
        break;
    case 4:
        mTransactionMeta.v4().sorobanMeta.activate().returnValue.activate() =
            returnValue;
        break;
    // Setting Soroban return value prior to meta v3 is a bug.
    case 2:
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaBuilder::TransactionMetaWrapper::setTransactionEvents(
    xdr::xvector<TransactionEvent>&& events)
{
    switch (mTransactionMeta.v())
    {
    case 4:
        mTransactionMeta.v4().events = std::move(events);
        break;
    // Setting transaction events before meta v4 is a bug.
    case 2:
    case 3:
    default:
        releaseAssert(false);
    }
}

#ifdef BUILD_TESTS
TransactionMetaFrame::TransactionMetaFrame(TransactionMeta const& meta)
    : mTransactionMeta(meta)
{
}
size_t
TransactionMetaFrame::getNumOperations() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        return mTransactionMeta.v2().operations.size();
    case 3:
        return mTransactionMeta.v3().operations.size();
    case 4:
        return mTransactionMeta.v4().operations.size();
    default:
        releaseAssert(false);
    }
}

size_t
TransactionMetaFrame::getNumChangesBefore() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        return mTransactionMeta.v2().txChangesBefore.size();
    case 3:
        return mTransactionMeta.v3().txChangesBefore.size();
    case 4:
        return mTransactionMeta.v4().txChangesBefore.size();
    default:
        releaseAssert(false);
    }
}

LedgerEntryChanges
TransactionMetaFrame::getChangesBefore() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        return mTransactionMeta.v2().txChangesBefore;
    case 3:
        return mTransactionMeta.v3().txChangesBefore;
    case 4:
        return mTransactionMeta.v4().txChangesBefore;
    default:
        releaseAssert(false);
    }
}

LedgerEntryChanges
TransactionMetaFrame::getChangesAfter() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        return mTransactionMeta.v2().txChangesAfter;
    case 3:
        return mTransactionMeta.v3().txChangesAfter;
    case 4:
        return mTransactionMeta.v4().txChangesAfter;
    default:
        releaseAssert(false);
    }
}

SCVal const&
TransactionMetaFrame::getReturnValue() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        throw std::runtime_error("Return value not available for v2 meta");
    case 3:
        return mTransactionMeta.v3().sorobanMeta->returnValue;
    case 4:
        releaseAssert(mTransactionMeta.v4().sorobanMeta->returnValue);
        return *mTransactionMeta.v4().sorobanMeta->returnValue;
    default:
        releaseAssert(false);
    }
}

xdr::xvector<DiagnosticEvent> const&
TransactionMetaFrame::getDiagnosticEvents() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        throw std::runtime_error("Diagnostic events not available for v2 meta");
    case 3:
        return mTransactionMeta.v3().sorobanMeta->diagnosticEvents;
    case 4:
        return mTransactionMeta.v4().diagnosticEvents;
    default:
        releaseAssert(false);
    }
}

xdr::xvector<ContractEvent> const&
TransactionMetaFrame::getOpEventsAtOp(size_t opIdx) const
{
    switch (mTransactionMeta.v())
    {
    case 2:
    case 3:
        throw std::runtime_error(
            "Operation events not available for v2/v3 meta");
    case 4:
        return mTransactionMeta.v4().operations.at(opIdx).events;
    default:
        releaseAssert(false);
    }
}

xdr::xvector<TransactionEvent> const&
TransactionMetaFrame::getTxEvents() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
    case 3:
        throw std::runtime_error(
            "Transaction-level contract events not available for v2/v3 meta");
    case 4:
        return mTransactionMeta.v4().events;
    default:
        releaseAssert(false);
    }
}

xdr::xvector<ContractEvent>
TransactionMetaFrame::getSorobanContractEvents() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        throw std::runtime_error("Contract events not available for v2 meta");
    case 3:
        return mTransactionMeta.v3().sorobanMeta->events;
    case 4:
        if (mTransactionMeta.v4().operations.empty())
        {
            return xdr::xvector<ContractEvent>{};
        }
        else if (mTransactionMeta.v4().operations.size() == 1)
        {
            return mTransactionMeta.v4().operations.at(0).events;
        }
        else
        {
            throw std::runtime_error("Operation meta size can only be 0 or 1 "
                                     "in a Soroban transaction");
        }
    default:
        releaseAssert(false);
    }
}

LedgerEntryChanges const&
TransactionMetaFrame::getLedgerEntryChangesAtOp(size_t opIdx) const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        return mTransactionMeta.v2().operations.at(opIdx).changes;
    case 3:
        return mTransactionMeta.v3().operations.at(opIdx).changes;
    case 4:
        return mTransactionMeta.v4().operations.at(opIdx).changes;
    default:
        releaseAssert(false);
    }
}

TransactionMeta const&
TransactionMetaFrame::getXDR() const
{
    return mTransactionMeta;
}

#endif

TransactionMetaBuilder::TransactionMetaBuilder(bool metaEnabled,
                                               TransactionFrameBase const& tx,
                                               uint32_t protocolVersion,
                                               AppConnector const& app)
    : mTransactionMeta(protocolVersion, app.getConfig())
    , mTxEventManager(metaEnabled, protocolVersion, app.getNetworkID(),
                      app.getConfig())
    , mDiagnosticEventManager(DiagnosticEventManager::createForApply(
          metaEnabled, tx, app.getConfig()))
    , mIsSoroban(tx.isSoroban())
    , mEnabled(metaEnabled)
    , mSorobanMetaExtEnabled(
          app.getConfig().EMIT_SOROBAN_TRANSACTION_META_EXT_V1)
{
    auto const& operationFrames = tx.getOperationFrames();
    size_t numOperations = operationFrames.size();
    mOperationMetaBuilders.reserve(numOperations);

    switch (mTransactionMeta.mTransactionMeta.v())
    {
    case 2:
    case 3:
    {
        auto& opMeta = mOperationMetas.emplace<xdr::xvector<OperationMeta>>();
        opMeta.resize(numOperations);
        for (size_t i = 0; i < numOperations; ++i)
        {
            mOperationMetaBuilders.emplace_back(OperationMetaBuilder(
                app.getConfig(), metaEnabled, opMeta[i], *operationFrames[i],
                protocolVersion, app.getNetworkID(), app.getConfig(),
                mDiagnosticEventManager));
        }
        break;
    }
    case 4:
    {
        auto& opMeta = mOperationMetas.emplace<xdr::xvector<OperationMetaV2>>();
        opMeta.resize(numOperations);
        for (size_t i = 0; i < numOperations; ++i)
        {
            mOperationMetaBuilders.emplace_back(OperationMetaBuilder(
                app.getConfig(), metaEnabled, opMeta[i], *operationFrames[i],
                protocolVersion, app.getNetworkID(), app.getConfig(),
                mDiagnosticEventManager));
        }
        break;
    }
    default:
        releaseAssert(false);
    }
}

TxEventManager&
TransactionMetaBuilder::getTxEventManager()
{
    return mTxEventManager;
}

OperationMetaBuilder&
TransactionMetaBuilder::getOperationMetaBuilderAt(size_t i)
{
    return mOperationMetaBuilders.at(i);
}

void
TransactionMetaBuilder::pushTxChangesBefore(AbstractLedgerTxn& changesBeforeLtx)
{
    maybePushChanges(changesBeforeLtx, mTransactionMeta.getChangesBefore());
}

void
TransactionMetaBuilder::pushTxChangesAfter(AbstractLedgerTxn& changesAfterLtx)
{
    maybePushChanges(changesAfterLtx, mTransactionMeta.getChangesAfter());
}

void
TransactionMetaBuilder::setNonRefundableResourceFee(int64_t fee)
{
    if (mEnabled && mSorobanMetaExtEnabled)
    {
        releaseAssert(!mFinalized);
        auto& metaExt = mTransactionMeta.getSorobanMetaExt();
        metaExt.v(1);
        auto& sorobanMeta = metaExt.v1();
        sorobanMeta.totalNonRefundableResourceFeeCharged = fee;
    }
}

void
TransactionMetaBuilder::maybeSetRefundableFeeMeta(
    std::optional<RefundableFeeTracker> const& refundableFeeTracker)
{
    if (mEnabled && refundableFeeTracker && mSorobanMetaExtEnabled)
    {
        releaseAssert(!mFinalized);
        auto& metaExt = mTransactionMeta.getSorobanMetaExt();
        metaExt.v(1);
        auto& sorobanMeta = metaExt.v1();
        sorobanMeta.rentFeeCharged = refundableFeeTracker->getConsumedRentFee();
        sorobanMeta.totalRefundableResourceFeeCharged =
            refundableFeeTracker->getConsumedRefundableFee();
    }
}

DiagnosticEventManager&
TransactionMetaBuilder::getDiagnosticEventManager()
{
    return mDiagnosticEventManager;
}

TransactionMeta
TransactionMetaBuilder::finalize(bool success)
{
    // Finalizing the meta only makes sense when it's enabled in the first
    // place.
    releaseAssert(mEnabled);
    releaseAssert(!mFinalized);
    mFinalized = true;

    if (mIsSoroban)
    {
        // Maybe activate Soroban meta, depending on the meta version
        // and transaction success.
        mTransactionMeta.maybeActivateSorobanMeta(success);
    }

    // Operation meta is only populated when transaction succeeds.
    if (success)
    {
        // Some of the Soroban meta is sometimes set at the transaction level,
        // even though semantically it belongs to the operation. Here we
        // reconcile this information back into transaction meta.
        if (mIsSoroban)
        {
            // Currently there can only be 1 operation per Soroban transaction.
            releaseAssert(mOperationMetaBuilders.size() == 1);
            auto& opMetaBuilder = mOperationMetaBuilders[0];

            if (opMetaBuilder.mSorobanReturnValue)
            {
                mTransactionMeta.setReturnValue(
                    *opMetaBuilder.mSorobanReturnValue);
            }
            // Events have to be finalized either at operation level (to put
            // them into the op meta), or at transaction level for v1
            // OperationMeta.
            if (!opMetaBuilder.maybeFinalizeOpEvents())
            {
                mTransactionMeta.maybeSetContractEventsAtTxLevel(
                    opMetaBuilder.getEventManager().finalize());
            }
        }
        else
        {
            // For classic transactions we only need to finalize the events
            // at operation level (if events are enabled at all).
            for (auto& opMetaBuilder : mOperationMetaBuilders)
            {
                opMetaBuilder.maybeFinalizeOpEvents();
            }
        }

        std::visit(
            [this](auto&& metas) {
                mTransactionMeta.setOperationMetas(std::move(metas));
            },
            mOperationMetas);
    }
    // Write transaction-level events if they are enabled. Note, that this
    // doesn't depend on success of the transaction, as fees are charged
    // even for failed transactions.
    if (mTxEventManager.isEnabled())
    {
        mTransactionMeta.setTransactionEvents(mTxEventManager.finalize());
    }
    // Write diagnostic events if they're enabled. Note, that this doesn't
    // depend on the success of the transaction, as diagnostic events should
    // also be populated for the failed transactions.
    if (mDiagnosticEventManager.isEnabled())
    {
        mTransactionMeta.setDiagnosticEvents(
            mDiagnosticEventManager.finalize());
    }
    return std::move(mTransactionMeta.mTransactionMeta);
}

void
TransactionMetaBuilder::maybePushChanges(AbstractLedgerTxn& changesLtx,
                                         LedgerEntryChanges& destChanges)
{
    if (mEnabled)
    {
        releaseAssert(!mFinalized);
        vecAppend(destChanges, changesLtx.getChanges());
    }
}
}
