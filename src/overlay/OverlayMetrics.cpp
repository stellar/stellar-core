#include "overlay/OverlayMetrics.h"
#include "main/Application.h"

#include "util/MetricsRegistry.h"

namespace stellar
{

OverlayMetrics::OverlayMetrics(Application& app)
    : mMessageRead(
          app.getMetrics().NewMeter({"overlay", "message", "read"}, "message"))
    , mMessageWrite(
          app.getMetrics().NewMeter({"overlay", "message", "write"}, "message"))
    , mMessageDrop(
          app.getMetrics().NewMeter({"overlay", "message", "drop"}, "message"))
    , mByteRead(app.getMetrics().NewMeter({"overlay", "byte", "read"}, "byte"))
    , mByteWrite(
          app.getMetrics().NewMeter({"overlay", "byte", "write"}, "byte"))
    , mErrorRead(
          app.getMetrics().NewMeter({"overlay", "error", "read"}, "error"))
    , mErrorWrite(
          app.getMetrics().NewMeter({"overlay", "error", "write"}, "error"))
    , mRecvTransactionTimer(app.getMetrics().NewSimpleTimer(
          {"overlay", "recv-transaction", ""}, std::chrono::microseconds{1}))
    , mRecvSCPMessageTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "scp-message"}))
    , mSendSCPMessageSetMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "scp-message"}, "message"))
    , mSendTransactionMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "transaction"}, "message"))
    , mSendTxSetMeter(
          app.getMetrics().NewMeter({"overlay", "send", "txset"}, "message"))
    , mSendFloodAdvertMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "flood-advert"}, "message"))
    , mMessagesDemanded(app.getMetrics().NewMeter(
          {"overlay", "flood", "demanded"}, "message"))
    , mMessagesFulfilledMeter(app.getMetrics().NewMeter(
          {"overlay", "flood", "fulfilled"}, "message"))
    , mUnknownMessageUnfulfilledMeter(app.getMetrics().NewMeter(
          {"overlay", "flood", "unfulfilled-unknown"}, "message"))
    , mTxPullLatency(
          app.getMetrics().NewTimer({"overlay", "flood", "tx-pull-latency"}))
    , mDemandTimeouts(app.getMetrics().NewMeter(
          {"overlay", "demand", "timeout"}, "timeout"))
    , mAbandonedDemandMeter(app.getMetrics().NewMeter(
          {"overlay", "flood", "abandoned-demands"}, "message"))
    , mMessagesBroadcast(app.getMetrics().NewMeter(
          {"overlay", "message", "broadcast"}, "message"))
    , mUniqueFloodBytesRecv(app.getMetrics().NewMeter(
          {"overlay", "flood", "unique-recv"}, "byte"))
    , mDuplicateFloodBytesRecv(app.getMetrics().NewMeter(
          {"overlay", "flood", "duplicate-recv"}, "byte"))
    , mTxBatchSizeHistogram(
          app.getMetrics().NewHistogram({"overlay", "flood", "tx-batch-size"}))
    , mPendingPeersSize(
          app.getMetrics().NewCounter({"overlay", "connection", "pending"}))
    , mAuthenticatedPeersSize(app.getMetrics().NewCounter(
          {"overlay", "connection", "authenticated"}))
    , mFetchTxSetTimer(app.getMetrics().NewTimer({"overlay", "fetch", "txset"}))
    , mTxSetShardBroadcast(app.getMetrics().NewMeter(
          {"overlay", "txset-shard", "broadcast"}, "message"))
    , mTxSetShardOriginalSent(app.getMetrics().NewMeter(
          {"overlay", "txset-shard", "original-sent"}, "shard"))
    , mTxSetShardRecoverySent(app.getMetrics().NewMeter(
          {"overlay", "txset-shard", "recovery-sent"}, "shard"))
    , mTxSetShardOriginalRecvUnique(app.getMetrics().NewMeter(
          {"overlay", "txset-shard", "original-recv-unique"}, "shard"))
    , mTxSetShardRecoveryRecvUnique(app.getMetrics().NewMeter(
          {"overlay", "txset-shard", "recovery-recv-unique"}, "shard"))
    , mTxSetShardOriginalRecvRedundant(app.getMetrics().NewMeter(
          {"overlay", "txset-shard", "original-recv-redundant"}, "shard"))
    , mTxSetShardRecoveryRecvRedundant(app.getMetrics().NewMeter(
          {"overlay", "txset-shard", "recovery-recv-redundant"}, "shard"))
    , mTxSetShardOriginalForwarded(app.getMetrics().NewMeter(
          {"overlay", "txset-shard", "original-forwarded"}, "shard"))
    , mTxSetShardRecoveryForwarded(app.getMetrics().NewMeter(
          {"overlay", "txset-shard", "recovery-forwarded"}, "shard"))
    , mTxSetShardReconstructSuccessOriginal(app.getMetrics().NewMeter(
          {"overlay", "txset-shard", "reconstruct-success-original"},
          "reconstruction"))
    , mTxSetShardReconstructSuccessRecovery(app.getMetrics().NewMeter(
          {"overlay", "txset-shard", "reconstruct-success-recovery"},
          "reconstruction"))
    , mTxSetShardReconstructFailOriginal(app.getMetrics().NewMeter(
          {"overlay", "txset-shard", "reconstruct-fail-original"},
          "reconstruction"))
    , mTxSetShardReconstructFailRecovery(app.getMetrics().NewMeter(
          {"overlay", "txset-shard", "reconstruct-fail-recovery"},
          "reconstruction"))
    , mTxSetShardReconstructRecoveryTimer(
          app.getMetrics().NewTimer({"overlay", "txset-shard",
                                     "reconstruct-recovery"}))
    , mTxSetShardFetchPreempted(app.getMetrics().NewMeter(
          {"overlay", "txset-shard", "fetch-preempted"}, "request"))
    , mTxSetShardEagerAlsoServed(app.getMetrics().NewMeter(
          {"overlay", "txset-shard", "eager-also-served"}, "request"))
{
}
}
