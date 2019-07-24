#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// This structure just exists to cache frequently-accessed, overlay-wide
// (non-peer-specific) metrics. Some of these metrics are subsequently
// tabulated at a per-peer level for purposes of identifying and
// disconnecting overloading peers, see LoadManager for details.

#include "util/BatchMetrics.h"
#include "util/Logging.h"

namespace medida
{
class Timer;
class Meter;
class Counter;
}

namespace stellar
{

class Application;

struct OverlayMetrics
{
    OverlayMetrics(Application& app);
    CachedLogLevel mLogLevel{"Overlay"};
    BatchMeter mMessageRead;
    BatchMeter mMessageWrite;
    BatchMeter mByteRead;
    BatchMeter mByteWrite;
    BatchMeter mErrorRead;
    BatchMeter mErrorWrite;
    BatchMeter mTimeoutIdle;
    BatchMeter mTimeoutStraggler;

    BatchTimer mRecvErrorTimer;
    BatchTimer mRecvHelloTimer;
    BatchTimer mRecvAuthTimer;
    BatchTimer mRecvDontHaveTimer;
    BatchTimer mRecvGetPeersTimer;
    BatchTimer mRecvPeersTimer;
    BatchTimer mRecvGetTxSetTimer;
    BatchTimer mRecvTxSetTimer;
    BatchTimer mRecvTransactionTimer;
    BatchTimer mRecvGetSCPQuorumSetTimer;
    BatchTimer mRecvSCPQuorumSetTimer;
    BatchTimer mRecvSCPMessageTimer;
    BatchTimer mRecvGetSCPStateTimer;

    BatchTimer mRecvSCPPrepareTimer;
    BatchTimer mRecvSCPConfirmTimer;
    BatchTimer mRecvSCPNominateTimer;
    BatchTimer mRecvSCPExternalizeTimer;

    BatchMeter mSendErrorMeter;
    BatchMeter mSendHelloMeter;
    BatchMeter mSendAuthMeter;
    BatchMeter mSendDontHaveMeter;
    BatchMeter mSendGetPeersMeter;
    BatchMeter mSendPeersMeter;
    BatchMeter mSendGetTxSetMeter;
    BatchMeter mSendTransactionMeter;
    BatchMeter mSendTxSetMeter;
    BatchMeter mSendGetSCPQuorumSetMeter;
    BatchMeter mSendSCPQuorumSetMeter;
    BatchMeter mSendSCPMessageSetMeter;
    BatchMeter mSendGetSCPStateMeter;

    BatchMeter mMessagesBroadcast;
    medida::Counter& mPendingPeersSize;
    medida::Counter& mAuthenticatedPeersSize;

    BatchMeter mUniqueFloodBytesRecv;
    BatchMeter mDuplicateFloodBytesRecv;
    BatchMeter mUniqueFetchBytesRecv;
    BatchMeter mDuplicateFetchBytesRecv;
};
}
