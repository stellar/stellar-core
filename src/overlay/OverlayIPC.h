// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "overlay/IPC.h"
#include "xdr/Stellar-overlay.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace stellar
{

class Application;

/**
 * OverlayIPC manages communication with the external Rust overlay process.
 *
 * This class:
 * 1. Spawns the overlay process on startup
 * 2. Sends SCP envelopes to be broadcast
 * 3. Receives SCP envelopes from the network
 * 4. Requests TX set hashes for nomination
 *
 * The overlay process handles:
 * - Peer connections and authentication (Noise protocol)
 * - SCP message relay with deduplication
 * - TX flooding with push-k strategy
 * - Mempool with fee ordering
 */
class OverlayIPC
{
  public:
    /// Callback when SCP envelope received from network
    using SCPReceivedCallback = std::function<void(SCPEnvelope const&)>;

    /// Callback when peer requests SCP state - returns envelopes to send
    using ScpStateRequestCallback =
        std::function<std::vector<SCPEnvelope>(uint32_t ledgerSeq)>;

    /// Callback when TX set received from peers (async fetch response)
    /// (the callee may take ownership of the decoded set)
    using TxSetReceivedCallback = std::function<void(
        Hash const& hash, GeneralizedTransactionSet&& txSet)>;

    /// Callback when the overlay reports its libp2p peer id at startup
    /// (called on the IPC reader thread)
    using OverlayIdentityCallback =
        std::function<void(std::vector<uint8_t> const& peerId)>;

    /**
     * Create an OverlayIPC instance.
     *
     * @param socketPath Path for Unix domain socket, or empty for default
     * @param overlayBinaryPath Path to the overlay binary, or empty to search
     * @param peerPort Port for peer TCP connections (passed to overlay)
     * @param txSetDir Directory in which the overlay stores TX sets durably
     *        before handing them to Core, or empty for no store
     */
    OverlayIPC(std::optional<std::string> socketPath,
               std::optional<std::string> overlayBinaryPath, uint16_t peerPort,
               std::optional<std::string> txSetDir = std::nullopt);

    /// Whether the overlay process was told to store TX sets durably.
    bool
    storesTxSets() const
    {
        return mTxSetDir.has_value();
    }

    static std::string defaultSocketPath(uint16_t peerPort);
    static std::optional<std::string> findOverlayBinaryPath();

    ~OverlayIPC();

    /**
     * Start the overlay process and connect.
     *
     * @return true if started successfully
     */
    bool start();

    /**
     * Stop the overlay process.
     */
    void shutdown();

    /**
     * Broadcast an SCP envelope to all peers.
     *
     * @param envelope The SCP envelope to broadcast
     * @return true if sent successfully
     */
    bool broadcastSCP(SCPEnvelope const& envelope);

    /**
     * Notify overlay of ledger close.
     *
     * @param ledgerSeq The closed ledger sequence number
     * @param ledgerHash The closed ledger hash
     */
    void notifyLedgerClosed(uint32_t ledgerSeq, Hash const& ledgerHash);

    /**
     * Notify overlay that a TX set was externalized.
     *
     * The overlay should clear the corresponding TXs from its mempool.
     *
     * @param txSetHash The hash of the externalized TX set
     * @param txHashes The hashes of all TXs in the externalized TX set
     */
    void notifyTxSetExternalized(Hash const& txSetHash,
                                 std::vector<Hash> const& txHashes);

    /**
     * Remove transactions from the Rust mempool by hash, e.g. transactions
     * that failed validation while building a tx set and would otherwise be
     * handed back to Core on every nomination. Uses the TX_SET_EXTERNALIZED
     * message with a zero tx set hash: the overlay only acts on the hashes.
     */
    void removeTransactions(std::vector<Hash> const& txHashes);

    /**
     * Request top N transactions by fee for nomination.
     *
     * Synchronous: blocks until the overlay responds. Shutdown or loss of
     * the IPC connection unblocks the wait and returns an empty vector.
     *
     * @param count Number of transactions to request
     * @return Vector of transaction envelopes (may be less than count if
     * mempool is small)
     */
    std::vector<TransactionEnvelope> getTopTransactions(size_t count);

    /**
     * The same request, returning the response payload undecoded (empty if
     * there is no response), for callers that decode it elsewhere with
     * parseTopTransactions.
     */
    std::vector<uint8_t> getTopTransactionsPayload(size_t count);

    /**
     * Decode a GET_TOP_TXS response payload:
     * [count:4]([len:4][TransactionEnvelope XDR:len])*. Envelopes are
     * decoded in place from the payload. Entries that fail to decode are
     * skipped (with a warning); a truncated payload ends the list.
     */
    static std::vector<TransactionEnvelope>
    parseTopTransactions(std::vector<uint8_t> const& payload);

    /**
     * Submit a transaction to the overlay for flooding.
     *
     * @param tx The transaction envelope
     * @param fee Transaction fee
     * @param numOps Number of operations
     */
    void submitTransaction(TransactionEnvelope const& tx, int64_t fee,
                           uint32_t numOps);

    /**
     * Request SCP state from peers.
     * Rust overlay will ask random peers for SCP messages >= ledgerSeq.
     *
     * @param ledgerSeq Minimum ledger sequence to request
     */
    void requestScpState(uint32_t ledgerSeq);

    /**
     * Configure peer addresses for the overlay.
     *
     * @param knownPeers List of known peer addresses (host:port)
     * @param preferredPeers List of preferred peer addresses (host:port)
     * @param listenPort Local port to listen on
     */
    void setPeerConfig(std::vector<std::string> const& knownPeers,
                       std::vector<std::string> const& preferredPeers,
                       uint16_t listenPort);

    /**
     * Request a TX set by hash from peers (asynchronous).
     *
     * The Rust overlay will fetch from peers and notify via the
     * TxSetReceivedCallback when available.
     *
     * @param hash The TX set hash to request
     * @param slotIndex The slot the set is for (stamps the cache entry)
     */
    void requestTxSet(Hash const& hash, uint32_t slotIndex);

    /**
     * Cache a locally-built TX set in the Rust overlay.
     *
     * After Core builds a TX set from mempool transactions, it must
     * send the set to Rust so that Rust can serve it to other peers
     * who request it via TX set fetching.
     *
     * @param hash The TX set hash
     * @param txSet The TX set, serialized directly into the IPC message
     * @param slotIndex The slot the set is for (stamps the cache entry)
     * @return whether the set was sent to the overlay
     */
    bool cacheTxSet(Hash const& hash, GeneralizedTransactionSet const& txSet,
                    uint32_t slotIndex);

    /**
     * Push a TX set to every connected peer (leader's tx-set push).
     *
     * The set must have been handed to Rust with cacheTxSet first. Rust
     * sends it to each connected peer once, without waiting for requests;
     * peers verify and keep it by its content hash and use it only if an
     * SCP statement names that hash.
     *
     * @param hash The TX set hash
     * @param slotIndex The slot the set is proposed for (for logging)
     */
    void pushTxSet(Hash const& hash, uint32_t slotIndex);

    /**
     * Tell the overlay which validator it belongs to: `signature` is the
     * node key's signature binding `nodeID` to the overlay's peer id (see
     * RustOverlayManager::signOverlayIdentity). The overlay shows it to its
     * peers.
     */
    void setNodeIdentity(Hash const& networkID, NodeID const& nodeID,
                         Signature const& signature);

    /**
     * Tell the overlay which validators get this node's transactions now.
     * nullopt = every peer (flood). Returns whether it was sent.
     */
    bool setTxTargets(std::optional<std::vector<NodeID>> const& targets);

    /**
     * Describe the spanning trees of the next slots to the overlay: the mode
     * and fanout, the seed of each slot's tree, and the nodes of the trees.
     * Returns whether it was sent.
     */
    bool setTxSetTree(uint8_t mode, uint8_t fanout,
                      std::vector<std::pair<uint32_t, Hash>> const& slots,
                      std::vector<NodeID> const& nodes);

    /// Set callback for the overlay's peer id report
    void setOnOverlayIdentity(OverlayIdentityCallback cb);

    /// Set callback for received SCP envelopes
    void setOnSCPReceived(SCPReceivedCallback cb);

    /// Set callback for SCP state requests from peers
    void setOnScpStateRequest(ScpStateRequestCallback cb);

    /// Set callback for TX set received from peers (async fetch)
    void setOnTxSetReceived(TxSetReceivedCallback cb);

    /**
     * Request overlay metrics snapshot from Rust overlay.
     *
     * Synchronous call — blocks until the Rust overlay responds with
     * a JSON-serialized metrics snapshot, or timeout.
     *
     * @param timeoutMs Timeout in milliseconds
     * @return JSON string with the overlay metrics, empty on timeout/error
     */
    std::string requestMetrics(int timeoutMs = 1000);

    /// Check if connected to overlay
    bool isConnected() const;

    /// Get the socket path
    std::string const&
    getSocketPath() const
    {
        return mSocketPath;
    }

  private:
    /// Spawn the overlay process
    bool spawnOverlay();

    std::optional<std::string> resolveOverlayBinaryPath() const;

    /// Reader thread function
    void readerLoop();

    /// Handle a received IPC message
    void handleMessage(IPCMessage const& msg);

    /// Send SCP state response to overlay with request ID for correlation
    void sendScpStateResponse(uint64_t requestId,
                              std::vector<SCPEnvelope> const& envelopes);

    std::string mSocketPath;
    std::optional<std::string> mOverlayBinaryPath;
    std::optional<std::string> mTxSetDir;
    uint16_t mPeerPort;

    std::unique_ptr<IPCChannel> mChannel;
    std::thread mReaderThread;
    std::atomic<bool> mRunning{false};
    // True while the reader thread can still deliver responses; cleared on
    // reader exit (disconnect) to unblock getTopTransactions waiters.
    std::atomic<bool> mReaderAlive{false};

    pid_t mOverlayPid{-1};

    SCPReceivedCallback mOnSCPReceived;
    ScpStateRequestCallback mOnScpStateRequest;
    TxSetReceivedCallback mOnTxSetReceived;
    OverlayIdentityCallback mOnOverlayIdentity;

    // For synchronous request/response (getTopTransactions)
    std::mutex mRequestMutex;
    std::condition_variable mRequestCv;
    std::optional<IPCMessage> mPendingResponse;

    // For synchronous metrics request/response
    std::mutex mMetricsMutex;
    std::condition_variable mMetricsCv;
    std::optional<IPCMessage> mPendingMetricsResponse;

    // Protects mChannel->send() - channel is not thread-safe
    mutable std::mutex mSendMutex;
};

} // namespace stellar
