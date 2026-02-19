// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/OverlayIPC.h"
#include "crypto/Hex.h"
#include "util/Logging.h"
#include "util/types.h"
#include "xdr/Stellar-ledger.h"
#include <xdrpp/marshal.h>

#include <chrono>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace stellar
{

OverlayIPC::OverlayIPC(std::string socketPath, std::string overlayBinaryPath,
                       uint16_t peerPort)
    : mSocketPath(std::move(socketPath))
    , mOverlayBinaryPath(std::move(overlayBinaryPath))
    , mPeerPort(peerPort)
{
}

OverlayIPC::~OverlayIPC()
{
    shutdown();
}

bool
OverlayIPC::start()
{
    if (mRunning)
    {
        CLOG_WARNING(Overlay, "OverlayIPC already running");
        return false;
    }

    // Remove old socket file if exists
    unlink(mSocketPath.c_str());

    // Spawn overlay process
    if (!spawnOverlay())
    {
        CLOG_ERROR(Overlay, "Failed to spawn overlay process");
        return false;
    }

    // Retry connection with backoff - overlay may take time to start
    constexpr int MAX_RETRIES = 10;
    constexpr int RETRY_DELAY_MS = 100;

    for (int attempt = 0; attempt < MAX_RETRIES; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_DELAY_MS));

        mChannel = IPCChannel::connect(mSocketPath);
        if (mChannel && mChannel->isConnected())
        {
            CLOG_INFO(Overlay, "Connected to overlay IPC at {} (attempt {})",
                      mSocketPath, attempt + 1);

            // Start reader thread
            mRunning = true;
            mReaderThread = std::thread(&OverlayIPC::readerLoop, this);
            return true;
        }

        CLOG_DEBUG(Overlay, "Connection attempt {} failed, retrying...",
                   attempt + 1);
    }

    CLOG_ERROR(Overlay, "Failed to connect to overlay at {} after {} attempts",
               mSocketPath, MAX_RETRIES);
    shutdown();
    return false;
}

void
OverlayIPC::shutdown()
{
    if (!mRunning)
    {
        return;
    }

    CLOG_INFO(Overlay, "Shutting down overlay IPC");

    mRunning = false;

    // Send shutdown message
    if (mChannel && mChannel->isConnected())
    {
        IPCMessage msg;
        msg.type = IPCMessageType::SHUTDOWN;
        std::lock_guard<std::mutex> lock(mSendMutex);
        mChannel->send(msg);
    }

    // Close channel (will unblock reader)
    mChannel.reset();

    // Wait for reader thread
    if (mReaderThread.joinable())
    {
        mReaderThread.join();
    }

    // Wait for overlay process
    if (mOverlayPid > 0)
    {
        int status;
        // Give it a moment to exit gracefully
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        pid_t result = waitpid(mOverlayPid, &status, WNOHANG);
        if (result == 0)
        {
            // Still running, send SIGTERM
            kill(mOverlayPid, SIGTERM);
            waitpid(mOverlayPid, &status, 0);
        }
        mOverlayPid = -1;
    }
}

bool
OverlayIPC::spawnOverlay()
{
    pid_t pid = fork();
    if (pid < 0)
    {
        CLOG_ERROR(Overlay, "fork() failed: {}", strerror(errno));
        return false;
    }

    if (pid == 0)
    {
        // Child process - exec overlay binary
        // Arguments: <binary> --listen <socket-path> --peer-port <port>
        std::string portStr = std::to_string(mPeerPort);
        execl(mOverlayBinaryPath.c_str(), mOverlayBinaryPath.c_str(),
              "--listen", mSocketPath.c_str(), "--peer-port", portStr.c_str(),
              nullptr);

        // exec failed
        _exit(1);
    }

    // Parent process
    mOverlayPid = pid;
    CLOG_INFO(Overlay, "Spawned overlay process (pid={})", pid);
    return true;
}

void
OverlayIPC::readerLoop()
{
    CLOG_DEBUG(Overlay, "OverlayIPC reader thread started");

    while (mRunning && mChannel && mChannel->isConnected())
    {
        auto msg = mChannel->receive();
        if (!msg)
        {
            if (mRunning)
            {
                CLOG_WARNING(Overlay, "Overlay IPC connection closed");
            }
            break;
        }

        handleMessage(*msg);
    }

    CLOG_DEBUG(Overlay, "OverlayIPC reader thread exiting");
}

void
OverlayIPC::handleMessage(IPCMessage const& msg)
{
    CLOG_INFO(Overlay, "IPC handleMessage: type={}, payload_size={}",
              static_cast<uint32_t>(msg.type), msg.payload.size());
    switch (msg.type)
    {
    case IPCMessageType::SCP_RECEIVED:
    {
        CLOG_DEBUG(Overlay,
                   "Received SCP_RECEIVED IPC message ({} bytes payload)",
                   msg.payload.size());
        if (mOnSCPReceived)
        {
            try
            {
                SCPEnvelope envelope;
                xdr::xdr_from_opaque(msg.payload, envelope);
                CLOG_TRACE(Overlay, "Invoking SCP received callback");
                mOnSCPReceived(envelope);
            }
            catch (std::exception const& e)
            {
                CLOG_WARNING(Overlay, "Failed to parse SCP envelope: {}",
                             e.what());
            }
        }
        else
        {
            CLOG_WARNING(Overlay, "No SCP callback registered!");
        }
        break;
    }

    case IPCMessageType::TOP_TXS_RESPONSE:
    {
        // Response to getTopTransactions - wake up waiting thread
        std::lock_guard<std::mutex> lock(mRequestMutex);
        mPendingResponse = msg;
        mRequestCv.notify_one();
        break;
    }

    case IPCMessageType::TX_SET_AVAILABLE:
    {
        // TX set received from peers (async fetch response)
        // Payload: [hash:32][xdr...]
        if (msg.payload.size() < 32)
        {
            CLOG_WARNING(Overlay, "TX_SET_AVAILABLE payload too short");
            break;
        }

        Hash hash;
        std::memcpy(hash.data(), msg.payload.data(), 32);

        if (mOnTxSetReceived && msg.payload.size() > 32)
        {
            try
            {
                GeneralizedTransactionSet txSet;
                std::vector<uint8_t> xdrData(msg.payload.begin() + 32,
                                             msg.payload.end());
                xdr::xdr_from_opaque(xdrData, txSet);
                CLOG_INFO(Overlay, "Received TX set {} ({} bytes) from overlay",
                          hexAbbrev(hash), xdrData.size());
                mOnTxSetReceived(hash, txSet);
            }
            catch (std::exception const& e)
            {
                CLOG_WARNING(Overlay, "Failed to parse TX set {}: {}",
                             hexAbbrev(hash), e.what());
            }
        }
        else if (!mOnTxSetReceived)
        {
            CLOG_WARNING(Overlay,
                         "TX_SET_AVAILABLE but no callback registered");
        }
        else
        {
            CLOG_WARNING(Overlay, "TX_SET_AVAILABLE payload too short for XDR");
        }
        break;
    }

    case IPCMessageType::PEER_REQUESTS_SCP_STATE:
    {
        // Peer is asking for our SCP state
        // Payload format: [request_id:8][ledger_seq:4]
        if (mOnScpStateRequest && msg.payload.size() >= 12)
        {
            uint64_t requestId;
            uint32_t ledgerSeq;
            std::memcpy(&requestId, msg.payload.data(), 8);
            std::memcpy(&ledgerSeq, msg.payload.data() + 8, 4);
            CLOG_DEBUG(
                Overlay,
                "Peer requesting SCP state for ledger >= {} (request_id={})",
                ledgerSeq, requestId);

            auto envelopes = mOnScpStateRequest(ledgerSeq);
            sendScpStateResponse(requestId, envelopes);
        }
        break;
    }

    default:
        CLOG_DEBUG(Overlay, "Unhandled IPC message type: {}",
                   static_cast<uint32_t>(msg.type));
        break;
    }
}

bool
OverlayIPC::broadcastSCP(SCPEnvelope const& envelope)
{
    if (!mChannel || !mChannel->isConnected())
    {
        CLOG_WARNING(Overlay, "Cannot broadcast SCP: not connected to overlay");
        return false;
    }

    IPCMessage msg;
    msg.type = IPCMessageType::BROADCAST_SCP;
    msg.payload = xdr::xdr_to_opaque(envelope);

    std::lock_guard<std::mutex> lock(mSendMutex);
    return mChannel->send(msg);
}

void
OverlayIPC::notifyLedgerClosed(uint32_t ledgerSeq, Hash const& ledgerHash)
{
    if (!mChannel || !mChannel->isConnected())
    {
        return;
    }

    IPCMessage msg;
    msg.type = IPCMessageType::LEDGER_CLOSED;

    // Payload: [ledgerSeq:4][ledgerHash:32]
    msg.payload.resize(4 + 32);
    std::memcpy(msg.payload.data(), &ledgerSeq, 4);
    std::memcpy(msg.payload.data() + 4, ledgerHash.data(), 32);

    std::lock_guard<std::mutex> lock(mSendMutex);
    mChannel->send(msg);
}

void
OverlayIPC::notifyTxSetExternalized(Hash const& txSetHash,
                                    std::vector<Hash> const& txHashes)
{
    if (!mChannel || !mChannel->isConnected())
    {
        return;
    }

    IPCMessage msg;
    msg.type = IPCMessageType::TX_SET_EXTERNALIZED;

    // Payload: [txSetHash:32][numTxHashes:4][txHash1:32][txHash2:32]...
    size_t payloadSize = 32 + 4 + (txHashes.size() * 32);
    msg.payload.resize(payloadSize);

    // TX set hash
    std::memcpy(msg.payload.data(), txSetHash.data(), 32);

    // Number of TX hashes
    uint32_t numHashes = static_cast<uint32_t>(txHashes.size());
    std::memcpy(msg.payload.data() + 32, &numHashes, 4);

    // TX hashes
    for (size_t i = 0; i < txHashes.size(); ++i)
    {
        std::memcpy(msg.payload.data() + 36 + (i * 32), txHashes[i].data(), 32);
    }

    std::lock_guard<std::mutex> lock(mSendMutex);
    mChannel->send(msg);
}

std::vector<TransactionEnvelope>
OverlayIPC::getTopTransactions(size_t count, int timeoutMs)
{
    std::vector<TransactionEnvelope> result;

    if (!mChannel || !mChannel->isConnected())
    {
        return result;
    }

    // Send request
    IPCMessage req;
    req.type = IPCMessageType::GET_TOP_TXS;
    uint32_t countU32 = static_cast<uint32_t>(count);
    req.payload.resize(4);
    std::memcpy(req.payload.data(), &countU32, 4);

    {
        std::lock_guard<std::mutex> lock(mSendMutex);
        if (!mChannel->send(req))
        {
            return result;
        }
    }

    // Wait for response
    std::unique_lock<std::mutex> lock(mRequestMutex);
    mPendingResponse.reset();

    bool gotResponse =
        mRequestCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [this] { return mPendingResponse.has_value(); });

    if (!gotResponse)
    {
        CLOG_WARNING(Overlay, "Timeout waiting for top transactions");
        return result;
    }

    auto& response = *mPendingResponse;
    if (response.type != IPCMessageType::TOP_TXS_RESPONSE)
    {
        CLOG_WARNING(Overlay,
                     "Unexpected response type for getTopTransactions");
        return result;
    }

    // Parse response: list of XDR-encoded TransactionEnvelopes
    // Format: [count:4][len1:4][tx1:len1][len2:4][tx2:len2]...
    if (response.payload.size() < 4)
    {
        return result;
    }

    uint32_t txCount;
    std::memcpy(&txCount, response.payload.data(), 4);

    size_t offset = 4;
    for (uint32_t i = 0; i < txCount && offset + 4 <= response.payload.size();
         ++i)
    {
        uint32_t txLen;
        std::memcpy(&txLen, response.payload.data() + offset, 4);
        offset += 4;

        if (offset + txLen > response.payload.size())
        {
            break;
        }

        try
        {
            TransactionEnvelope tx;
            std::vector<uint8_t> txData(response.payload.begin() + offset,
                                        response.payload.begin() + offset +
                                            txLen);
            xdr::xdr_from_opaque(txData, tx);
            result.push_back(std::move(tx));
        }
        catch (std::exception const& e)
        {
            CLOG_WARNING(Overlay, "Failed to parse transaction: {}", e.what());
        }

        offset += txLen;
    }

    return result;
}

void
OverlayIPC::submitTransaction(TransactionEnvelope const& tx, int64_t fee,
                              uint32_t numOps)
{
    if (!mChannel || !mChannel->isConnected())
    {
        return;
    }

    IPCMessage msg;
    msg.type = IPCMessageType::SUBMIT_TX;

    auto txData = xdr::xdr_to_opaque(tx);

    // Payload: [fee:8][numOps:4][txData...]
    msg.payload.resize(8 + 4 + txData.size());
    size_t offset = 0;

    std::memcpy(msg.payload.data() + offset, &fee, 8);
    offset += 8;

    std::memcpy(msg.payload.data() + offset, &numOps, 4);
    offset += 4;

    std::memcpy(msg.payload.data() + offset, txData.data(), txData.size());

    std::lock_guard<std::mutex> lock(mSendMutex);
    mChannel->send(msg);
}

void
OverlayIPC::requestTxSet(Hash const& hash)
{
    if (!mChannel || !mChannel->isConnected())
    {
        return;
    }

    IPCMessage msg;
    msg.type = IPCMessageType::REQUEST_TX_SET;
    msg.payload.resize(32);
    std::memcpy(msg.payload.data(), hash.data(), 32);

    CLOG_DEBUG(Overlay, "Requesting TX set {}", hexAbbrev(hash));
    std::lock_guard<std::mutex> lock(mSendMutex);
    mChannel->send(msg);
}

void
OverlayIPC::cacheTxSet(Hash const& hash, std::vector<uint8_t> const& xdr)
{
    if (!mChannel || !mChannel->isConnected())
    {
        return;
    }

    IPCMessage msg;
    msg.type = IPCMessageType::CACHE_TX_SET;
    msg.payload.resize(32 + xdr.size());
    std::memcpy(msg.payload.data(), hash.data(), 32);
    std::memcpy(msg.payload.data() + 32, xdr.data(), xdr.size());

    CLOG_DEBUG(Overlay, "Caching TX set {} ({} bytes)", hexAbbrev(hash),
               xdr.size());
    std::lock_guard<std::mutex> lock(mSendMutex);
    mChannel->send(msg);
}

void
OverlayIPC::setPeerConfig(std::vector<std::string> const& knownPeers,
                          std::vector<std::string> const& preferredPeers,
                          uint16_t listenPort)
{
    if (!mChannel || !mChannel->isConnected())
    {
        return;
    }

    // Build JSON payload
    std::string json = "{\"known_peers\":[";
    for (size_t i = 0; i < knownPeers.size(); ++i)
    {
        if (i > 0)
            json += ",";
        json += "\"" + knownPeers[i] + "\"";
    }
    json += "],\"preferred_peers\":[";
    for (size_t i = 0; i < preferredPeers.size(); ++i)
    {
        if (i > 0)
            json += ",";
        json += "\"" + preferredPeers[i] + "\"";
    }
    json += "],\"listen_port\":" + std::to_string(listenPort) + "}";

    IPCMessage msg;
    msg.type = IPCMessageType::SET_PEER_CONFIG;
    msg.payload.assign(json.begin(), json.end());

    CLOG_DEBUG(Overlay, "Sending peer config: {}", json);
    std::lock_guard<std::mutex> lock(mSendMutex);
    mChannel->send(msg);
}

void
OverlayIPC::requestScpState(uint32_t ledgerSeq)
{
    if (!mChannel || !mChannel->isConnected())
    {
        return;
    }

    IPCMessage msg;
    msg.type = IPCMessageType::REQUEST_SCP_STATE;
    msg.payload.resize(4);
    std::memcpy(msg.payload.data(), &ledgerSeq, 4);

    CLOG_DEBUG(Overlay, "Requesting SCP state from peers, ledger >= {}",
               ledgerSeq);
    std::lock_guard<std::mutex> lock(mSendMutex);
    mChannel->send(msg);
}

void
OverlayIPC::setOnSCPReceived(SCPReceivedCallback cb)
{
    mOnSCPReceived = std::move(cb);
}

void
OverlayIPC::setOnScpStateRequest(ScpStateRequestCallback cb)
{
    mOnScpStateRequest = std::move(cb);
}

void
OverlayIPC::setOnTxSetReceived(TxSetReceivedCallback cb)
{
    mOnTxSetReceived = std::move(cb);
}

void
OverlayIPC::sendScpStateResponse(uint64_t requestId,
                                 std::vector<SCPEnvelope> const& envelopes)
{
    if (!mChannel || !mChannel->isConnected())
    {
        return;
    }

    // Serialize all envelopes into payload
    // Format: [request_id:u64][count:u32][envelope1_len:u32][envelope1_xdr]...
    std::vector<uint8_t> payload;
    uint32_t count = static_cast<uint32_t>(envelopes.size());
    payload.resize(12); // 8 bytes request_id + 4 bytes count
    std::memcpy(payload.data(), &requestId, 8);
    std::memcpy(payload.data() + 8, &count, 4);

    for (auto const& env : envelopes)
    {
        auto xdr = xdr::xdr_to_opaque(env);
        uint32_t len = static_cast<uint32_t>(xdr.size());
        size_t offset = payload.size();
        payload.resize(offset + 4 + len);
        std::memcpy(payload.data() + offset, &len, 4);
        std::memcpy(payload.data() + offset + 4, xdr.data(), len);
    }

    IPCMessage msg;
    msg.type = IPCMessageType::SCP_STATE_RESPONSE;
    msg.payload = std::move(payload);

    CLOG_DEBUG(Overlay,
               "Sending SCP state response with {} envelopes (request_id={})",
               count, requestId);
    std::lock_guard<std::mutex> lock(mSendMutex);
    mChannel->send(msg);
}

bool
OverlayIPC::isConnected() const
{
    return mChannel && mChannel->isConnected();
}

} // namespace stellar
