// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Overlay-wide (non-peer-specific) metrics synced from the Rust overlay
// process.  Legacy per-message-type recv/send timers and C++-only queue
// metrics have been removed — the Rust overlay uses different stream
// protocols and doesn't have the old per-message framing.

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
    medida::Timer& mRecvTransactionTimer;
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

    // ── Leader tx-set push (synced from the Rust overlay) ──
    medida::Meter& mTxSetPushSent;
    medida::Meter& mTxSetPushReceived;
    medida::Meter& mTxSetPushHit;
    medida::Meter& mTxSetPushRequestSkipped;
    medida::Meter& mTxSetPushDropped;
    medida::Meter& mTxSetPushEvicted;

    // Spanning tree for the leader's tx-set push (kept by the Rust overlay;
    // see overlay/src/metrics.rs for what each counts).
    medida::Meter& mTxsetPushSentBytes;
    medida::Meter& mTxsetTreeRootSent;
    medida::Meter& mTxsetTreeRelayed;
    medida::Meter& mTxsetTreeRelayedBytes;
    medida::Meter& mTxsetTreeReceived;
    medida::Meter& mTxsetTreeDuplicate;
    medida::Meter& mTxsetTreeRefused;
    medida::Meter& mTxsetTreeUnreachable;
    medida::Meter& mTxsetTreeChildBusy;
    medida::Meter& mTxsetTreeRelayBusy;
    medida::Meter& mTxsetTreeRelayLimited;
    medida::Meter& mTxsetTreeFetchDeferred;
    medida::Meter& mTxsetTreeFetchFallback;
    medida::Meter& mTxsetTreeDelayMsSum;
    medida::Meter& mTxsetTreeDelayCount;
    medida::Meter& mTxsetTreeDelayHop1MsSum;
    medida::Meter& mTxsetTreeDelayHop1Count;
    medida::Meter& mTxsetTreeDelayHop2MsSum;
    medida::Meter& mTxsetTreeDelayHop2Count;
    medida::Meter& mTxsetTreeDelayLe50;
    medida::Meter& mTxsetTreeDelayLe100;
    medida::Meter& mTxsetTreeDelayLe150;
    medida::Meter& mTxsetTreeDelayLe200;
    medida::Meter& mTxsetTreeDelayLe300;
    medida::Meter& mTxsetTreeDelayGt300;

    // Erasure-coded pieces of the leader's tx set (EXPERIMENTAL_TX_SET_TREE
    // = "coded")
    medida::Meter& mTxsetTreeWithheld;
    medida::Meter& mTxsetCodedSetsEncoded;
    medida::Meter& mTxsetCodedPiecesSent;
    medida::Meter& mTxsetCodedPiecesRelayed;
    medida::Meter& mTxsetCodedPiecesReceived;
    medida::Meter& mTxsetCodedPiecesLate;
    medida::Meter& mTxsetCodedPiecesRefused;
    medida::Meter& mTxsetCodedPiecesInvalid;
    medida::Meter& mTxsetCodedPiecesDuplicate;
    medida::Meter& mTxsetCodedPiecesSkipped;
    medida::Meter& mTxsetCodedSetsRebuilt;
    medida::Meter& mTxsetCodedSetsRebuiltWithParity;
    medida::Meter& mTxsetCodedSetsFailed;
    medida::Meter& mTxsetCodedSetsInconsistent;
    medida::Meter& mTxsetCodedRebuildUsSum;

    // Transactions sent to the target leaders only (tx-flood-to-leaders)
    medida::Meter& mTxDirectSent;
    medida::Meter& mTxDirectSentBytes;
    medida::Meter& mTxDirectRecv;
    medida::Meter& mTxLateCopy;
    medida::Meter& mTxLocalFlooded;
    medida::Meter& mNodeIdRejected;
    medida::Counter& mNodeIdIdentified;
};
}
