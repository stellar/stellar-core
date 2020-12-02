#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/PeerBareAddress.h"
#include "util/Timer.h"

#include <functional>

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
        ENSURE_OUTBOUND,
        SET_PREFERRED,
        ENSURE_NOT_PREFERRED,
    };

    enum class BackOffUpdate
    {
        HARD_RESET,
        RESET,
        INCREASE
    };

    static void dropAll(Database& db);

    explicit PeerManager(Application& app);

    /**
     * Ensure that given peer is stored in database.
     */
    void ensureExists(PeerBareAddress const& address);

    /**
     * Update type of peer associated with given address. This function takes
     * observed peer type, and whether the preferred type is definitely known
     * (in some cases it is unknown whether a peer is preferred or not).
     * Depending on the peer type stored in the DB, a new type is determined.
     */
    void update(PeerBareAddress const& address, PeerType observedType,
                bool preferredTypeKnown);

    /**
     * Update "next try" of peer associated with given address - can reset
     * it to now or back off even further in future.
     */
    void update(PeerBareAddress const& address, BackOffUpdate backOff);

    /**
     * Update both type and "next try" of peer associated with given address.
     */
    void update(PeerBareAddress const& address, PeerType observedType,
                bool preferredTypeKnown, BackOffUpdate backOff);

    /**
     * Load PeerRecord data for peer with given address. If not available in
     * database, create default one. Second value in pair is true when data
     * was loaded from database, false otherwise.
     */
    std::pair<PeerRecord, bool> load(PeerBareAddress const& address);

    /**
     * Store PeerRecord data into database. If inDatabase is true, uses UPDATE
     * query, uses INSERT otherwise.
     */
    void store(PeerBareAddress const& address, PeerRecord const& PeerRecord,
               bool inDatabase);

    /**
     * Load size random peers matching query from database.
     */
    std::vector<PeerBareAddress> loadRandomPeers(PeerQuery const& query,
                                                 int size);

    /**
     * Remove peers that have at least minNumFailures. Can only remove peer with
     * given address.
     */
    void removePeersWithManyFailures(int minNumFailures,
                                     PeerBareAddress const* address = nullptr);

    /**
     * Get list of peers to send to peer with given address.
     */
    std::vector<PeerBareAddress> getPeersToSend(int size,
                                                PeerBareAddress const& address);

  private:
    static const char* kSQLCreateStatement;

    Application& mApp;
    std::unique_ptr<RandomPeerSource> mOutboundPeersToSend;
    std::unique_ptr<RandomPeerSource> mInboundPeersToSend;

    int countPeers(std::string const& where,
                   std::function<void(soci::statement&)> const& bind);
    std::vector<PeerBareAddress>
    loadPeers(int limit, int offset, std::string const& where,
              std::function<void(soci::statement&)> const& bind);

    void update(PeerRecord& peer, TypeUpdate type);
    void update(PeerRecord& peer, BackOffUpdate backOff, Application& app);
};
}
