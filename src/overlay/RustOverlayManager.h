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
    // topologies. May be called before start() (the peer is included in the
    // initial peer config) or after it (the updated peer list is pushed to the
    // Rust overlay immediately).
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

    // Drop transactions from the Rust mempool (e.g. ones that failed
    // validation while building a tx set).
    void removeTransactions(std::vector<Hash> const& txHashes);

    // Request TX set from peers (via Rust overlay, async). slotIndex is the
    // slot the set is for, used to stamp the Rust-side cache entry.
    void requestTxSet(Hash const& txSetHash, uint32_t slotIndex);

    // Cache a locally-built TX set in Rust overlay. slotIndex is the slot the
    // set is for, used to stamp the Rust-side cache entry.
    // Returns true if the overlay also stores the set durably (see
    // storesTxSets): it does so before it handles anything Core sends after
    // this, e.g. the SCP statement that proposes the set.
    bool cacheTxSet(Hash const& txSetHash,
                    GeneralizedTransactionSet const& txSet, uint32_t slotIndex);

    // Send a TX set that was handed to the overlay with cacheTxSet to every
    // connected peer now, without waiting for their requests. Called by a
    // round leader for the set it is about to propose. Peers keep a pushed
    // set by its content hash and use it only if an SCP statement names it;
    // a peer that did not get it requests it as before.
    void pushTxSet(Hash const& txSetHash, uint32_t slotIndex);

#ifdef BUILD_TESTS
    // Make pushTxSet a no-op, as if every push were lost: peers then get
    // the leader's set through the request path only.
    void
    setDropTxSetPushForTesting(bool drop)
    {
        mDropTxSetPushForTesting = drop;
    }
#endif

    // Tell the overlay which validators get this node's transactions now:
    // it sends the transactions Core accepts (and the ones still pending)
    // straight to those nodes instead of flooding them. nullopt = every peer
    // (the flood). Only HerderImpl::updateTxFloodTargets decides this.
    // Returns whether the overlay was told.
    bool setTxFloodTargets(std::optional<std::vector<NodeID>> const& targets);

    // Describe the spanning trees of the next slots to the overlay (see
    // HerderImpl::updateTxSetTree): with them a leader's pushTxSet goes to
    // its children in the slot's tree, and every node relays what its
    // parent sends it. Returns whether the overlay was told.
    bool setTxSetTree(uint8_t mode, uint8_t fanout,
                      std::vector<OverlayIPC::TxSetTreeSlot> const& slots,
                      std::vector<NodeID> const& nodes);

    // What a validator signs to say "the overlay with this libp2p peer id is
    // mine": SHA256(networkID || "stellar-overlay-peer-id" || peerId). The
    // tag keeps it apart from everything else the node key signs.
    static Hash overlayIdentityBinding(Hash const& networkID,
                                       std::vector<uint8_t> const& peerId);

    // Whether the overlay process writes every TX set to stable storage
    // before it hands it to Core (a set received from a peer) or broadcasts
    // anything that refers to it (a set cached by Core). When it does, Core
    // does not store those sets itself, and gets them back from the overlay
    // after a restart.
    bool storesTxSets() const;

    // Get top transactions from Rust overlay's mempool for TX set building.
    // Blocks until the overlay responds, shuts down, or disconnects.
    std::vector<TransactionEnvelope> getTopTransactions(size_t count);
    // The same, undecoded (see OverlayIPC::getTopTransactionsPayload).
    std::vector<uint8_t> getTopTransactionsPayload(size_t count);

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
#ifdef BUILD_TESTS
    bool mDropTxSetPushForTesting{false};
#endif

    // Config KNOWN_PEERS plus any peers added via addKnownPeerForTesting.
    std::vector<std::string> effectiveKnownPeers() const;

    OverlayMetrics mOverlayMetrics;

    // For computing deltas on monotonic counters between syncs.
    // Key: metric name, Value: last synced value.
    std::unordered_map<std::string, int64_t> mLastSyncedValues;
};

} // namespace stellar
