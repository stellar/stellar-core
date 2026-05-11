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
    , mReconstructedSize(app.getMetrics().NewCounter(
          {"overlay", "compact", "reconstructed-size"}))
    , mReconstructedCount(app.getMetrics().NewCounter(
          {"overlay", "compact", "reconstructed-count"}))
    , mCompactSize(
          app.getMetrics().NewCounter({"overlay", "compact", "compact-size"}))
    , mCompactCount(
          app.getMetrics().NewCounter({"overlay", "compact", "compact-count"}))
    , mTxsRequested(
          app.getMetrics().NewCounter({"overlay", "compact", "txs-requested"}))
    , mTxBytesRequested(app.getMetrics().NewCounter(
          {"overlay", "compact", "tx-bytes-requested"}))
    , mTxBytesReceived(app.getMetrics().NewCounter(
          {"overlay", "compact", "tx-bytes-received"}))
    , mFetchTxSetTimer(app.getMetrics().NewTimer({"overlay", "fetch", "txset"}))
{
}
}
