// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

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
    bool mConnected;
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
