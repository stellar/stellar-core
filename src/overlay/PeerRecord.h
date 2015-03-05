#pragma once

#include "util/Timer.h"
#include "database/Database.h"
#include <string>
#include "util/optional.h"
#include "main/Config.h"

namespace stellar
{
using namespace std;

class PeerRecord
{
public:
    string mIP;
    uint32_t mPort;
    VirtualClock::time_point mNextAttempt;
    uint32_t mNumFailures;
    uint32_t mRank;

    PeerRecord() {};

    PeerRecord(const string& ip, uint32_t port, VirtualClock::time_point nextAttempt, uint32_t fails, uint32_t rank) :
        mIP(ip), mPort(port), mNextAttempt(nextAttempt), mNumFailures(fails), mRank(rank) { }

    bool operator==(PeerRecord& other)
    {
        return mIP == other.mIP &&
            mPort == other.mPort &&
            mNextAttempt == other.mNextAttempt &&
            mNumFailures == other.mNumFailures &&
            mRank == other.mRank;
    }

    static void fromIPPort(const string &ip, uint32_t port, VirtualClock &clock, PeerRecord &ret);
    static void parseIPPort(const string &ipPort, VirtualClock &clock, PeerRecord &ret, uint32_t defaultPort = DEFAULT_PEER_PORT);


    static optional<PeerRecord> loadPeerRecord(Database &db, string ip, uint32_t port);
    static void loadPeerRecords(Database &db, uint32_t max, VirtualClock::time_point nextAttemptCutoff, vector<PeerRecord>& retList);

    bool isStored(Database &db);
    void storePeerRecord(Database& db);

    void backOff(VirtualClock &clock);
    
    void toXdr(PeerAddress &ret);
    
    static void dropAll(Database &db);
    string toString();

private:
    static void ipToXdr(string ip, xdr::opaque_array<4U>& ret);
    static const char *kSQLCreateStatement;
};

}

