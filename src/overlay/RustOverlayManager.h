// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/TxSetFrame.h"
#include "overlay/OverlayIPC.h"
#include "overlay/OverlayMetrics.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace stellar
{

class Application;
class PeerBareAddress;
struct StellarMessage;

using TxSetXDRFrameConstPtr = std::shared_ptr<TxSetXDRFrame const>;

/**
 * RustOverlayManager delegates peer management to an external Rust process.
 *
 * All networking is routed through the Rust overlay via IPC:
 * - broadcastMessage() -> sends SCP/TX via IPC
 * - Peer discovery handled by Kademlia DHT in Rust overlay
 */
class RustOverlayManager
{
  public:
    RustOverlayManager(Application& app);
    ~RustOverlayManager();

    // Lifecycle
    void start();
    void shutdown();
    bool isShuttingDown() const;

#ifdef BUILD_TESTS
    // Advertise an additional peer address ("host:port") to the Rust overlay
    // on top of the config's KNOWN_PEERS. Used by Simulation to wire test
    // topologies; must be called before start().
    void addKnownPeerForTesting(std::string const& addr);
#endif

    // Network operations
    bool broadcastMessage(std::shared_ptr<StellarMessage const> msg,
                          std::optional<Hash> const hash = std::nullopt);
    void broadcastTransaction(TransactionEnvelope const& tx, int64_t fee,
                              uint32_t numOps);

    void clearLedgersBelow(uint32_t ledgerSeq, uint32_t lclSeq);

    // TX set management - notify that TX set was externalized with its TX
    // hashes
    void notifyTxSetExternalized(Hash const& txSetHash,
                                 std::vector<Hash> const& txHashes);

    // Request TX set from peers (via Rust overlay, async). slotIndex is the
    // slot the set is for, used to stamp the Rust-side cache entry.
    void requestTxSet(Hash const& txSetHash, uint32_t slotIndex);

    // Cache a locally-built TX set in Rust overlay. slotIndex is the slot the
    // set is for, used to stamp the Rust-side cache entry.
    void cacheTxSet(Hash const& txSetHash, std::vector<uint8_t> const& xdr,
                    uint32_t slotIndex);

    // Get top transactions from Rust overlay's mempool for TX set building.
    // Blocks until the overlay responds, shuts down, or disconnects.
    std::vector<TransactionEnvelope> getTopTransactions(size_t count);

    // Metrics and managers
    OverlayMetrics& getOverlayMetrics();

    /// Fetch the latest metrics snapshot from the Rust overlay and update
    /// the libmedida-backed OverlayMetrics counters/timers so they appear
    /// on the /metrics HTTP endpoint.
    void syncOverlayMetrics();

    // Access to IPC (for Herder to set callbacks)
    OverlayIPC&
    getOverlayIPC()
    {
        return *mOverlayIPC;
    }

  private:
    Application& mApp;
    std::unique_ptr<OverlayIPC> mOverlayIPC;
    std::atomic<bool> mShuttingDown{false};
    std::vector<std::string> mExtraKnownPeers;

    // Config KNOWN_PEERS plus any peers added via addKnownPeerForTesting.
    std::vector<std::string> effectiveKnownPeers() const;

    OverlayMetrics mOverlayMetrics;

    // For computing deltas on monotonic counters between syncs.
    // Key: metric name, Value: last synced value.
    std::unordered_map<std::string, int64_t> mLastSyncedValues;
};

} // namespace stellar
