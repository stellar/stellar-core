#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/PeerBareAddress.h"

namespace soci
{
class statement;
}

namespace stellar
{

class Database;
class RandomPeerSource;

enum class PeerType
{
    INBOUND,
    OUTBOUND,
    PREFERRED
};

enum class PeerTypeFilter
{
    INBOUND_ONLY,
    OUTBOUND_ONLY,
    PREFERRED_ONLY,
    ANY_OUTBOUND
};

/**
 * Raw database record of peer data. Its key is PeerBareAddress.
 */
struct PeerRecord
{
    std::tm mNextAttempt;
    int mNumFailures{0};
    int mType{0};
};

bool operator==(PeerRecord const& x, PeerRecord const& y);

struct PeerQuery
{
    bool mUseNextAttempt;
    int mMaxNumFailures;
    PeerTypeFilter mTypeFilter;
};

PeerAddress toXdr(PeerBareAddress const& address);

/**
 * Maintain list of know peers in database.
 */
class PeerManager
{
  public:
    enum class TypeUpdate
    {
        SET_OUTBOUND,
        SET_PREFERRED,
        REMOVE_PREFERRED,
        UPDATE_TO_OUTBOUND
    };

    enum class BackOffUpdate
    {
        HARD_RESET,
        RESET,
        INCREASE
    };

    static void dropAll(Database& db);

    /**
     * Ensure that given peer is stored in database.
     */
    virtual void ensureExists(PeerBareAddress const& address) = 0;

    /**
     * Update type of peer associated with given address.
     */
    virtual void update(PeerBareAddress const& address, TypeUpdate type) = 0;

    /**
     * Update "next try" of peer associated with given address - can reset
     * it to now or back off even further in future.
     */
    virtual void update(PeerBareAddress const& address,
                        BackOffUpdate backOff) = 0;

    /**
     * Update both type and "next try" of peer associated with given address.
     */
    virtual void update(PeerBareAddress const& address, TypeUpdate type,
                        BackOffUpdate backOff) = 0;

    /**
     * Load PeerRecord data for peer with given address. If not available in
     * database, create default one. Second value in pair is true when data
     * was loaded from database, false otherwise.
     */
    virtual std::pair<PeerRecord, bool>
    load(PeerBareAddress const& address) = 0;

    /**
     * Store PeerRecord data into database. If inDatabase is true, uses UPDATE
     * query, uses INSERT otherwise.
     */
    virtual void store(PeerBareAddress const& address,
                       PeerRecord const& PeerRecord, bool inDatabase) = 0;

    /**
     * Load size random peers matching query from database.
     */
    virtual std::vector<PeerBareAddress> loadRandomPeers(PeerQuery const& query,
                                                         int size) = 0;

    /**
     * Remove peers that have at least minNumFailures. Can only remove peer with
     * given address.
     */
    virtual void
    removePeersWithManyFailures(int minNumFailures,
                                PeerBareAddress const* address = nullptr) = 0;

    /**
     * Get list of peers to send to peer with given address.
     */
    virtual std::vector<PeerBareAddress>
    getPeersToSend(int size, PeerBareAddress const& address) = 0;

    virtual ~PeerManager()
    {
    }
};
}
