// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace stellar
{

/**
 * IPC message types for communication between Core and Overlay processes.
 *
 * Message format over Unix domain sockets (SOCK_STREAM):
 * ┌──────────────────────────────────────────────────────────────────┐
 * │  0      1      2      3      4      5      6      7     ...      │
 * ├──────────────────────────────────────────────────────────────────┤
 * │    Message Type (u32)    │    Payload Length (u32)    │ Payload  │
 * └──────────────────────────────────────────────────────────────────┘
 */

enum class IPCMessageType : uint32_t
{
    // ═══ Core → Overlay (Critical Path) ═══

    /// Broadcast this SCP envelope to all peers
    BROADCAST_SCP = 1,

    /// Request top N transactions from mempool for nomination
    /// Payload: [count:4]
    GET_TOP_TXS = 2,

    /// Request current SCP state (peer asked via GET_SCP_STATE)
    REQUEST_SCP_STATE = 3,

    // ═══ Core → Overlay (Non-Critical) ═══

    /// Ledger closed, here's the new state
    LEDGER_CLOSED = 4,

    /// We externalized this TX set, drop related TXs from mempool
    /// Payload: [txSetHash:32][numTxHashes:4][txHash1:32][txHash2:32]...
    TX_SET_EXTERNALIZED = 5,

    /// Response: here's the SCP state you requested
    SCP_STATE_RESPONSE = 6,

    /// Shutdown the overlay process
    SHUTDOWN = 7,

    /// Configure bootstrap peer addresses for Kademlia DHT
    /// Payload: JSON { "known_peers": [...], "preferred_peers": [...],
    /// "listen_port": u16 }
    SET_PEER_CONFIG = 8,

    /// Submit a transaction for flooding
    /// Payload: [fee:i64][numOps:u32][txEnvelope XDR...]
    SUBMIT_TX = 10,

    /// Request a TX set by hash (async - response via TX_SET_AVAILABLE)
    /// Payload: [hash:32]
    REQUEST_TX_SET = 11,

    /// Cache a locally-built TX set so Rust can serve it to peers
    /// Payload: [hash:32][txSetXDR...]
    CACHE_TX_SET = 12,

    /// Request overlay metrics snapshot (empty payload)
    /// Response: OVERLAY_METRICS_RESPONSE with JSON payload
    REQUEST_OVERLAY_METRICS = 13,

    /// Push a cached TX set to every connected peer now, without waiting
    /// for their requests. Sent for the set this node is about to propose
    /// as a round leader, after the CACHE_TX_SET that carried the set.
    /// Payload: [hash:32][slotSeq:4]
    PUSH_TX_SET = 14,

    /// This node's validator identity, in answer to OVERLAY_IDENTITY: the
    /// node key's signature over
    /// SHA256(networkID || "stellar-overlay-peer-id" || peerId). The overlay
    /// shows it to its peers, which is how a peer connection is tied to a
    /// NodeID.
    /// Payload: [networkID:32][nodeID:32][signature:64]
    SET_NODE_IDENTITY = 15,

    /// The validators that get this node's transactions now (see
    /// HerderImpl::computeTxFloodTargets).
    /// Payload: [everyone:1][count:4][nodeID:32]...
    /// everyone = 1: flood to all peers (the list is ignored).
    SET_TX_TARGETS = 16,

    /// The spanning trees the leader's TX set travels down in the next
    /// slots (see HerderImpl::updateTxSetTree and
    /// overlay/src/flood/txset_tree.rs).
    /// Payload: [mode:1][fanout:1][slots:1]
    ///          ([slot:4][seed:32][leaders:1][nodeID:32]...)...
    ///          [count:4][nodeID:32]...
    /// mode: 0 off, 1 star, 2 hash, 3 rtt. fanout 0 = the smallest that
    /// reaches every node in two hops. leaders of a slot: the nodes that may
    /// lead it and so root a tree.
    SET_TX_SET_TREE = 17,

    // ═══ Overlay → Core (Critical Path) ═══

    /// Received SCP envelope from network
    SCP_RECEIVED = 100,

    /// Response to GET_TOP_TXS request
    /// Payload: [count:4][len1:4][tx1:len1][len2:4][tx2:len2]...
    TOP_TXS_RESPONSE = 101,

    /// Peer requested SCP state
    PEER_REQUESTS_SCP_STATE = 102,

    // ═══ Overlay → Core (Non-Critical) ═══

    /// TX set fetched from peer (response to REQUEST_TX_SET)
    /// Payload: [hash:32][txSetXDR...]
    TX_SET_AVAILABLE = 103,

    /// Here's a quorum set referenced in SCP
    QUORUM_SET_AVAILABLE = 104,

    /// Overlay metrics snapshot (JSON payload)
    /// Response to REQUEST_OVERLAY_METRICS
    OVERLAY_METRICS_RESPONSE = 105,

    /// The overlay's libp2p peer id, sent once at startup so that Core can
    /// sign the identity binding (SET_NODE_IDENTITY).
    /// Payload: [peerId bytes]
    OVERLAY_IDENTITY = 106,
};

/**
 * Represents a single IPC message.
 */
struct IPCMessage
{
    IPCMessageType type;
    std::vector<uint8_t> payload;
};

namespace ipc
{

// Largest payload either side sends or accepts in one message; a reader
// treats a larger length as a corrupt stream and stops reading it. Must equal
// MAX_PAYLOAD_SIZE in overlay/src/ipc/messages.rs. A mempool pull (up to
// 2 x the tx-set capacity plus a pending set's transactions, e.g. 22,700 SAC
// payments of ~750 bytes) must fit, and the overlay truncates its top
// transactions response to this size.
constexpr uint32_t MAX_PAYLOAD_BYTES = 64 * 1024 * 1024;

/**
 * Send an IPC message over a socket.
 *
 * @param socket File descriptor for Unix domain socket
 * @param msg Message to send
 * @return true if message was sent successfully, false on error
 */
bool sendMessage(int socket, IPCMessage const& msg);

/**
 * Receive an IPC message from a socket.
 *
 * @param socket File descriptor for Unix domain socket
 * @return Message if received successfully, nullopt on error or EOF
 */
std::optional<IPCMessage> receiveMessage(int socket);

} // namespace ipc

/**
 * IPCChannel manages a Unix domain socket connection for IPC.
 *
 * This is used by Core to communicate with the Overlay process.
 * The channel is created by connecting to a socket path.
 */
class IPCChannel
{
  public:
    /// Connect to Unix domain socket at the given path
    static std::unique_ptr<IPCChannel> connect(std::string const& socketPath);

    /// Create from existing socket pair (for testing)
    static std::unique_ptr<IPCChannel> fromSocket(int socket);

    ~IPCChannel();

    /// Send a message. Returns true on success.
    bool send(IPCMessage const& msg);

    /// Receive a message. Blocks until message available or connection closed.
    std::optional<IPCMessage> receive();

    /// Shut down the socket. Unblocks any concurrent receive() call.
    void shutdown();

    /// Check if connection is still open
    bool isConnected() const;

    /// Get the underlying socket fd (for use with poll/select)
    int
    getSocket() const
    {
        return mSocket;
    }

  private:
    explicit IPCChannel(int socket);

    int mSocket;
    std::atomic<bool> mConnected;
};

/**
 * IPCServer listens for incoming connections on a Unix domain socket.
 *
 * This is used by the Overlay process to accept connections from Core.
 */
class IPCServer
{
  public:
    /// Create a server listening on the given socket path
    static std::unique_ptr<IPCServer> create(std::string const& socketPath);

    ~IPCServer();

    /// Accept a connection. Blocks until connection available.
    std::unique_ptr<IPCChannel> accept();

    /// Get the socket path
    std::string const&
    getPath() const
    {
        return mSocketPath;
    }

  private:
    explicit IPCServer(int socket, std::string socketPath);

    int mSocket;
    std::string mSocketPath;
};

} // namespace stellar
