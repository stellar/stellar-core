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
class StatementContext;

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

PeerAddress toXdr(PeerBareAddress const& address);

/**
 * Maintain list of know peers in database.
 */
class PeerManager
{
  public:
    enum class TypeUpdate
    {
        KEEP,
        SET_INBOUND,
        SET_OUTBOUND,
        SET_PREFERRED,
        REMOVE_PREFERRED
    };

    enum class BackOffUpdate
    {
        KEEP,
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
     * Update type of peer associated with given address.
     */
    void update(PeerBareAddress const& address, TypeUpdate type);

    /**
     * Update "next try" of peer associated with given address - can reset
     * it to now or back off even further in future.
     */
    void update(PeerBareAddress const& address, BackOffUpdate backOff);

    /**
     * Update both type and "next try" of peer associated with given address.
     */
    void update(PeerBareAddress const& address, TypeUpdate type,
                BackOffUpdate backOff);

    /**
     * Update back off to now() + seconds.
     */
    void update(PeerBareAddress const& address, std::chrono::seconds seconds);

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

    // pred returns false if we should stop processing entries
    void loadPeers(int batchSize, VirtualClock::time_point nextAttemptCutoff,
                   PeerTypeFilter peerTypeFilter,
                   std::function<bool(PeerBareAddress const& address)> pred);

  private:
    static const char* kSQLCreateStatement;

    Application& mApp;

    void loadPeers(StatementContext& prep,
                   std::function<bool(PeerBareAddress const& address)>
                       peerRecordProcessor);

    void update(PeerRecord& peer, TypeUpdate type);
    void update(PeerRecord& peer, BackOffUpdate backOff, Application& app);
};
}
