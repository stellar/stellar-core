#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Curve25519.h"
#include "overlay/Peer.h"
#include "overlay/StellarXDR.h"
#include "overlay/SurveyMessageLimiter.h"
#include "util/Timer.h"
#include "util/UnorderedSet.h"
#include <lib/json/json.h>

namespace stellar
{
class Application;

class SurveyManager : public std::enable_shared_from_this<SurveyManager>,
                      public NonMovableOrCopyable
{
  public:
    static uint32_t const SURVEY_THROTTLE_TIMEOUT_MULT;

    SurveyManager(Application& app);

    bool startSurvey(SurveyMessageCommandType type,
                     std::chrono::seconds surveyDuration);
    void stopSurvey();

    void addNodeToRunningSurveyBacklog(SurveyMessageCommandType type,
                                       std::chrono::seconds surveyDuration,
                                       NodeID const& nodeToSurvey);

    void relayOrProcessResponse(StellarMessage const& msg, Peer::pointer peer);
    void relayOrProcessRequest(StellarMessage const& msg, Peer::pointer peer);
    void clearOldLedgers(uint32_t lastClosedledgerSeq);
    Json::Value const& getJsonResults();

    static std::string getMsgSummary(StellarMessage const& msg);

  private:
    // topology specific methods
    void sendTopologyRequest(NodeID const& nodeToSurvey) const;
    void processTopologyResponse(NodeID const& surveyedPeerID,
                                 SurveyResponseBody const& body);
    void processTopologyRequest(SurveyRequestMessage const& request) const;

    void broadcast(StellarMessage const& msg) const;
    void populatePeerStats(std::vector<Peer::pointer> const& peers,
                           PeerStatList& results,
                           VirtualClock::time_point now) const;
    void recordResults(Json::Value& jsonResultList,
                       PeerStatList const& peerList) const;

    void topOffRequests(SurveyMessageCommandType type);
    void updateSurveyExpiration(std::chrono::seconds surveyDuration);

    void addPeerToBacklog(NodeID const& nodeToSurvey);

    // returns true if signature is valid
    bool dropPeerIfSigInvalid(PublicKey const& key, Signature const& signature,
                              ByteSlice const& bin, Peer::pointer peer);

    static std::string commandTypeName(SurveyMessageCommandType type);

    Application& mApp;

    std::unique_ptr<VirtualTimer> mSurveyThrottleTimer;
    VirtualClock::time_point mSurveyExpirationTime;

    uint32_t const NUM_LEDGERS_BEFORE_IGNORE;
    uint32_t const MAX_REQUEST_LIMIT_PER_LEDGER;

    optional<SurveyMessageCommandType> mRunningSurveyType;
    Curve25519Secret mCurve25519SecretKey;
    Curve25519Public mCurve25519PublicKey;
    SurveyMessageLimiter mMessageLimiter;

    UnorderedSet<NodeID> mPeersToSurvey;
    std::queue<NodeID> mPeersToSurveyQueue;

    std::chrono::seconds const SURVEY_THROTTLE_TIMEOUT_SEC;

    UnorderedSet<NodeID> mBadResponseNodes;
    Json::Value mResults;
};
}
