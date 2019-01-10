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

/**
 * Raw database record of peer data. Its key is PeerBareAddress.
 */
struct PeerRecord
{
    std::tm mNextAttempt;
    int mNumFailures{0};
    int mFlags{0};
    int mIsOutbound{0};
};

bool operator==(PeerRecord const& x, PeerRecord const& y);

using PeerRecordModifier = std::function<void(Application&, PeerRecord&)>;

namespace PeerRecordModifiers
{
void resetBackOff(Application& app, PeerRecord& peer);
void backOff(Application& app, PeerRecord& peer);
void markPreferred(Application& app, PeerRecord& peer);
void unmarkPreferred(Application& app, PeerRecord& peer);
void markOutbound(Application& app, PeerRecord& peer);
}

PeerAddress toXdr(PeerBareAddress const& address);

/**
 * Maintain list of know peers in database.
 */
class PeerManager
{
  public:
    struct PeerQuery
    {
        bool mNextAttempt;
        int mMaxNumFailures;
        int mOutbound;
    };

    static PeerQuery maxFailures(int maxFailures, bool outbound);
    static PeerQuery nextAttemptCutoff(bool outbound);

    static void dropAll(Database& db);

    explicit PeerManager(Application& app);

    std::vector<PeerBareAddress>
    getRandomPeers(PeerQuery const& query, size_t size,
                   std::function<bool(PeerBareAddress const&)> pred);

    /**
     * Update or create PeerRecord entry in database with given addrees. If
     * data for given address is not available, a new default PeerRecord is
     * created. Before saving data to database again each function in
     * peerModifiers parameter is called on that PeerRecord.
     */
    void update(PeerBareAddress const& address,
                std::vector<PeerRecordModifier> const& peerModifiers);

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

  private:
    static const char* kSQLCreateStatement;

    Application& mApp;
    size_t const mBatchSize;
    std::map<PeerQuery, std::vector<PeerBareAddress>> mPeerCache;

    std::vector<PeerBareAddress> loadRandomPeers(PeerQuery const& query,
                                                 size_t size);

    size_t countPeers(std::string const& where,
                      std::function<void(soci::statement&)> const& bind);
    std::vector<PeerBareAddress>
    loadPeers(int limit, int offset, std::string const& where,
              std::function<void(soci::statement&)> const& bind);
};

bool operator<(PeerManager::PeerQuery const& x,
               PeerManager::PeerQuery const& y);
}
