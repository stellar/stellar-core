#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include "main/Config.h"
#include "util/Timer.h"
#include "util/optional.h"
#include <string>

namespace stellar
{
using namespace std;

class PeerRecord
{
  private:
    std::string mIP;
    unsigned short mPort;

  public:
    VirtualClock::time_point mNextAttempt;
    uint32_t mNumFailures;

    /**
     * Create new PeerRecord object. If preconditions are not met - exception
     * is thrown.
     *
     * @pre: !ip.empty()
     * @pre: port > 0
     */
    PeerRecord(string const& ip, unsigned short port,
               VirtualClock::time_point nextAttempt, uint32_t fails = 0);

    bool
    operator==(PeerRecord& other)
    {
        return mIP == other.mIP && mPort == other.mPort &&
               mNextAttempt == other.mNextAttempt &&
               mNumFailures == other.mNumFailures;
    }

    static PeerRecord
    parseIPPort(std::string const& ipPort, Application& app,
                unsigned short defaultPort = DEFAULT_PEER_PORT);

    /**
     * Load PeerRecord from database. If preconditions are not met - exception
     * is thrown from PeerRecord constructor.
     * If given PeerRecord is not found, nullopt is returned.
     * If @p ip is empty or @ip is set to 0 nullopt is returned.
     */
    static optional<PeerRecord> loadPeerRecord(Database& db, std::string ip,
                                               unsigned short port);
    static void loadPeerRecords(Database& db, int batchSize,
                                VirtualClock::time_point nextAttemptCutoff,
                                std::function<bool(PeerRecord const& pr)> p);
    const std::string&
    ip() const
    {
        return mIP;
    };
    unsigned short
    port() const
    {
        return mPort;
    };

    bool isSelfAddressAndPort(std::string const& ip, unsigned short port) const;
    bool isPrivateAddress() const;
    bool isLocalhost() const;

    // insert record in database if it's a new record
    // returns true if inserted
    bool insertIfNew(Database& db);

    // insert or update record from database
    void storePeerRecord(Database& db);

    void resetBackOff(VirtualClock& clock, bool preferred);
    void backOff(VirtualClock& clock);

    void toXdr(PeerAddress& ret) const;

    static void dropAll(Database& db);
    std::string toString();

  private:
    std::chrono::seconds computeBackoff(VirtualClock& clock);
    static void ipToXdr(std::string ip, xdr::opaque_array<4U>& ret);
    static const char* kSQLCreateStatement;
};
}
