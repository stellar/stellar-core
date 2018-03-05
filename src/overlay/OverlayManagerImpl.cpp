// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/OverlayManagerImpl.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "item/ItemFetcher.h"
#include "item/ItemKey.h"
#include "main/Application.h"
#include "overlay/OverlayMessageHandler.h"
#include "transport/BanManager.h"
#include "transport/PreferredPeers.h"
#include "transport/Transport.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/make_unique.h"

#include <xdrpp/marshal.h>

/*

Connection process:
A wants to connect to B
A initiates a tcp connection to B
connection is established
A sends HELLO(CertA,NonceA) to B
B now has IP and listening port of A, sends HELLO(CertB,NonceB) back
A sends AUTH(signed([0],keyAB))
B verifies and either:
    sends AUTH(signed([0],keyBA)) back or
    disconnects, if it's full, optionally sending a list of other peers to try
first

keyAB and keyBA are per-connection HMAC keys derived from non-interactive
ECDH on random curve25519 keys conveyed in CertA and CertB (certs signed by
Node Ed25519 keys) the result of which is then fed through HKDF with the
per-connection nonces. See PeerAuth.h.

If any verify step fails, the peer disconnects immediately.

*/

namespace stellar
{

using namespace soci;
using namespace std;

std::unique_ptr<OverlayManager>
OverlayManager::create(Application& app)
{
    return make_unique<OverlayManagerImpl>(app);
}

OverlayManagerImpl::OverlayManagerImpl(Application& app)
    : mApp(app)
    , mTCPAcceptor(
          mApp,
          [&](Peer::pointer peer) { mApp.getTransport().addPendingPeer(peer); })
    , mShuttingDown(false)
    , mMessagesReceived(app.getMetrics().NewMeter(
          {"overlay", "message", "flood-receive"}, "message"))
    , mMessagesBroadcast(app.getMetrics().NewMeter(
          {"overlay", "message", "broadcast"}, "message"))

    , mTimer(app)
    , mFloodGate(app)
{
}

OverlayManagerImpl::~OverlayManagerImpl()
{
}

void
OverlayManagerImpl::start()
{
    mTimer.expires_from_now(std::chrono::seconds(2));

    if (!mApp.getConfig().RUN_STANDALONE)
    {
        mTimer.async_wait(
            [this]() {
                mApp.getPreferredPeers().storeConfigPeers();
                tick();
            },
            VirtualTimer::onFailureNoop);
        mTCPAcceptor.start();
    }
}

std::vector<PeerRecord>
OverlayManagerImpl::getPreferredPeersFromConfig()
{
    std::vector<PeerRecord> peers;
    for (auto& pp : mApp.getPreferredPeers().getAll())
    {
        auto address = PeerBareAddress::resolve(pp, mApp);
        if (!mApp.getTransport().getConnectedPeer(address))
        {
            auto pr = PeerRecord::loadPeerRecord(mApp.getDatabase(), address);
            if (pr && pr->mNextAttempt <= mApp.getClock().now())
            {
                peers.emplace_back(*pr);
            }
        }
    }
    return peers;
}

std::vector<PeerRecord>
OverlayManagerImpl::getPeersToConnectTo(int maxNum)
{
    // don't connect to too many peers at once
    maxNum = std::min(maxNum, 50);

    // batch is how many peers to load from the database every time
    const int batchSize = std::max(50, maxNum);

    std::vector<PeerRecord> peers;

    PeerRecord::loadPeerRecords(
        mApp.getDatabase(), batchSize, mApp.getClock().now(),
        [&](PeerRecord const& pr) {
            // skip peers that we're already
            // connected/connecting to
            if (!mApp.getTransport().getConnectedPeer(pr.getAddress()))
            {
                peers.emplace_back(pr);
            }
            return peers.size() < static_cast<size_t>(maxNum);
        });
    return peers;
}

void
OverlayManagerImpl::connectToMorePeers(std::vector<PeerRecord>& peers)
{
    mApp.getPreferredPeers().orderByPreferredPeers(peers);

    for (auto& pr : peers)
    {
        if (pr.mNextAttempt > mApp.getClock().now())
        {
            continue;
        }
        // we always try to connect to preferred peers
        if (!pr.isPreferred() &&
            mApp.getTransport().getAuthenticatedPeersCount() >=
                mApp.getConfig().TARGET_PEER_CONNECTIONS)
        {
            break;
        }
        mApp.getTransport().connectTo(pr);
    }
}

// called every 2 seconds
void
OverlayManagerImpl::tick()
{
    CLOG(TRACE, "Overlay") << "OverlayManagerImpl tick";

    mLoad.maybeShedExcessLoad(mApp);

    // first, see if we should trigger connections to preferred peers
    auto peers = getPreferredPeersFromConfig();
    connectToMorePeers(peers);

    if (mApp.getTransport().getAuthenticatedPeersCount() <
        mApp.getConfig().TARGET_PEER_CONNECTIONS)
    {
        // load best candidates from the database,
        // when PREFERRED_PEER_ONLY is set and we connect to a non
        // preferred_peer we just end up dropping & backing off
        // it during handshake (this allows for preferred_peers
        // to work for both ip based and key based preferred mode)
        peers = getPeersToConnectTo(
            static_cast<int>(mApp.getConfig().TARGET_PEER_CONNECTIONS -
                             mApp.getTransport().getAuthenticatedPeersCount()));
        connectToMorePeers(peers);
    }

    mTimer.expires_from_now(
        std::chrono::seconds(mApp.getConfig().PEER_AUTHENTICATION_TIMEOUT + 1));
    mTimer.async_wait([this]() { this->tick(); }, VirtualTimer::onFailureNoop);
}

void
OverlayManagerImpl::ledgerClosed(uint32_t lastClosedledgerSeq)
{
    mFloodGate.clearBelow(lastClosedledgerSeq);
}

QSetCache&
OverlayManagerImpl::getQSetCache()
{
    return mQSetCache;
}

TxSetCache&
OverlayManagerImpl::getTxSetCache()
{
    return mTxSetCache;
}

void
OverlayManagerImpl::recvFloodedMsg(StellarMessage const& msg,
                                   uint32_t ledgerSeq, Peer::pointer peer)
{
    if (!peer)
    {
        return;
    }

    mMessagesReceived.Mark();
    mFloodGate.addRecord(msg, ledgerSeq, peer);
}

void
OverlayManagerImpl::transactionProcessed(
    Peer::pointer peer, uint32_t ledgerSeq,
    TransactionEnvelope const& transaction,
    TransactionHandler::TransactionStatus status)
{
    if (status != TransactionHandler::TX_STATUS_PENDING &&
        status != TransactionHandler::TX_STATUS_DUPLICATE)
    {
        return;
    }

    StellarMessage msg;
    msg.type(TRANSACTION);
    msg.transaction() = transaction;

    recvFloodedMsg(msg, ledgerSeq, peer);

    if (status == TransactionHandler::TX_STATUS_PENDING)
    {
        // if it's a new transaction, broadcast it
        broadcastMessage(msg, ledgerSeq);
    }
}

void
OverlayManagerImpl::scpEnvelopeProcessed(Peer::pointer peer, uint32_t ledgerSeq,
                                         SCPEnvelope const& envelope,
                                         EnvelopeHandler::EnvelopeStatus status)
{
    StellarMessage msg;
    msg.type(SCP_MESSAGE);
    msg.envelope() = envelope;

    recvFloodedMsg(msg, ledgerSeq, peer);

    if (status == EnvelopeHandler::ENVELOPE_STATUS_READY)
    {
        broadcastMessage(msg, ledgerSeq);
    }
}

void
OverlayManagerImpl::broadcastMessage(StellarMessage const& msg,
                                     uint32_t ledgerSeq, bool force)
{
    mMessagesBroadcast.Mark();
    mFloodGate.broadcast(msg, ledgerSeq, force);
}

void
OverlayManager::dropAll(Database& db)
{
    PeerRecord::dropAll(db);
}

LoadManager&
OverlayManagerImpl::getLoadManager()
{
    return mLoad;
}

void
OverlayManagerImpl::shutdown()
{
    if (mShuttingDown)
    {
        return;
    }
    mShuttingDown = true;
    mTCPAcceptor.close();
    mFloodGate.shutdown();
    mApp.getTransport().shutdown();
}

bool
OverlayManagerImpl::isShuttingDown() const
{
    return mShuttingDown;
}
}
