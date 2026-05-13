// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/RustOverlayManager.h"
#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "lib/json/json.h"
#include "main/Application.h"
#include "util/Backtrace.h"
#include "util/Logging.h"
#include "xdr/Stellar-overlay.h"
#include <medida/counter.h>
#include <medida/histogram.h>
#include <medida/meter.h>
#include <medida/timer.h>

namespace stellar
{

RustOverlayManager::RustOverlayManager(Application& app)
    : mApp(app), mOverlayMetrics(app)
{
    auto const& cfg = mApp.getConfig();

    CLOG_INFO(Overlay, "Creating RustOverlayManager with port={}",
              cfg.PEER_PORT);

    mOverlayIPC = std::make_unique<OverlayIPC>(
        cfg.OVERLAY_SOCKET_PATH, cfg.OVERLAY_BINARY_PATH, cfg.PEER_PORT,
        cfg.NODE_SEED.getPublicKey());
}

RustOverlayManager::~RustOverlayManager()
{
    shutdown();
}

void
RustOverlayManager::start()
{
    auto const& cfg = mApp.getConfig();

    if (cfg.RUN_STANDALONE)
    {
        CLOG_INFO(Overlay,
                  "Skipping RustOverlayManager start in standalone mode");
        return;
    }

    CLOG_INFO(Overlay, "Starting RustOverlayManager");

    mOverlayIPC->setOnSCPReceived([this](SCPEnvelope const& env) {
        mApp.postOnMainThread(
            [this, env]() { mApp.getHerder().recvSCPEnvelope(env); },
            "RustOverlayManager: SCPReceived");
    });

    mOverlayIPC->setOnScpStateRequest([this](uint32_t ledgerSeq) {
        // Called from IPC reader thread - collect SCP state synchronously
        return mApp.getHerder().getSCPStateForPeer(ledgerSeq);
    });

    mOverlayIPC->setOnTxSetReceived(
        [this](Hash const& hash, GeneralizedTransactionSet const& txSet) {
            // Called from IPC reader thread - post to main thread
            auto frame = TxSetXDRFrame::makeFromWire(txSet);
            mApp.postOnMainThread(
                [this, hash, frame]() {
                    mApp.getHerder().recvTxSet(hash, frame);
                },
                "RustOverlayManager: TxSetReceived");
        });

    if (!mOverlayIPC->start())
    {
        CLOG_ERROR(Overlay, "Failed to start Rust overlay process");
        printCurrentBacktrace();
        throw std::runtime_error("Failed to start Rust overlay");
    }

    mOverlayIPC->setPeerConfig(cfg.KNOWN_PEERS, cfg.PREFERRED_PEERS,
                               cfg.PEER_PORT);

    CLOG_INFO(Overlay, "RustOverlayManager started, peer_port={}",
              cfg.PEER_PORT);
}

void
RustOverlayManager::shutdown()
{
    if (mShuttingDown.exchange(true))
    {
        return;
    }

    CLOG_INFO(Overlay, "Shutting down RustOverlayManager");
    if (mOverlayIPC)
    {
        mOverlayIPC->shutdown();
    }
}

bool
RustOverlayManager::isShuttingDown() const
{
    return mShuttingDown.load();
}

bool
RustOverlayManager::broadcastMessage(std::shared_ptr<StellarMessage const> msg,
                                     std::optional<Hash> const hash)
{
    if (mShuttingDown.load() || !mOverlayIPC->isConnected())
    {
        return false;
    }

    if (msg->type() == SCP_MESSAGE)
    {
        return mOverlayIPC->broadcastSCP(msg->envelope());
    }
    else if (msg->type() == TRANSACTION)
    {
        auto const& env = msg->transaction();
        int64_t fee = env.type() == ENVELOPE_TYPE_TX_V0 ? env.v0().tx.fee
                                                        : env.v1().tx.fee;
        uint32_t numOps =
            env.type() == ENVELOPE_TYPE_TX_V0
                ? static_cast<uint32_t>(env.v0().tx.operations.size())
                : static_cast<uint32_t>(env.v1().tx.operations.size());
        mOverlayIPC->submitTransaction(env, fee, numOps);
        return true;
    }

    return false;
}

void
RustOverlayManager::broadcastTransaction(TransactionEnvelope const& tx,
                                         int64_t fee, uint32_t numOps)
{
    if (mOverlayIPC && !mShuttingDown)
    {
        mOverlayIPC->submitTransaction(tx, fee, numOps);
    }
}

void
RustOverlayManager::clearLedgersBelow(uint32_t ledgerSeq, uint32_t lclSeq)
{
    if (mOverlayIPC && mOverlayIPC->isConnected())
    {
        Hash dummyHash;
        mOverlayIPC->notifyLedgerClosed(lclSeq, dummyHash);
    }
}

void
RustOverlayManager::notifyTxSetExternalized(Hash const& txSetHash,
                                            std::vector<Hash> const& txHashes)
{
    if (mOverlayIPC && !mShuttingDown)
    {
        mOverlayIPC->notifyTxSetExternalized(txSetHash, txHashes);
    }
}

void
RustOverlayManager::requestTxSet(Hash const& txSetHash)
{
    if (mOverlayIPC && !mShuttingDown)
    {
        mOverlayIPC->requestTxSet(txSetHash);
    }
}

void
RustOverlayManager::cacheTxSet(Hash const& txSetHash,
                               std::vector<uint8_t> const& xdr)
{
    if (mOverlayIPC && !mShuttingDown)
    {
        mOverlayIPC->cacheTxSet(txSetHash, xdr);
    }
}

std::vector<TransactionEnvelope>
RustOverlayManager::getTopTransactions(size_t count, int timeoutMs)
{
    if (mOverlayIPC && !mShuttingDown)
    {
        return mOverlayIPC->getTopTransactions(count, timeoutMs);
    }
    return {};
}

OverlayMetrics&
RustOverlayManager::getOverlayMetrics()
{
    return mOverlayMetrics;
}

// Helper: compute delta between current and last-synced value for a monotonic
// counter, update last-synced, and mark the medida Meter. Returns the delta.
static int64_t
markMeterDelta(medida::Meter& meter, int64_t currentValue,
               std::unordered_map<std::string, int64_t>& lastSynced,
               std::string const& key)
{
    int64_t last = 0;
    auto it = lastSynced.find(key);
    if (it != lastSynced.end())
    {
        last = it->second;
    }
    int64_t delta = currentValue - last;
    if (delta > 0)
    {
        meter.Mark(delta);
    }
    lastSynced[key] = currentValue;
    return delta;
}

void
RustOverlayManager::syncOverlayMetrics()
{
    if (!mOverlayIPC || !mOverlayIPC->isConnected() || mShuttingDown)
    {
        return;
    }

    auto jsonStr = mOverlayIPC->requestMetrics(/* timeoutMs */ 500);
    if (jsonStr.empty())
    {
        CLOG_DEBUG(Overlay, "No overlay metrics received (timeout or error)");
        return;
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(jsonStr, root))
    {
        CLOG_WARNING(Overlay, "Failed to parse overlay metrics JSON");
        return;
    }

    auto& m = mOverlayMetrics;

    // ── Gauges (set counter value directly) ──
    if (root.isMember("connection_authenticated"))
    {
        auto val = root["connection_authenticated"].asInt64();
        // Counter has set_count in medida — use increment approach:
        // Counter is a gauge, so reset and set.
        auto current = m.mAuthenticatedPeersSize.count();
        m.mAuthenticatedPeersSize.inc(val - current);
    }
    if (root.isMember("connection_pending"))
    {
        auto val = root["connection_pending"].asInt64();
        auto current = m.mPendingPeersSize.count();
        m.mPendingPeersSize.inc(val - current);
    }

    // ── recv-transaction SimpleTimer ──
    // SimpleTimer only supports Update(duration) — compute deltas and
    // issue individual updates with average duration.
    if (root.isMember("recv_transaction_sum_us") &&
        root.isMember("recv_transaction_count"))
    {
        auto sum =
            static_cast<int64_t>(root["recv_transaction_sum_us"].asUInt64());
        auto count =
            static_cast<int64_t>(root["recv_transaction_count"].asUInt64());
        auto lastSum = mLastSyncedValues["recv_transaction_sum_us"];
        auto lastCount = mLastSyncedValues["recv_transaction_count"];
        auto deltaSum = sum - lastSum;
        auto deltaCount = count - lastCount;
        if (deltaCount > 0 && deltaSum > 0)
        {
            auto avgUs = deltaSum / deltaCount;
            for (int64_t i = 0; i < deltaCount; ++i)
            {
                m.mRecvTransactionTimer.Update(
                    std::chrono::microseconds{avgUs});
            }
        }
        mLastSyncedValues["recv_transaction_sum_us"] = sum;
        mLastSyncedValues["recv_transaction_count"] = count;
    }

    // ── Monotonic counters → Meter deltas ──

    auto markDelta = [&](medida::Meter& meter, std::string const& jsonField) {
        if (root.isMember(jsonField))
        {
            markMeterDelta(meter, root[jsonField].asInt64(), mLastSyncedValues,
                           jsonField);
        }
    };

    markDelta(m.mByteRead, "byte_read");
    markDelta(m.mByteWrite, "byte_write");
    markDelta(m.mMessageRead, "message_read");
    markDelta(m.mMessageWrite, "message_write");
    markDelta(m.mMessagesBroadcast, "message_broadcast");
    markDelta(m.mMessageDrop, "message_drop");
    markDelta(m.mErrorRead, "error_read");
    markDelta(m.mErrorWrite, "error_write");

    // Flood metrics
    markDelta(m.mSendFloodAdvertMeter, "flood_advertised");
    markDelta(m.mMessagesDemanded, "flood_demanded");
    markDelta(m.mMessagesFulfilledMeter, "flood_fulfilled");
    markDelta(m.mUnknownMessageUnfulfilledMeter, "flood_unfulfilled_unknown");
    markDelta(m.mUniqueFloodBytesRecv, "flood_unique_recv");
    markDelta(m.mDuplicateFloodBytesRecv, "flood_duplicate_recv");
    markDelta(m.mAbandonedDemandMeter, "flood_abandoned_demands");
    markDelta(m.mDemandTimeouts, "demand_timeout");

    // Send meters per message type
    markDelta(m.mSendSCPMessageSetMeter, "send_scp_message");
    markDelta(m.mSendTransactionMeter, "send_transaction");
    markDelta(m.mSendTxSetMeter, "send_txset");

    // TX set shard dissemination metrics
    markDelta(m.mTxSetShardBroadcast, "txset_shard_broadcast");
    markDelta(m.mTxSetShardOriginalSent, "txset_shard_original_sent");
    markDelta(m.mTxSetShardRecoverySent, "txset_shard_recovery_sent");
    markDelta(m.mTxSetShardOriginalRecvUnique,
              "txset_shard_original_recv_unique");
    markDelta(m.mTxSetShardRecoveryRecvUnique,
              "txset_shard_recovery_recv_unique");
    markDelta(m.mTxSetShardOriginalRecvRedundant,
              "txset_shard_original_recv_redundant");
    markDelta(m.mTxSetShardRecoveryRecvRedundant,
              "txset_shard_recovery_recv_redundant");
    markDelta(m.mTxSetShardOriginalForwarded, "txset_shard_original_forwarded");
    markDelta(m.mTxSetShardRecoveryForwarded, "txset_shard_recovery_forwarded");
    markDelta(m.mTxSetShardReconstructSuccessOriginal,
              "txset_shard_reconstruct_success_original");
    markDelta(m.mTxSetShardReconstructSuccessRecovery,
              "txset_shard_reconstruct_success_recovery");
    markDelta(m.mTxSetShardReconstructFailOriginal,
              "txset_shard_reconstruct_fail_original");
    markDelta(m.mTxSetShardReconstructFailRecovery,
              "txset_shard_reconstruct_fail_recovery");
    markDelta(m.mTxSetShardFetchPreempted, "txset_shard_fetch_preempted");
    markDelta(m.mTxSetShardEagerAlsoServed, "txset_shard_eager_also_served");

    // Connection lifecycle — these aren't registered as medida meters on
    // the C++ side yet, so they'll just be tracked by the existing counters.
    // The inbound/outbound attempt/establish/drop are already covered
    // by the send/recv metrics or connection gauges above.

    // ── Timer summaries ──
    // For recv SCP timer, we compute the average duration per call
    // and update the medida Timer accordingly.
    if (root.isMember("recv_scp_sum_us") && root.isMember("recv_scp_count"))
    {
        auto sum = static_cast<int64_t>(root["recv_scp_sum_us"].asUInt64());
        auto count = static_cast<int64_t>(root["recv_scp_count"].asUInt64());
        auto lastSum = mLastSyncedValues["recv_scp_sum_us"];
        auto lastCount = mLastSyncedValues["recv_scp_count"];
        auto deltaSum = sum - lastSum;
        auto deltaCount = count - lastCount;
        if (deltaCount > 0 && deltaSum > 0)
        {
            auto avgUs = deltaSum / deltaCount;
            for (int64_t i = 0; i < deltaCount; ++i)
            {
                m.mRecvSCPMessageTimer.Update(std::chrono::microseconds{avgUs});
            }
        }
        mLastSyncedValues["recv_scp_sum_us"] = sum;
        mLastSyncedValues["recv_scp_count"] = count;
    }

    // TX batch size histogram
    if (root.isMember("flood_tx_batch_size_sum") &&
        root.isMember("flood_tx_batch_size_count"))
    {
        auto sum =
            static_cast<int64_t>(root["flood_tx_batch_size_sum"].asUInt64());
        auto count =
            static_cast<int64_t>(root["flood_tx_batch_size_count"].asUInt64());
        auto lastSum = mLastSyncedValues["flood_tx_batch_size_sum"];
        auto lastCount = mLastSyncedValues["flood_tx_batch_size_count"];
        auto deltaSum = sum - lastSum;
        auto deltaCount = count - lastCount;
        if (deltaCount > 0 && deltaSum > 0)
        {
            auto avg = deltaSum / deltaCount;
            for (int64_t i = 0; i < deltaCount; ++i)
            {
                m.mTxBatchSizeHistogram.Update(avg);
            }
        }
        mLastSyncedValues["flood_tx_batch_size_sum"] = sum;
        mLastSyncedValues["flood_tx_batch_size_count"] = count;
    }

    // ── Fetch TxSet timer ──
    if (root.isMember("fetch_txset_sum_us") &&
        root.isMember("fetch_txset_count"))
    {
        auto sum = static_cast<int64_t>(root["fetch_txset_sum_us"].asUInt64());
        auto count = static_cast<int64_t>(root["fetch_txset_count"].asUInt64());
        auto lastSum = mLastSyncedValues["fetch_txset_sum_us"];
        auto lastCount = mLastSyncedValues["fetch_txset_count"];
        auto deltaSum = sum - lastSum;
        auto deltaCount = count - lastCount;
        if (deltaCount > 0 && deltaSum > 0)
        {
            auto avgUs = deltaSum / deltaCount;
            for (int64_t i = 0; i < deltaCount; ++i)
            {
                m.mFetchTxSetTimer.Update(std::chrono::microseconds{avgUs});
            }
        }
        mLastSyncedValues["fetch_txset_sum_us"] = sum;
        mLastSyncedValues["fetch_txset_count"] = count;
    }

    // ── Recovery-assisted TxSet shard reconstruction timer ──
    if (root.isMember("txset_shard_reconstruct_recovery_sum_us") &&
        root.isMember("txset_shard_reconstruct_recovery_count"))
    {
        auto sum = static_cast<int64_t>(
            root["txset_shard_reconstruct_recovery_sum_us"].asUInt64());
        auto count = static_cast<int64_t>(
            root["txset_shard_reconstruct_recovery_count"].asUInt64());
        auto lastSum =
            mLastSyncedValues["txset_shard_reconstruct_recovery_sum_us"];
        auto lastCount =
            mLastSyncedValues["txset_shard_reconstruct_recovery_count"];
        auto deltaSum = sum - lastSum;
        auto deltaCount = count - lastCount;
        if (deltaCount > 0 && deltaSum > 0)
        {
            auto avgUs = deltaSum / deltaCount;
            for (int64_t i = 0; i < deltaCount; ++i)
            {
                m.mTxSetShardReconstructRecoveryTimer.Update(
                    std::chrono::microseconds{avgUs});
            }
        }
        mLastSyncedValues["txset_shard_reconstruct_recovery_sum_us"] = sum;
        mLastSyncedValues["txset_shard_reconstruct_recovery_count"] = count;
    }

    // ── Flood TX pull latency timer ──
    if (root.isMember("flood_tx_pull_latency_sum_us") &&
        root.isMember("flood_tx_pull_latency_count"))
    {
        auto sum = static_cast<int64_t>(
            root["flood_tx_pull_latency_sum_us"].asUInt64());
        auto count = static_cast<int64_t>(
            root["flood_tx_pull_latency_count"].asUInt64());
        auto lastSum = mLastSyncedValues["flood_tx_pull_latency_sum_us"];
        auto lastCount = mLastSyncedValues["flood_tx_pull_latency_count"];
        auto deltaSum = sum - lastSum;
        auto deltaCount = count - lastCount;
        if (deltaCount > 0 && deltaSum > 0)
        {
            auto avgUs = deltaSum / deltaCount;
            for (int64_t i = 0; i < deltaCount; ++i)
            {
                m.mTxPullLatency.Update(std::chrono::microseconds{avgUs});
            }
        }
        mLastSyncedValues["flood_tx_pull_latency_sum_us"] = sum;
        mLastSyncedValues["flood_tx_pull_latency_count"] = count;
    }

    // ── Memory gauge ──
    if (root.isMember("memory_flood_known"))
    {
        // This is informational — exposed via the metrics snapshot
        // but doesn't have a dedicated C++ medida metric yet.
        // Could be added as a Counter if needed.
    }

    CLOG_TRACE(Overlay, "Synced overlay metrics from Rust overlay");
}

} // namespace stellar
