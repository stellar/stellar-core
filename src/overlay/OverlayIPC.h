// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "overlay/IPC.h"
#include "xdr/Stellar-overlay.h"
#include <functional>
#include <memory>
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
    using TxSetReceivedCallback = std::function<void(
        Hash const& hash, GeneralizedTransactionSet const& txSet)>;

    /**
     * Create an OverlayIPC instance.
     *
     * @param socketPath Path for Unix domain socket
     * @param overlayBinaryPath Path to the overlay binary (stellar-overlay)
     * @param peerPort Port for peer TCP connections (passed to overlay)
     */
    OverlayIPC(std::string socketPath, std::string overlayBinaryPath,
               uint16_t peerPort);

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
     * Request top N transactions by fee for nomination.
     *
     * This is a synchronous call that blocks until response received
     * or timeout expires.
     *
     * @param count Number of transactions to request
     * @param timeoutMs Timeout in milliseconds
     * @return Vector of transaction envelopes (may be less than count if
     * mempool is small)
     */
    std::vector<TransactionEnvelope> getTopTransactions(size_t count,
                                                        int timeoutMs = 1000);

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
     */
    void requestTxSet(Hash const& hash);

    /**
     * Cache a locally-built TX set in the Rust overlay.
     *
     * After Core builds a TX set from mempool transactions, it must
     * send the set to Rust so that Rust can serve it to other peers
     * who request it via TX set fetching.
     *
     * @param hash The TX set hash
     * @param xdr The serialized TX set XDR
     */
    void cacheTxSet(Hash const& hash, std::vector<uint8_t> const& xdr);

    /// Set callback for received SCP envelopes
    void setOnSCPReceived(SCPReceivedCallback cb);

    /// Set callback for SCP state requests from peers
    void setOnScpStateRequest(ScpStateRequestCallback cb);

    /// Set callback for TX set received from peers (async fetch)
    void setOnTxSetReceived(TxSetReceivedCallback cb);

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

    /// Reader thread function
    void readerLoop();

    /// Handle a received IPC message
    void handleMessage(IPCMessage const& msg);

    /// Send SCP state response to overlay with request ID for correlation
    void sendScpStateResponse(uint64_t requestId,
                              std::vector<SCPEnvelope> const& envelopes);

    std::string mSocketPath;
    std::string mOverlayBinaryPath;
    uint16_t mPeerPort;

    std::unique_ptr<IPCChannel> mChannel;
    std::thread mReaderThread;
    std::atomic<bool> mRunning{false};

    pid_t mOverlayPid{-1};

    SCPReceivedCallback mOnSCPReceived;
    ScpStateRequestCallback mOnScpStateRequest;
    TxSetReceivedCallback mOnTxSetReceived;

    // For synchronous request/response
    std::mutex mRequestMutex;
    std::condition_variable mRequestCv;
    std::optional<IPCMessage> mPendingResponse;

    // Protects mChannel->send() - channel is not thread-safe
    mutable std::mutex mSendMutex;
};

} // namespace stellar
