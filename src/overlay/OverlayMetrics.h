// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Overlay-wide (non-peer-specific) metrics synced from the Rust overlay
// process.  Legacy per-message-type recv/send timers and C++-only queue
// metrics have been removed — the Rust overlay uses different stream
// protocols and doesn't have the old per-message framing.

#include "util/SimpleTimer.h"
namespace medida
{
class Timer;
class Meter;
class Counter;
class Histogram;
}

namespace stellar
{

class Application;

// OverlayMetrics is a thread-safe struct
struct OverlayMetrics
{
    OverlayMetrics(Application& app);

    // ── Byte / message throughput ──
    medida::Meter& mMessageRead;
    medida::Meter& mMessageWrite;
    medida::Meter& mMessageDrop;
    medida::Meter& mByteRead;
    medida::Meter& mByteWrite;
    medida::Meter& mErrorRead;
    medida::Meter& mErrorWrite;

    // ── Recv timers (aggregate) ──
    // SimpleTimer: high-frequency TX recv path
    SimpleTimer& mRecvTransactionTimer;
    medida::Timer& mRecvSCPMessageTimer;

    // ── Send meters (per logical message type) ──
    medida::Meter& mSendSCPMessageSetMeter;
    medida::Meter& mSendTransactionMeter;
    medida::Meter& mSendTxSetMeter;
    medida::Meter& mSendFloodAdvertMeter;

    // ── Flood / demand metrics ──
    medida::Meter& mMessagesDemanded;
    medida::Meter& mMessagesFulfilledMeter;
    medida::Meter& mUnknownMessageUnfulfilledMeter;
    medida::Timer& mTxPullLatency;
    medida::Meter& mDemandTimeouts;
    medida::Meter& mAbandonedDemandMeter;

    // ── Broadcast / dedup ──
    medida::Meter& mMessagesBroadcast;
    medida::Meter& mUniqueFloodBytesRecv;
    medida::Meter& mDuplicateFloodBytesRecv;
    medida::Histogram& mTxBatchSizeHistogram;

    // ── Connection gauges ──
    medida::Counter& mPendingPeersSize;
    medida::Counter& mAuthenticatedPeersSize;

    // ── TxSet fetch latency ──
    medida::Timer& mFetchTxSetTimer;

    // ── TxSet shard dissemination ──
    medida::Meter& mTxSetShardBroadcast;
    medida::Meter& mTxSetShardOriginalSent;
    medida::Meter& mTxSetShardRecoverySent;
    medida::Meter& mTxSetShardOriginalRecvUnique;
    medida::Meter& mTxSetShardRecoveryRecvUnique;
    medida::Meter& mTxSetShardOriginalRecvRedundant;
    medida::Meter& mTxSetShardRecoveryRecvRedundant;
    medida::Meter& mTxSetShardOriginalForwarded;
    medida::Meter& mTxSetShardRecoveryForwarded;
    medida::Meter& mTxSetShardReconstructSuccessOriginal;
    medida::Meter& mTxSetShardReconstructSuccessRecovery;
    medida::Meter& mTxSetShardReconstructFailOriginal;
    medida::Meter& mTxSetShardReconstructFailRecovery;
    medida::Timer& mTxSetShardReconstructRecoveryTimer;
    medida::Meter& mTxSetShardFetchPreempted;
    medida::Meter& mTxSetShardEagerAlsoServed;
};
}
