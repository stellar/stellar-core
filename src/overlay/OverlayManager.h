#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include "transport/EnvelopeHandler.h"
#include "transport/Peer.h"
#include "transport/TransactionHandler.h"

#include <set>

/**
 * OverlayManager maintains a virtual broadcast network, consisting of a set of
 * remote TCP peers (TCPPeer), a mechanism for flooding messages to all peers
 * (FloodGate), and a mechanism for sending and receiving anycast request/reply
 * pairs (ItemFetcher).
 *
 * Overlay network messages are defined as the XDR structure type
 * `StellarMessage`, in the file src/xdr/Stellar-overlay.x
 *
 * They are minimally framed using the Record Marking (RM) standard of RFC5531
 * (https://tools.ietf.org/html/rfc5531#page-16) and the RM-framed messages are
 * transmitted over TCP/IP sockets, between peers.
 *
 * The `StellarMessage` union contains 3 logically distinct kinds of message:
 *
 *  - Messages directed to or from a specific peer, with or without a response:
 *    HELLO, GET_PEERS, PEERS, DONT_HAVE, ERROR_MSG
 *
 *  - One-way broadcast messages informing other peers of an event:
 *    TRANSACTION and SCP_MESSAGE
 *
 *  - Two-way anycast messages requesting a value (by hash) or providing it:
 *    GET_TX_SET, TX_SET, GET_SCP_QUORUMSET, SCP_QUORUMSET, GET_SCP_STATE
 *
 * Anycasts are initiated and serviced two instances of ItemFetcher
 * (mTxSetFetcher and mQuorumSetFetcher). Anycast messages are sent to
 * directly-connected peers, in sequence until satisfied. They are not
 * flooded between peers.
 *
 * Broadcasts are initiated by the Herder and sent to both the Herder _and_ the
 * local FloodGate, for propagation to other peers.
 *
 * The OverlayManager tracks its known peers in the Database and shares peer
 * records with other peers when asked.
 */

namespace stellar
{

class Database;
class LoadManager;
class QSetCache;
class TxSetCache;

class OverlayManager
{
  public:
    static std::unique_ptr<OverlayManager> create(Application& app);

    // Drop all PeerRecords from the Database
    static void dropAll(Database& db);

    // Flush all FloodGate and ItemFetcher state for ledgers older than
    // `ledger`.
    // This is called by LedgerManager when a ledger closes.
    virtual void ledgerClosed(uint32_t lastClosedledgerSeq) = 0;

    // Transaction has been processed. Overlay can now broadcast it if
    // neccessary.
    virtual void
    transactionProcessed(Peer::pointer peer, uint32_t ledgerSeq,
                         TransactionEnvelope const& transaction,
                         TransactionHandler::TransactionStatus status) = 0;

    // SCP envelope has been processed. Overlay can now broadcast it if
    // neccessary.
    virtual void
    scpEnvelopeProcessed(Peer::pointer peer, uint32_t ledgerSeq,
                         SCPEnvelope const& envelope,
                         EnvelopeHandler::EnvelopeStatus status) = 0;

    // Send a given message to all peers, via the FloodGate. This is called by
    // Herder.
    virtual void broadcastMessage(StellarMessage const& msg, uint32_t ledgerSeq,
                                  bool force = false) = 0;

    // Return the persistent peer-load-accounting cache.
    virtual LoadManager& getLoadManager() = 0;

    // return the persistent quorum set cache
    virtual QSetCache& getQSetCache() = 0;

    // return the persistent tx set cache
    virtual TxSetCache& getTxSetCache() = 0;

    // start up all background tasks for overlay
    virtual void start() = 0;
    // drops all connections
    virtual void shutdown() = 0;

    virtual bool isShuttingDown() const = 0;

    virtual ~OverlayManager()
    {
    }
};
}
