// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/OverlayMessageHandler.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "item/ItemFetcher.h"
#include "main/Application.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/PendingEnvelopes.h"
#include "transport/BanManager.h"
#include "transport/ConnectionHandler.h"
#include "transport/PeerAuth.h"
#include "transport/Transport.h"
#include "util/Logging.h"

#include <medida/meter.h>
#include <medida/metrics_registry.h>
#include <medida/timer.h>
#include <xdrpp/marshal.h>

namespace stellar
{

using xdr::operator<;

OverlayMessageHandler::OverlayMessageHandler(Application& app)
    : mApp{app}

    , mDropInRecvHelloSelfMeter{app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-hello-self"}, "drop")}
    , mDropInRecvHelloPeerIDMeter{app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-hello-peerid"}, "drop")}
    , mDropInRecvHelloCertMeter{app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-hello-cert"}, "drop")}
    , mDropInRecvHelloBanMeter{app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-hello-ban"}, "drop")}
    , mDropInRecvHelloNetMeter{app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-hello-net"}, "drop")}
{
}

bool OverlayMessageHandler::shouldAbort(Peer::const_pointer)
{
    return mApp.getOverlayManager().isShuttingDown();
}

std::pair<bool, std::string>
OverlayMessageHandler::acceptHello(Peer::pointer peer, Hello const& elo)
{
    auto& peerAuth = mApp.getPeerAuth();
    if (!peerAuth.verifyRemoteAuthCert(elo.peerID, elo.cert))
    {
        CLOG(ERROR, "Overlay") << "failed to verify remote peer auth cert";
        mDropInRecvHelloCertMeter.Mark();
        return std::make_pair(false, std::string{});
    }

    if (mApp.getBanManager().isBanned(elo.peerID))
    {
        CLOG(ERROR, "Overlay") << "Node is banned";
        mDropInRecvHelloBanMeter.Mark();
        return std::make_pair(false, std::string{});
    }

    if (elo.peerID == mApp.getConfig().NODE_SEED.getPublicKey())
    {
        CLOG(WARNING, "Overlay") << "connecting to self";
        mDropInRecvHelloSelfMeter.Mark();
        return std::make_pair(false, std::string{"connecting to self"});
    }

    if (elo.networkID != mApp.getNetworkID())
    {
        CLOG(WARNING, "Overlay")
            << "connection from peer with different NetworkID";
        CLOG(DEBUG, "Overlay")
            << "NetworkID = " << hexAbbrev(elo.networkID)
            << " expected: " << hexAbbrev(mApp.getNetworkID());
        mDropInRecvHelloNetMeter.Mark();
        return std::make_pair(false, std::string{"wrong network passphrase"});
    }

    auto const& authenticated = mApp.getTransport().getAuthenticatedPeers();
    auto authenticatedIt = authenticated.find(elo.peerID);
    // no need to self-check here as this one cannot be in authenticated yet
    if (authenticatedIt != std::end(authenticated))
    {
        if (&(authenticatedIt->second->getPeerID()) != &elo.peerID)
        {
            CLOG(WARNING, "Overlay")
                << "connection from already-connected peerID "
                << mApp.getConfig().toShortString(elo.peerID);
            mDropInRecvHelloPeerIDMeter.Mark();
            return std::make_pair(
                false, std::string{"connecting already-connected peer"});
        }
    }

    for (auto const& p : mApp.getTransport().getPendingPeers())
    {
        if (&(p->getPeerID()) == &elo.peerID)
        {
            continue;
        }
        if (p->getPeerID() == elo.peerID)
        {
            CLOG(WARNING, "Overlay")
                << "connection from already-connected peerID "
                << mApp.getConfig().toShortString(elo.peerID);
            mDropInRecvHelloPeerIDMeter.Mark();
            return std::make_pair(
                false, std::string{"connecting already-connected peer"});
        }
    }

    return std::make_pair(true, std::string{});
}

bool
OverlayMessageHandler::acceptAuthenticated(Peer::pointer peer)
{
    if (!mApp.getTransport().acceptAuthenticatedPeer(peer))
    {
        return false;
    }

    mApp.getConnectionHandler().peerAuthenticated(peer);
    return true;
}

void
OverlayMessageHandler::getSCPState(Peer::pointer peer, uint32_t seq)
{
    CLOG(TRACE, "Overlay") << "get SCP State " << seq;
    auto const& envelopes = mApp.getEnvelopeHandler().getSCPState(seq);
    CLOG(DEBUG, "Overlay") << "Send state " << envelopes.size()
                           << " from ledger " << seq;

    for (auto const& e : envelopes)
    {
        StellarMessage m;
        m.type(SCP_MESSAGE);
        m.envelope() = e;
        peer->sendMessage(m);
    }
}

void
OverlayMessageHandler::getPeers(Peer::pointer peer)
{
    auto msg = StellarMessage{};
    msg.type(PEERS);
    uint32 maxPeerCount = std::min<uint32>(50, msg.peers().max_size());

    // send top peers we know about
    std::vector<PeerRecord> peers;
    PeerRecord::loadPeerRecords(mApp.getDatabase(), 50, mApp.getClock().now(),
                                [&](PeerRecord const& pr) {
                                    bool r = peers.size() < maxPeerCount;
                                    if (r)
                                    {
                                        if (!pr.getAddress().isPrivate() &&
                                            pr.getAddress() !=
                                                peer->getAddress())
                                        {
                                            peers.emplace_back(pr);
                                        }
                                    }
                                    return r;
                                });

    peer->sendPeers(peers);
}

void
OverlayMessageHandler::peers(Peer::pointer peer,
                             std::vector<PeerAddress> const& peers)
{
    const uint32 NEW_PEER_WINDOW_SECONDS = 10;

    for (auto const& peerAddress : peers)
    {
        if (peerAddress.port == 0 || peerAddress.port > UINT16_MAX)
        {
            CLOG(WARNING, "Overlay")
                << "ignoring received peer with bad port " << peerAddress.port;
            continue;
        }
        if (peerAddress.ip.type() == IPv6)
        {
            CLOG(WARNING, "Overlay") << "ignoring received IPv6 address"
                                     << " (not yet supported)";
            continue;
        }
        // randomize when we'll try to connect to this peer next if we don't
        // know it
        auto defaultNextAttempt =
            mApp.getClock().now() +
            std::chrono::seconds(std::rand() % NEW_PEER_WINDOW_SECONDS);

        assert(peerAddress.ip.type() == IPv4);
        auto address = PeerBareAddress{peerAddress};

        if (address.isPrivate())
        {
            CLOG(WARNING, "Overlay")
                << "ignoring received private address " << address.toString();
        }
        else if (address == PeerBareAddress{peer->getAddress().getIP(),
                                            mApp.getConfig().PEER_PORT})
        {
            CLOG(WARNING, "Overlay")
                << "ignoring received self-address " << address.toString();
        }
        else if (address.isLocalhost() &&
                 !mApp.getConfig().ALLOW_LOCALHOST_FOR_TESTING)
        {
            CLOG(WARNING, "Overlay") << "ignoring received localhost";
        }
        else
        {
            // don't use peerAddress.numFailures here as we may have better luck
            // (and we don't want to poison our failure count)
            PeerRecord pr{address, defaultNextAttempt, 0};
            pr.insertIfNew(mApp.getDatabase());
        }
    }
}

void
OverlayMessageHandler::doesNotHave(Peer::pointer peer, ItemKey itemKey)
{
    mApp.getItemFetcher().doesntHave(peer, itemKey);
}

void
OverlayMessageHandler::getTxSet(Peer::pointer peer, Hash const& hash)
{
    if (auto txSet = mApp.getOverlayManager().getTxSetCache().get(hash))
    {
        StellarMessage newMsg;
        newMsg.type(TX_SET);
        txSet->toXDR(newMsg.txSet());

        peer->sendMessage(newMsg);
    }
    else
    {
        peer->sendDontHave(TX_SET, hash);
    }
}

void
OverlayMessageHandler::getQuorumSet(Peer::pointer peer, Hash const& hash)
{
    if (auto qset = mApp.getOverlayManager().getQSetCache().get(hash))
    {
        peer->sendSCPQuorumSet(qset);
    }
    else
    {
        if (Logging::logTrace("Overlay"))
            CLOG(TRACE, "Overlay") << "No quorum set: " << hexAbbrev(hash);
        peer->sendDontHave(SCP_QUORUMSET, hash);
        // do we want to ask other people for it?
    }
}

std::set<SCPEnvelope>
OverlayMessageHandler::txSet(Peer::pointer peer, TransactionSet const& txSet,
                             bool force)
{
    auto txSetFrame = std::make_shared<TxSetFrame>(mApp.getNetworkID(), txSet);
    auto envelopes = mApp.getPendingEnvelopes().handleTxSet(txSetFrame, force);
    for (auto& e : envelopes)
    {
        mApp.getEnvelopeHandler().envelope(peer, e);
    }
    return envelopes;
}

std::set<SCPEnvelope>
OverlayMessageHandler::quorumSet(Peer::pointer peer, SCPQuorumSet const& qSet)
{
    auto envelopes = mApp.getPendingEnvelopes().handleQuorumSet(qSet);
    for (auto& e : envelopes)
    {
        mApp.getEnvelopeHandler().envelope(peer, e);
    }
    return envelopes;
}
}
