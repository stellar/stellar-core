#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Peer.h"
#include "overlay/StellarXDR.h"
#include <map>
#include <memory>
#include <set>

/**
 * FloodGate keeps track of which peers have sent us which broadcast messages,
 * in order to ensure that for each broadcast message M and for each peer P, we
 * either send M to P once (and only once), or receive M _from_ P (thereby
 * inhibit sending M to P at all).
 *
 * The broadcast message types are TRANSACTION and SCP_MESSAGE.
 *
 * All messages are marked with the ledger sequence number to which they
 * relate, and all flood-management information for a given ledger number
 * is purged from the FloodGate when the ledger closes.
 */

namespace medida
{
class Counter;
}

namespace stellar
{

class Floodgate
{
    class FloodRecord
    {
      public:
        typedef std::shared_ptr<FloodRecord> pointer;

        uint32_t mLedgerSeq;
        StellarMessage mMessage;
        std::set<std::string> mPeersTold;

        // mKeyedShortHash is SIPHash24 of mMessage keyed by the the low 128
        // bits of the LCL hash at which it was received (or broadcast). The
        // hash keying serves two goals:
        //
        //  - Preventing accidental collision masking a tx: on next ledger
        //    close, HerderImpl::updateTransactionQueue will rebroadcast any
        //    _unconsumed_ txs in the tx queue under new hashes, so they will
        //    just be delayed / sent twice by such accidental poisoning.
        //
        //  - Narrow the attack window for any _intentional_ poisoning to a
        //    short real-time window: attacker would have to find a SIPHash24
        //    preimage in the window of time between the LCL hash being known
        //    and the broadcast they want to poison being flooded ahead of their
        //    poison.
        //
        // We also calculate an mUnkeyedShortHash (well, keyed by 128 zero bits)
        // representing the short hash value to use in adverts (and that other
        // nodes will avertize to us) when out of sync.
        uint64_t mKeyedShortHash;
        uint64_t mUnkeyedShortHash;

        FloodRecord(StellarMessage const& msg, uint32_t ledger,
                    uint64_t keyedShortHash, uint64_t unkeyedShortHash);
    };

    bool shouldFloodLazily(StellarMessage const& msg, Peer::pointer peer);

    std::pair<std::map<Hash, FloodRecord::pointer>::iterator, bool>
    insert(StellarMessage const& msg, bool force = false);
    void removeAnyPendingDemand(uint64_t hash);
    void addPendingDemand(uint64_t hash, std::string const& peer);

    // Messages in mFloodMap are those we have a full copy of, and are in the
    // process of flooding to others.
    std::map<Hash, FloodRecord::pointer> mFloodMap;

    // Pointers into the same map but indexed by short hash (both keyed and
    // unkeyed).
    std::map<uint64_t, FloodRecord::pointer> mShortHashFloodMap;

    // The set of short hashes we've sent demands for, so we only demand them
    // from one peer at a time. The value stored in this map is the time at
    // which we made our last demand, and the peer we demanded it from. We use
    // this to GC demands older than 1 ledger (in clearBelow) and more narrowly
    // we allow re-issuing demands (to other peers) if a pending demand is older
    // older than PENDING_DEMAND_TIMEOUT.
    std::map<uint64_t, std::pair<std::string, VirtualClock::time_point>>
        mPendingDemands;

    // The current number of pending demands against a given peer. This is used
    // to limit the amount of memory a peer can use by flooding us with a set of
    // nonsense adverts and never answering demands for them. See
    // PENDING_DEMAND_LIMIT.
    std::map<std::string, size_t> mPendingDemandCount;

    Application& mApp;
    std::string mId;
    medida::Counter& mFloodMapSize;
    medida::Counter& mPendingDemandsSize;
    medida::Meter& mSendFromBroadcast;
    medida::Meter& mMessagesAdvertized;
    medida::Meter& mMessagesDemanded;
    medida::Meter& mMessagesFulfilled;
    bool mShuttingDown;

  public:
    Floodgate(Application& app);
    // forget data strictly older than `maxLedger`
    void clearBelow(uint32_t maxLedger);
    // returns true if this is a new record
    // fills msgID with msg's hash
    bool addRecord(StellarMessage const& msg, Peer::pointer fromPeer,
                   Hash& msgID);

    // returns true if msg was sent to at least one peer
    bool broadcast(StellarMessage const& msg, bool force);

    void demandMissing(FloodAdvert const& adv, Peer::pointer fromPeer);
    void fulfillDemand(FloodDemand const& dmd, Peer::pointer fromPeer);

    // returns the list of peers that sent us the item with hash `msgID`
    // NB: `msgID` is the hash of a `StellarMessage`
    std::set<Peer::pointer> getPeersKnows(Hash const& msgID);

    // removes the record corresponding to `msgID`
    // `msgID` corresponds to a `StellarMessage`
    void forgetRecord(Hash const& msgID);

    void shutdown();

    void updateRecord(StellarMessage const& oldMsg,
                      StellarMessage const& newMsg);
};
}
