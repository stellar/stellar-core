#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Floodgate.h"
#include "overlay/OverlayManager.h"
#include "overlay/QSetCache.h"
#include "overlay/TxSetCache.h"
#include "transport/LoadManager.h"
#include "transport/TCPAcceptor.h"

#include <set>
#include <vector>

namespace medida
{
class Meter;
class Counter;
}

/*
Maintain the set of peers we are connected to
*/
namespace stellar
{

class PeerRecord;

class OverlayManagerImpl : public OverlayManager /*, public MessageHandler*/
{
  protected:
    Application& mApp;

    LoadManager mLoad;
    QSetCache mQSetCache;
    TxSetCache mTxSetCache;
    TCPAcceptor mTCPAcceptor;
    bool mShuttingDown;

    medida::Meter& mMessagesReceived;
    medida::Meter& mMessagesBroadcast;

    void tick();
    VirtualTimer mTimer;

    friend class OverlayManagerTests;

    Floodgate mFloodGate;

  public:
    OverlayManagerImpl(Application& app);
    ~OverlayManagerImpl();

    void ledgerClosed(uint32_t lastClosedledgerSeq) override;
    void
    transactionProcessed(Peer::pointer peer, uint32_t ledgerSeq,
                         TransactionEnvelope const& transaction,
                         TransactionHandler::TransactionStatus status) override;
    void scpEnvelopeProcessed(Peer::pointer peer, uint32_t ledgerSeq,
                              SCPEnvelope const& envelope,
                              EnvelopeHandler::EnvelopeStatus status) override;
    void broadcastMessage(StellarMessage const& msg, uint32_t ledgerSeq,
                          bool force = false) override;

    LoadManager& getLoadManager() override;
    QSetCache& getQSetCache() override;
    TxSetCache& getTxSetCache() override;

    void start() override;
    void shutdown() override;

    bool isShuttingDown() const override;

  private:
    std::vector<PeerRecord> getPreferredPeersFromConfig();
    std::vector<PeerRecord> getPeersToConnectTo(int maxNum);

    void connectToMorePeers(std::vector<PeerRecord>& peers);
    void recvFloodedMsg(StellarMessage const& msg, uint32_t ledgerSeq,
                        Peer::pointer peer);
};
}
