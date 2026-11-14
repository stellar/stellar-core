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
    , mRecvTransactionTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "transaction"}))
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
    , mTxSetPushSent(
          app.getMetrics().NewMeter({"overlay", "txset-push", "sent"}, "txset"))
    , mTxSetPushReceived(app.getMetrics().NewMeter(
          {"overlay", "txset-push", "received"}, "txset"))
    , mTxSetPushHit(
          app.getMetrics().NewMeter({"overlay", "txset-push", "hit"}, "txset"))
    , mTxSetPushRequestSkipped(app.getMetrics().NewMeter(
          {"overlay", "txset-push", "request-skipped"}, "txset"))
    , mTxSetPushDropped(app.getMetrics().NewMeter(
          {"overlay", "txset-push", "dropped"}, "txset"))
    , mTxSetPushEvicted(app.getMetrics().NewMeter(
          {"overlay", "txset-push", "evicted"}, "txset"))
    , mTxsetPushSentBytes(app.getMetrics().NewMeter(
          {"overlay", "txset-push", "sent-bytes"}, "byte"))
    , mTxsetTreeRootSent(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "root-sent"}, "txset"))
    , mTxsetTreeRelayed(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "relayed"}, "txset"))
    , mTxsetTreeRelayedBytes(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "relayed-bytes"}, "byte"))
    , mTxsetTreeReceived(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "received"}, "txset"))
    , mTxsetTreeDuplicate(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "duplicate"}, "txset"))
    , mTxsetTreeRefused(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "refused"}, "txset"))
    , mTxsetTreeUnreachable(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "unreachable"}, "peer"))
    , mTxsetTreeChildBusy(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "child-busy"}, "peer"))
    , mTxsetTreeRelayBusy(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "relay-busy"}, "peer"))
    , mTxsetTreeRelayLimited(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "relay-limited"}, "txset"))
    , mTxsetTreeFetchDeferred(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "fetch-deferred"}, "txset"))
    , mTxsetTreeFetchFallback(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "fetch-fallback"}, "txset"))
    , mTxsetTreeDelayMsSum(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "delay-ms-sum"}, "ms"))
    , mTxsetTreeDelayCount(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "delay-count"}, "txset"))
    , mTxsetTreeDelayHop1MsSum(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "delay-hop1-ms-sum"}, "ms"))
    , mTxsetTreeDelayHop1Count(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "delay-hop1-count"}, "txset"))
    , mTxsetTreeDelayHop2MsSum(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "delay-hop2-ms-sum"}, "ms"))
    , mTxsetTreeDelayHop2Count(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "delay-hop2-count"}, "txset"))
    , mTxsetTreeDelayLe50(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "delay-le-50ms"}, "txset"))
    , mTxsetTreeDelayLe100(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "delay-le-100ms"}, "txset"))
    , mTxsetTreeDelayLe150(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "delay-le-150ms"}, "txset"))
    , mTxsetTreeDelayLe200(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "delay-le-200ms"}, "txset"))
    , mTxsetTreeDelayLe300(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "delay-le-300ms"}, "txset"))
    , mTxsetTreeDelayGt300(app.getMetrics().NewMeter(
          {"overlay", "txset-tree", "delay-gt-300ms"}, "txset"))
    , mTxDirectSent(app.getMetrics().NewMeter(
          {"overlay", "flood", "tx-direct-sent"}, "transaction"))
    , mTxDirectSentBytes(app.getMetrics().NewMeter(
          {"overlay", "flood", "tx-direct-sent-bytes"}, "byte"))
    , mTxDirectRecv(app.getMetrics().NewMeter(
          {"overlay", "flood", "tx-direct-recv"}, "transaction"))
    , mTxLateCopy(app.getMetrics().NewMeter(
          {"overlay", "flood", "tx-late-copy"}, "transaction"))
    , mTxLocalFlooded(app.getMetrics().NewMeter(
          {"overlay", "flood", "tx-local-flooded"}, "transaction"))
    , mNodeIdRejected(app.getMetrics().NewMeter(
          {"overlay", "node-id", "rejected"}, "claim"))
    , mNodeIdIdentified(
          app.getMetrics().NewCounter({"overlay", "node-id", "identified"}))
{
}
}
