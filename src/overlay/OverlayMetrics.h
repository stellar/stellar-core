#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// This structure just exists to cache frequently-accessed, overlay-wide
// (non-peer-specific) metrics.

namespace medida
{
class Timer;
class Meter;
class Counter;
}

namespace stellar
{

class Application;

// OverlayMetrics is a thread-safe struct
struct OverlayMetrics
{
    OverlayMetrics(Application& app);
    medida::Meter& mMessageRead;
    medida::Meter& mMessageWrite;
    medida::Meter& mMessageDrop;
    medida::Meter& mAsyncRead;
    medida::Meter& mAsyncWrite;
    medida::Meter& mByteRead;
    medida::Meter& mByteWrite;
    medida::Meter& mErrorRead;
    medida::Meter& mErrorWrite;
    medida::Meter& mTimeoutIdle;
    medida::Meter& mTimeoutStraggler;
    medida::Timer& mConnectionLatencyTimer;
    medida::Timer& mConnectionReadThrottle;
    medida::Timer& mConnectionFloodThrottle;

    medida::Meter& mItemFetcherNextPeer;

    medida::Timer& mRecvErrorTimer;
    medida::Timer& mRecvHelloTimer;
    medida::Timer& mRecvAuthTimer;
    medida::Timer& mRecvDontHaveTimer;
    medida::Timer& mRecvPeersTimer;
    medida::Timer& mRecvGetTxSetTimer;
    medida::Timer& mRecvTxSetTimer;
    medida::Timer& mRecvTransactionTimer;
    medida::Timer& mRecvGetSCPQuorumSetTimer;
    medida::Timer& mRecvSCPQuorumSetTimer;
    medida::Timer& mRecvSCPMessageTimer;
    medida::Timer& mRecvGetSCPStateTimer;
    medida::Timer& mRecvSendMoreTimer;

    medida::Timer& mRecvSCPPrepareTimer;
    medida::Timer& mRecvSCPConfirmTimer;
    medida::Timer& mRecvSCPNominateTimer;
    medida::Timer& mRecvSCPExternalizeTimer;

    medida::Timer& mRecvSurveyRequestTimer;
    medida::Timer& mRecvSurveyResponseTimer;
    medida::Timer& mRecvStartSurveyCollectingTimer;
    medida::Timer& mRecvStopSurveyCollectingTimer;

    medida::Timer& mRecvFloodAdvertTimer;
    medida::Timer& mRecvFloodDemandTimer;

    medida::Timer& mMessageDelayInWriteQueueTimer;
    medida::Timer& mMessageDelayInAsyncWriteTimer;

    medida::Timer& mOutboundQueueDelaySCP;
    medida::Timer& mOutboundQueueDelayTxs;
    medida::Timer& mOutboundQueueDelayAdvert;
    medida::Timer& mOutboundQueueDelayDemand;
    medida::Meter& mOutboundQueueDropSCP;
    medida::Meter& mOutboundQueueDropTxs;
    medida::Meter& mOutboundQueueDropAdvert;
    medida::Meter& mOutboundQueueDropDemand;

    medida::Meter& mSendErrorMeter;
    medida::Meter& mSendHelloMeter;
    medida::Meter& mSendAuthMeter;
    medida::Meter& mSendDontHaveMeter;
    medida::Meter& mSendPeersMeter;
    medida::Meter& mSendGetTxSetMeter;
    medida::Meter& mSendTransactionMeter;
    medida::Meter& mSendTxSetMeter;
    medida::Meter& mSendGetSCPQuorumSetMeter;
    medida::Meter& mSendSCPQuorumSetMeter;
    medida::Meter& mSendSCPMessageSetMeter;
    medida::Meter& mSendGetSCPStateMeter;
    medida::Meter& mSendSendMoreMeter;

    medida::Meter& mSendSurveyRequestMeter;
    medida::Meter& mSendSurveyResponseMeter;
    medida::Meter& mSendStartSurveyCollectingMeter;
    medida::Meter& mSendStopSurveyCollectingMeter;

    medida::Meter& mSendFloodAdvertMeter;
    medida::Meter& mSendFloodDemandMeter;
    medida::Meter& mMessagesDemanded;
    medida::Meter& mMessagesFulfilledMeter;
    medida::Meter& mBannedMessageUnfulfilledMeter;
    medida::Meter& mUnknownMessageUnfulfilledMeter;
    medida::Timer& mTxPullLatency;
    medida::Timer& mPeerTxPullLatency;
    medida::Timer& mAdvertQueueDelay;

    medida::Meter& mDemandTimeouts;
    medida::Meter& mPulledRelevantTxs;
    medida::Meter& mPulledIrrelevantTxs;

    medida::Meter& mAbandonedDemandMeter;

    medida::Meter& mMessagesBroadcast;
    medida::Counter& mPendingPeersSize;
    medida::Counter& mAuthenticatedPeersSize;

    medida::Meter& mUniqueFloodBytesRecv;
    medida::Meter& mDuplicateFloodBytesRecv;
    medida::Meter& mUniqueFetchBytesRecv;
    medida::Meter& mDuplicateFetchBytesRecv;
};
}
