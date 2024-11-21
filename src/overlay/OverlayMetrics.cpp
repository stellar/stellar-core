#include "overlay/OverlayMetrics.h"
#include "main/Application.h"

#include "medida/metrics_registry.h"

namespace stellar
{

OverlayMetrics::OverlayMetrics(Application& app)
    : mMessageRead(
          app.getMetrics().NewMeter({"overlay", "message", "read"}, "message"))
    , mMessageWrite(
          app.getMetrics().NewMeter({"overlay", "message", "write"}, "message"))
    , mMessageDrop(
          app.getMetrics().NewMeter({"overlay", "message", "drop"}, "message"))
    , mAsyncRead(
          app.getMetrics().NewMeter({"overlay", "async", "read"}, "call"))
    , mAsyncWrite(
          app.getMetrics().NewMeter({"overlay", "async", "write"}, "call"))
    , mByteRead(app.getMetrics().NewMeter({"overlay", "byte", "read"}, "byte"))
    , mByteWrite(
          app.getMetrics().NewMeter({"overlay", "byte", "write"}, "byte"))
    , mErrorRead(
          app.getMetrics().NewMeter({"overlay", "error", "read"}, "error"))
    , mErrorWrite(
          app.getMetrics().NewMeter({"overlay", "error", "write"}, "error"))
    , mTimeoutIdle(
          app.getMetrics().NewMeter({"overlay", "timeout", "idle"}, "timeout"))
    , mTimeoutStraggler(app.getMetrics().NewMeter(
          {"overlay", "timeout", "straggler"}, "timeout"))
    , mConnectionLatencyTimer(
          app.getMetrics().NewTimer({"overlay", "connection", "latency"}))
    , mConnectionReadThrottle(
          app.getMetrics().NewTimer({"overlay", "connection", "read-throttle"}))
    , mConnectionFloodThrottle(app.getMetrics().NewTimer(
          {"overlay", "connection", "flood-throttle"}))

    , mItemFetcherNextPeer(app.getMetrics().NewMeter(
          {"overlay", "item-fetcher", "next-peer"}, "item-fetcher"))

    , mRecvErrorTimer(app.getMetrics().NewTimer({"overlay", "recv", "error"}))
    , mRecvHelloTimer(app.getMetrics().NewTimer({"overlay", "recv", "hello"}))
    , mRecvAuthTimer(app.getMetrics().NewTimer({"overlay", "recv", "auth"}))
    , mRecvDontHaveTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "dont-have"}))
    , mRecvPeersTimer(app.getMetrics().NewTimer({"overlay", "recv", "peers"}))
    , mRecvGetTxSetTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "get-txset"}))
    , mRecvTxSetTimer(app.getMetrics().NewTimer({"overlay", "recv", "txset"}))
    , mRecvTransactionTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "transaction"}))
    , mRecvGetSCPQuorumSetTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "get-scp-qset"}))
    , mRecvSCPQuorumSetTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "scp-qset"}))
    , mRecvSCPMessageTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "scp-message"}))
    , mRecvGetSCPStateTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "get-scp-state"}))
    , mRecvSendMoreTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "send-more"}))

    , mRecvSCPPrepareTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "scp-prepare"}))
    , mRecvSCPConfirmTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "scp-confirm"}))
    , mRecvSCPNominateTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "scp-nominate"}))
    , mRecvSCPExternalizeTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "scp-externalize"}))

    , mRecvSurveyRequestTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "survey-request"}))
    , mRecvSurveyResponseTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "survey-response"}))
    , mRecvStartSurveyCollectingTimer(app.getMetrics().NewTimer(
          {"overlay", "recv", "start-survey-collecting"}))
    , mRecvStopSurveyCollectingTimer(app.getMetrics().NewTimer(
          {"overlay", "recv", "stop-survey-collecting"}))

    , mRecvFloodAdvertTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "flood-advert"}))
    , mRecvFloodDemandTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "flood-demand"}))

    , mMessageDelayInWriteQueueTimer(
          app.getMetrics().NewTimer({"overlay", "delay", "write-queue"}))
    , mMessageDelayInAsyncWriteTimer(
          app.getMetrics().NewTimer({"overlay", "delay", "async-write"}))
    , mOutboundQueueDelaySCP(
          app.getMetrics().NewTimer({"overlay", "outbound-queue", "scp"}))
    , mOutboundQueueDelayTxs(
          app.getMetrics().NewTimer({"overlay", "outbound-queue", "tx"}))
    , mOutboundQueueDelayAdvert(
          app.getMetrics().NewTimer({"overlay", "outbound-queue", "advert"}))
    , mOutboundQueueDelayDemand(
          app.getMetrics().NewTimer({"overlay", "outbound-queue", "demand"}))

    , mOutboundQueueDropSCP(app.getMetrics().NewMeter(
          {"overlay", "outbound-queue", "drop-scp"}, "message"))
    , mOutboundQueueDropTxs(app.getMetrics().NewMeter(
          {"overlay", "outbound-queue", "drop-tx"}, "message"))
    , mOutboundQueueDropAdvert(app.getMetrics().NewMeter(
          {"overlay", "outbound-queue", "drop-advert"}, "message"))
    , mOutboundQueueDropDemand(app.getMetrics().NewMeter(
          {"overlay", "outbound-queue", "drop-demand"}, "message"))
    , mSendErrorMeter(
          app.getMetrics().NewMeter({"overlay", "send", "error"}, "message"))
    , mSendHelloMeter(
          app.getMetrics().NewMeter({"overlay", "send", "hello"}, "message"))
    , mSendAuthMeter(
          app.getMetrics().NewMeter({"overlay", "send", "auth"}, "message"))
    , mSendDontHaveMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "dont-have"}, "message"))
    , mSendPeersMeter(
          app.getMetrics().NewMeter({"overlay", "send", "peers"}, "message"))
    , mSendGetTxSetMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "get-txset"}, "message"))
    , mSendTransactionMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "transaction"}, "message"))
    , mSendTxSetMeter(
          app.getMetrics().NewMeter({"overlay", "send", "txset"}, "message"))
    , mSendGetSCPQuorumSetMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "get-scp-qset"}, "message"))
    , mSendSCPQuorumSetMeter(
          app.getMetrics().NewMeter({"overlay", "send", "scp-qset"}, "message"))
    , mSendSCPMessageSetMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "scp-message"}, "message"))
    , mSendGetSCPStateMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "get-scp-state"}, "message"))
    , mSendSendMoreMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "send-more"}, "message"))
    , mSendSurveyRequestMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "survey-request"}, "message"))
    , mSendSurveyResponseMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "survey-response"}, "message"))
    , mSendStartSurveyCollectingMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "start-survey-collecting"}, "message"))
    , mSendStopSurveyCollectingMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "stop-survey-collecting"}, "message"))
    , mSendFloodAdvertMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "flood-advert"}, "message"))
    , mSendFloodDemandMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "flood-demand"}, "message"))
    , mMessagesDemanded(app.getMetrics().NewMeter(
          {"overlay", "flood", "demanded"}, "message"))
    , mMessagesFulfilledMeter(app.getMetrics().NewMeter(
          {"overlay", "flood", "fulfilled"}, "message"))
    , mBannedMessageUnfulfilledMeter(app.getMetrics().NewMeter(
          {"overlay", "flood", "unfulfilled-banned"}, "message"))
    , mUnknownMessageUnfulfilledMeter(app.getMetrics().NewMeter(
          {"overlay", "flood", "unfulfilled-unknown"}, "message"))
    , mTxPullLatency(
          app.getMetrics().NewTimer({"overlay", "flood", "tx-pull-latency"}))
    , mPeerTxPullLatency(app.getMetrics().NewTimer(
          {"overlay", "flood", "peer-tx-pull-latency"}))
    , mAdvertQueueDelay(
          app.getMetrics().NewTimer({"overlay", "flood", "advert-delay"}))
    , mDemandTimeouts(app.getMetrics().NewMeter(
          {"overlay", "demand", "timeout"}, "timeout"))
    , mPulledRelevantTxs(app.getMetrics().NewMeter(
          {"overlay", "flood", "relevant-txs"}, "transaction"))
    , mPulledIrrelevantTxs(app.getMetrics().NewMeter(
          {"overlay", "flood", "irrelevant-txs"}, "transaction"))
    , mAbandonedDemandMeter(app.getMetrics().NewMeter(
          {"overlay", "flood", "abandoned-demands"}, "message"))
    , mMessagesBroadcast(app.getMetrics().NewMeter(
          {"overlay", "message", "broadcast"}, "message"))
    , mPendingPeersSize(
          app.getMetrics().NewCounter({"overlay", "connection", "pending"}))
    , mAuthenticatedPeersSize(app.getMetrics().NewCounter(
          {"overlay", "connection", "authenticated"}))
    , mUniqueFloodBytesRecv(app.getMetrics().NewMeter(
          {"overlay", "flood", "unique-recv"}, "byte"))
    , mDuplicateFloodBytesRecv(app.getMetrics().NewMeter(
          {"overlay", "flood", "duplicate-recv"}, "byte"))
    , mUniqueFetchBytesRecv(app.getMetrics().NewMeter(
          {"overlay", "fetch", "unique-recv"}, "byte"))
    , mDuplicateFetchBytesRecv(app.getMetrics().NewMeter(
          {"overlay", "fetch", "duplicate-recv"}, "byte"))
{
}
}
