// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/RustOverlayManager.h"
#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "main/Application.h"
#include "util/Logging.h"
#include "xdr/Stellar-overlay.h"
#include <fmt/format.h>

namespace stellar
{

RustOverlayManager::RustOverlayManager(Application& app)
    : mApp(app), mOverlayMetrics(app)
{
    auto const& cfg = mApp.getConfig();

    std::string socketPath = cfg.OVERLAY_SOCKET_PATH;
    if (socketPath.empty())
    {
        socketPath = fmt::format("/tmp/stellar-overlay-{}-{}.sock", getpid(),
                                 cfg.HTTP_PORT);
    }

    std::string binaryPath = cfg.OVERLAY_BINARY_PATH;
    if (binaryPath.empty())
    {
        binaryPath = "stellar-overlay";
    }

    CLOG_INFO(Overlay,
              "Creating RustOverlayManager with socket={}, binary={}, port={}",
              socketPath, binaryPath, cfg.PEER_PORT);

    mOverlayIPC =
        std::make_unique<OverlayIPC>(socketPath, binaryPath, cfg.PEER_PORT);
}

RustOverlayManager::~RustOverlayManager()
{
    shutdown();
}

void
RustOverlayManager::start()
{
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
        throw std::runtime_error("Failed to start Rust overlay");
    }

    auto const& cfg = mApp.getConfig();
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

} // namespace stellar
