// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/OverlayIPC.h"
#include "crypto/Hex.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "xdr/Stellar-ledger.h"
#include <fmt/format.h>
#include <xdrpp/marshal.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <signal.h>
#include <sstream>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace stellar
{

namespace
{

constexpr char OVERLAY_BINARY_ENV_VAR[] = "STELLAR_OVERLAY_BINARY";

bool
isExecutable(std::string const& path)
{
    return access(path.c_str(), X_OK) == 0;
}

std::optional<std::string>
absoluteIfExecutable(std::string const& path)
{
    if (isExecutable(path))
    {
        return std::filesystem::absolute(path).string();
    }
    return std::nullopt;
}

} // namespace

OverlayIPC::OverlayIPC(std::optional<std::string> socketPath,
                       std::optional<std::string> overlayBinaryPath,
                       uint16_t peerPort, std::optional<std::string> txSetDir)
    : mSocketPath(socketPath && !socketPath->empty()
                      ? std::move(*socketPath)
                      : defaultSocketPath(peerPort))
    , mOverlayBinaryPath(std::move(overlayBinaryPath))
    , mTxSetDir(std::move(txSetDir))
    , mPeerPort(peerPort)
{
    if (mTxSetDir && mTxSetDir->empty())
    {
        mTxSetDir.reset();
    }
}

std::string
OverlayIPC::defaultSocketPath(uint16_t peerPort)
{
    return fmt::format("/tmp/stellar-overlay-{}-{}.sock", getpid(), peerPort);
}

std::optional<std::string>
OverlayIPC::findOverlayBinaryPath()
{
    if (auto const binaryEnv = std::getenv(OVERLAY_BINARY_ENV_VAR))
    {
        if (binaryEnv[0] != '\0')
        {
            return std::filesystem::absolute(binaryEnv).string();
        }
    }

    std::vector<std::string> paths = {
        "stellar-overlay",
        "../stellar-overlay",
        "target/release/stellar-overlay",
        "../target/release/stellar-overlay",
        "target/debug/stellar-overlay",
        "../target/debug/stellar-overlay",
    };

    for (auto const& path : paths)
    {
        auto resolved = absoluteIfExecutable(path);
        if (resolved)
        {
            return resolved;
        }
    }

    if (auto const pathEnv = std::getenv("PATH"))
    {
        std::stringstream pathStream(pathEnv);
        std::string directory;
        while (std::getline(pathStream, directory, ':'))
        {
            if (directory.empty())
            {
                directory = ".";
            }
            auto candidate =
                (std::filesystem::path(directory) / "stellar-overlay").string();
            auto resolved = absoluteIfExecutable(candidate);
            if (resolved)
            {
                return resolved;
            }
        }
    }

    return std::nullopt;
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
            mReaderAlive = true;
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

    // Wake any getTopTransactions waiter under the lock so the notification
    // can't race its predicate check.
    {
        std::lock_guard<std::mutex> lock(mRequestMutex);
        mRequestCv.notify_all();
    }

    // Send shutdown message
    if (mChannel && mChannel->isConnected())
    {
        IPCMessage msg;
        msg.type = IPCMessageType::SHUTDOWN;
        std::lock_guard<std::mutex> lock(mSendMutex);
        mChannel->send(msg);
    }

    // Shut down the socket to unblock the reader thread before destroying the
    // channel object it is using.
    if (mChannel)
    {
        mChannel->shutdown();
    }

    // Wait for reader thread
    if (mReaderThread.joinable())
    {
        mReaderThread.join();
    }

    mChannel.reset();

    // Wait for overlay process
    if (mOverlayPid > 0)
    {
        int status;
        // Give it a moment to exit gracefully
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        pid_t result = waitpid(mOverlayPid, &status, WNOHANG);
        if (result == 0)
        {
            // Still running, send SIGTERM and give it a bounded amount of time
            // to exit before falling back to SIGKILL, so that a wedged overlay
            // process cannot hang core's shutdown.
            kill(mOverlayPid, SIGTERM);
            auto const deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while ((result = waitpid(mOverlayPid, &status, WNOHANG)) == 0 &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (result == 0)
            {
                CLOG_WARNING(Overlay,
                             "Overlay process {} did not exit after SIGTERM, "
                             "sending SIGKILL",
                             mOverlayPid);
                kill(mOverlayPid, SIGKILL);
                waitpid(mOverlayPid, &status, 0);
            }
        }
        mOverlayPid = -1;
    }
}

bool
OverlayIPC::spawnOverlay()
{
    auto overlayBinaryPath = resolveOverlayBinaryPath();
    if (!overlayBinaryPath)
    {
        CLOG_ERROR(Overlay, "Unable to locate stellar-overlay binary");
        return false;
    }

    long const maxFd = sysconf(_SC_OPEN_MAX);
    // Arguments: <binary> --listen <socket-path> --peer-port <port>
    //            [--txset-dir <dir>]
    // Built before fork(): only async-signal-safe calls are allowed after it.
    std::string const portStr = std::to_string(mPeerPort);
    std::vector<char const*> args{overlayBinaryPath->c_str(), "--listen",
                                  mSocketPath.c_str(), "--peer-port",
                                  portStr.c_str()};
    if (mTxSetDir)
    {
        args.push_back("--txset-dir");
        args.push_back(mTxSetDir->c_str());
    }
    args.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0)
    {
        CLOG_ERROR(Overlay, "fork() failed: {}", strerror(errno));
        return false;
    }

    if (pid == 0)
    {
        // Child process. Close every descriptor inherited from core except
        // stdin/stdout/stderr: otherwise the overlay keeps core's listening
        // sockets bound after core exits and holds the read ends of sibling
        // overlays' IPC sockets, which can leave a shutting-down sibling
        // blocked forever on a write nobody will read. Only async-signal-safe
        // calls are allowed between fork() and exec().
        bool closed = false;
#if defined(__linux__) && defined(SYS_close_range)
        closed = syscall(SYS_close_range, 3, ~0U, 0) == 0;
#endif
        if (!closed)
        {
            for (long fd = 3; fd < maxFd; ++fd)
            {
                close(static_cast<int>(fd));
            }
        }

        // Exec overlay binary
        execv(overlayBinaryPath->c_str(),
              const_cast<char* const*>(args.data()));

        // exec failed
        _exit(1);
    }

    // Parent process
    mOverlayPid = pid;
    CLOG_INFO(Overlay, "Spawned overlay process (pid={})", pid);
    return true;
}

std::optional<std::string>
OverlayIPC::resolveOverlayBinaryPath() const
{
    if (mOverlayBinaryPath && !mOverlayBinaryPath->empty())
    {
        return mOverlayBinaryPath;
    }
    return findOverlayBinaryPath();
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

    // No more responses can arrive; wake any getTopTransactions waiter.
    // The flag is flipped under mRequestMutex so a waiter can't check it
    // and then sleep past the notification.
    {
        std::lock_guard<std::mutex> lock(mRequestMutex);
        mReaderAlive = false;
    }
    mRequestCv.notify_all();
}

void
OverlayIPC::handleMessage(IPCMessage const& msg)
{
    CLOG_DEBUG(Overlay, "IPC handleMessage: type={}, payload_size={}",
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

    case IPCMessageType::OVERLAY_IDENTITY:
    {
        if (mOnOverlayIdentity && !msg.payload.empty() &&
            msg.payload.size() <= 128)
        {
            mOnOverlayIdentity(msg.payload);
        }
        else
        {
            CLOG_WARNING(Overlay,
                         "Ignoring OVERLAY_IDENTITY ({} bytes payload)",
                         msg.payload.size());
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

    case IPCMessageType::OVERLAY_METRICS_RESPONSE:
    {
        // Response to requestMetrics - wake up waiting thread
        std::lock_guard<std::mutex> lock(mMetricsMutex);
        mPendingMetricsResponse = msg;
        mMetricsCv.notify_one();
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
                // Decode in place after the hash (offset 32 keeps the 4-byte
                // alignment xdrpp requires) and hand over the decoded set.
                GeneralizedTransactionSet txSet;
                xdr::xdr_get get(msg.payload.data() + 32,
                                 msg.payload.data() + msg.payload.size());
                xdr::xdr_argpack_archive(get, txSet);
                get.done();
                CLOG_INFO(Overlay, "Received TX set {} ({} bytes) from overlay",
                          hexAbbrev(hash), msg.payload.size() - 32);
                mOnTxSetReceived(hash, std::move(txSet));
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

void
OverlayIPC::removeTransactions(std::vector<Hash> const& txHashes)
{
    if (txHashes.empty())
    {
        return;
    }
    notifyTxSetExternalized(Hash{}, txHashes);
}

std::vector<TransactionEnvelope>
OverlayIPC::getTopTransactions(size_t count)
{
    return parseTopTransactions(getTopTransactionsPayload(count));
}

std::vector<uint8_t>
OverlayIPC::getTopTransactionsPayload(size_t count)
{
    std::vector<uint8_t> result;

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
        std::lock_guard<std::mutex> lock(mRequestMutex);
        mPendingResponse.reset();
    }

    {
        std::lock_guard<std::mutex> sendLock(mSendMutex);
        if (!mChannel->send(req))
        {
            return result;
        }
    }

    // Wait for response; shutdown and connection loss notify the cv to
    // unblock us. The wait is bounded so a lost response degrades to an
    // empty candidate list instead of wedging the main thread.
    std::unique_lock<std::mutex> lock(mRequestMutex);
    bool gotResponse =
        mRequestCv.wait_for(lock, std::chrono::seconds(5), [this] {
            return mPendingResponse.has_value() || !mRunning || !mReaderAlive;
        });

    if (!gotResponse || !mPendingResponse.has_value())
    {
        CLOG_WARNING(Overlay,
                     "Overlay IPC {} while waiting for top transactions",
                     !gotResponse ? "timed out"
                                  : (mRunning ? "disconnected" : "shut down"));
        return result;
    }

    auto& response = *mPendingResponse;
    if (response.type != IPCMessageType::TOP_TXS_RESPONSE)
    {
        CLOG_WARNING(Overlay,
                     "Unexpected response type for getTopTransactions");
        return result;
    }

    return std::move(response.payload);
}

std::vector<TransactionEnvelope>
OverlayIPC::parseTopTransactions(std::vector<uint8_t> const& payload)
{
    std::vector<TransactionEnvelope> result;
    // Format: [count:4][len1:4][tx1:len1][len2:4][tx2:len2]...
    if (payload.size() < 4)
    {
        return result;
    }

    uint32_t txCount;
    std::memcpy(&txCount, payload.data(), 4);
    result.reserve(std::min<size_t>(txCount, payload.size() / 4));

    size_t offset = 4;
    for (uint32_t i = 0; i < txCount && offset + 4 <= payload.size(); ++i)
    {
        uint32_t txLen;
        std::memcpy(&txLen, payload.data() + offset, 4);
        offset += 4;

        if (txLen > payload.size() - offset)
        {
            break;
        }

        try
        {
            TransactionEnvelope tx;
            uint8_t const* const start = payload.data() + offset;
            // XDR is read in 4-byte words from 4-byte aligned memory. Well
            // formed entries are (XDR lengths are multiples of 4), so decode
            // them in place; copy anything else to aligned memory first.
            if ((reinterpret_cast<uintptr_t>(start) & 3) == 0)
            {
                xdr::xdr_get g(start, start + txLen);
                xdr::xdr_argpack_archive(g, tx);
                g.done();
            }
            else
            {
                std::vector<uint8_t> const txData(start, start + txLen);
                xdr::xdr_from_opaque(txData, tx);
            }
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
OverlayIPC::requestTxSet(Hash const& hash, uint32_t slotIndex)
{
    if (!mChannel || !mChannel->isConnected())
    {
        return;
    }

    IPCMessage msg;
    msg.type = IPCMessageType::REQUEST_TX_SET;
    // Payload: [hash:32][slotSeq:4]. The slot the set is for; used by the
    // Rust overlay to stamp the cache entry for exact age-based eviction.
    msg.payload.resize(36);
    std::memcpy(msg.payload.data(), hash.data(), 32);
    std::memcpy(msg.payload.data() + 32, &slotIndex, 4);

    CLOG_DEBUG(Overlay, "Requesting TX set {} for slot {}", hexAbbrev(hash),
               slotIndex);
    std::lock_guard<std::mutex> lock(mSendMutex);
    mChannel->send(msg);
}

bool
OverlayIPC::cacheTxSet(Hash const& hash, GeneralizedTransactionSet const& txSet,
                       uint32_t slotIndex)
{
    if (!mChannel || !mChannel->isConnected())
    {
        return false;
    }

    IPCMessage msg;
    msg.type = IPCMessageType::CACHE_TX_SET;
    // Payload: [hash:32][slotSeq:4][txSetXDR...]. The slot the set is for;
    // used by the Rust overlay to stamp the cache entry for exact age-based
    // eviction. The set is serialized in place (offset 36 keeps the 4-byte
    // alignment xdrpp requires).
    auto const xdrSize = xdr::xdr_argpack_size(txSet);
    msg.payload.resize(36 + xdrSize);
    std::memcpy(msg.payload.data(), hash.data(), 32);
    std::memcpy(msg.payload.data() + 32, &slotIndex, 4);
    xdr::xdr_put put(msg.payload.data() + 36,
                     msg.payload.data() + msg.payload.size());
    xdr::xdr_argpack_archive(put, txSet);
    releaseAssert(put.p_ == put.e_);

    CLOG_DEBUG(Overlay, "Caching TX set {} for slot {} ({} bytes)",
               hexAbbrev(hash), slotIndex, xdrSize);
    std::lock_guard<std::mutex> lock(mSendMutex);
    return mChannel->send(msg);
}

void
OverlayIPC::pushTxSet(Hash const& hash, uint32_t slotIndex)
{
    if (!mChannel || !mChannel->isConnected())
    {
        return;
    }

    IPCMessage msg;
    msg.type = IPCMessageType::PUSH_TX_SET;
    // Payload: [hash:32][slotSeq:4]
    msg.payload.resize(36);
    std::memcpy(msg.payload.data(), hash.data(), 32);
    std::memcpy(msg.payload.data() + 32, &slotIndex, 4);

    CLOG_DEBUG(Overlay, "Pushing TX set {} for slot {} to peers",
               hexAbbrev(hash), slotIndex);
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
OverlayIPC::setNodeIdentity(Hash const& networkID, NodeID const& nodeID,
                            Signature const& signature)
{
    if (!mChannel || !mChannel->isConnected())
    {
        return;
    }
    releaseAssert(signature.size() == 64);
    IPCMessage msg;
    msg.type = IPCMessageType::SET_NODE_IDENTITY;
    msg.payload.resize(128);
    std::memcpy(msg.payload.data(), networkID.data(), 32);
    std::memcpy(msg.payload.data() + 32, nodeID.ed25519().data(), 32);
    std::memcpy(msg.payload.data() + 64, signature.data(), 64);
    std::lock_guard<std::mutex> lock(mSendMutex);
    mChannel->send(msg);
}

bool
OverlayIPC::setTxTargets(std::optional<std::vector<NodeID>> const& targets)
{
    if (!mChannel || !mChannel->isConnected())
    {
        return false;
    }
    IPCMessage msg;
    msg.type = IPCMessageType::SET_TX_TARGETS;
    uint32_t const count =
        targets ? static_cast<uint32_t>(targets->size()) : 0;
    msg.payload.resize(5 + static_cast<size_t>(count) * 32);
    msg.payload[0] = targets ? 0 : 1;
    std::memcpy(msg.payload.data() + 1, &count, 4);
    for (uint32_t i = 0; i < count; ++i)
    {
        std::memcpy(msg.payload.data() + 5 + static_cast<size_t>(i) * 32,
                    (*targets)[i].ed25519().data(), 32);
    }
    std::lock_guard<std::mutex> lock(mSendMutex);
    return mChannel->send(msg);
}

bool
OverlayIPC::setTxSetTree(uint8_t mode, uint8_t fanout,
                         std::vector<std::pair<uint32_t, Hash>> const& slots,
                         std::vector<NodeID> const& nodes)
{
    if (!mChannel || !mChannel->isConnected())
    {
        return false;
    }
    releaseAssert(slots.size() <= UINT8_MAX);
    IPCMessage msg;
    msg.type = IPCMessageType::SET_TX_SET_TREE;
    uint32_t const count = static_cast<uint32_t>(nodes.size());
    msg.payload.resize(3 + slots.size() * 36 + 4 +
                       static_cast<size_t>(count) * 32);
    auto* out = msg.payload.data();
    *out++ = mode;
    *out++ = fanout;
    *out++ = static_cast<uint8_t>(slots.size());
    for (auto const& [slot, seed] : slots)
    {
        std::memcpy(out, &slot, 4);
        std::memcpy(out + 4, seed.data(), 32);
        out += 36;
    }
    std::memcpy(out, &count, 4);
    out += 4;
    for (auto const& node : nodes)
    {
        std::memcpy(out, node.ed25519().data(), 32);
        out += 32;
    }
    std::lock_guard<std::mutex> lock(mSendMutex);
    return mChannel->send(msg);
}

void
OverlayIPC::setOnOverlayIdentity(OverlayIdentityCallback cb)
{
    mOnOverlayIdentity = std::move(cb);
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

std::string
OverlayIPC::requestMetrics(int timeoutMs)
{
    if (!mChannel || !mChannel->isConnected())
    {
        return {};
    }

    // Send empty request
    IPCMessage req;
    req.type = IPCMessageType::REQUEST_OVERLAY_METRICS;

    // Clear the response slot before sending (same reset-after-send race as
    // getTopTransactions; here it only manifested as spurious timeouts).
    {
        std::lock_guard<std::mutex> lock(mMetricsMutex);
        mPendingMetricsResponse.reset();
    }

    {
        std::lock_guard<std::mutex> sendLock(mSendMutex);
        if (!mChannel->send(req))
        {
            return {};
        }
    }

    std::unique_lock<std::mutex> lock(mMetricsMutex);
    bool gotResponse =
        mMetricsCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
            return mPendingMetricsResponse.has_value();
        });

    if (!gotResponse)
    {
        CLOG_WARNING(Overlay, "Timeout waiting for overlay metrics");
        return {};
    }

    auto& response = *mPendingMetricsResponse;
    if (response.type != IPCMessageType::OVERLAY_METRICS_RESPONSE)
    {
        CLOG_WARNING(Overlay, "Unexpected response type for requestMetrics");
        return {};
    }

    return std::string(response.payload.begin(), response.payload.end());
}

bool
OverlayIPC::isConnected() const
{
    return mChannel && mChannel->isConnected();
}

} // namespace stellar
