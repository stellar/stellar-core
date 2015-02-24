#pragma once

#include "util/Timer.h"
#include "database/Database.h"
#include <string>

namespace stellar
{
using namespace std;


class PeerRecord
{
public:
    string mIP;
    int mPort;
    VirtualClock::time_point mNextAttempt;
    int mNumFailures;
    int mRank;

    PeerRecord() {};

    PeerRecord(int id,const string& ip,int port, VirtualClock::time_point nextAttempt, int fails, int rank) :
        mIP(ip), mPort(port), mNextAttempt(nextAttempt), mNumFailures(fails), mRank(rank) { }

    bool operator==(PeerRecord& other)
    {
        return mIP == other.mIP &&
            mPort == other.mPort &&
            mNextAttempt == other.mNextAttempt &&
            mNumFailures == other.mNumFailures &&
            mRank == other.mRank;
    }

    static void fromIPPort(const string &ipPort, int defaultPort, VirtualClock &clock, PeerRecord &ret);


    static bool loadPeerRecord(Database &db, string ip, int port, PeerRecord &ret);
    static void loadPeerRecords(Database &db, int max, VirtualClock::time_point nextAttemptCutoff, vector<PeerRecord>& retList);

    bool isStored(Database &db);
    void storePeerRecord(Database& db);

    void backOff(VirtualClock &clock);
    
    void toXdr(PeerAddress &ret);
    
    static void dropAll(Database &db);
    string toString();

private:
    static void ipToXdr(string ip, xdr::opaque_array<4U>& ret);
    static void parseIPPort(const std::string& peerStr, int defaultPort, std::string& retIP, int& retPort);
    static const char *kSQLCreateStatement;
};

}

