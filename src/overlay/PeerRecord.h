#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include "main/Config.h"
#include "overlay/PeerBareAddress.h"
#include "util/Timer.h"
#include "util/optional.h"
#include <string>

namespace stellar
{
using namespace std;

class PeerRecord
{
  private:
    PeerBareAddress mAddress;
    bool mIsPreferred;

  public:
    VirtualClock::time_point mNextAttempt;
    int mNumFailures;

    /**
     * Create new PeerRecord object. If preconditions are not met - exception
     * is thrown.
     *
     * @pre: !ip.empty()
     * @pre: port > 0
     */
    PeerRecord(PeerBareAddress address, VirtualClock::time_point nextAttempt,
               int fails = 0);

    bool
    operator==(PeerRecord const& other) const
    {
        return mAddress == other.mAddress &&
               mNextAttempt == other.mNextAttempt &&
               mNumFailures == other.mNumFailures;
    }

    /**
     * Load PeerRecord from database. If preconditions are not met - exception
     * is thrown from PeerRecord constructor.
     * If given PeerRecord is not found, nullopt is returned.
     * If @p ip is empty or @ip is set to 0 nullopt is returned.
     */
    static optional<PeerRecord> loadPeerRecord(Database& db,
                                               PeerBareAddress const& address);

    // pred returns false if we should stop processing entries
    static void loadPeerRecords(Database& db, int batchSize,
                                VirtualClock::time_point nextAttemptCutoff,
                                std::function<bool(PeerRecord const& pr)> pred);

    PeerBareAddress const&
    getAddress() const
    {
        return mAddress;
    };

    void setPreferred(bool p);
    bool isPreferred() const;

    // insert record in database if it's a new record
    // returns true if inserted
    bool insertIfNew(Database& db);

    // insert or update record from database
    void storePeerRecord(Database& db);

    void resetBackOff(VirtualClock& clock);
    void backOff(VirtualClock& clock);

    void toXdr(PeerAddress& ret) const;

    static void dropAll(Database& db);
    std::string toString() const;

  private:
    // peerRecordProcessor returns false if we should stop processing entries
    static void
    loadPeerRecords(Database& db, StatementContext& prep,
                    std::function<bool(PeerRecord const&)> peerRecordProcessor);
    std::chrono::seconds computeBackoff(VirtualClock& clock);
    static const char* kSQLCreateStatement;
};
}
