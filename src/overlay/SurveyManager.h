#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Peer.h"
#include "overlay/SurveyDataManager.h"
#include "overlay/SurveyMessageLimiter.h"
#include "util/Timer.h"
#include "util/UnorderedSet.h"
#include <lib/json/json.h>
#include <optional>

namespace stellar
{
class Application;

/*
SurveyManager orchestrates network surveys by initiating them and
maintaining a backlog of peers to survey, sending and processing messages,
throttling requests to prevent overload, aggregating results, and concluding the
survey upon completion or expiry.
*/
class SurveyManager : public std::enable_shared_from_this<SurveyManager>,
                      public NonMovableOrCopyable
{
  public:
    static uint32_t const SURVEY_THROTTLE_TIMEOUT_MULT;

    SurveyManager(Application& app);

    // Start/stop survey reporting. Must be called before/after gathering data
    // during the reporting phase of a survey. `surveyDuration` must be provided
    // for old style surveys, and must not be provided for time sliced surveys.
    bool
    startSurveyReporting(SurveyMessageCommandType type,
                         std::optional<std::chrono::seconds> surveyDuration);
    void stopSurveyReporting();

    // Add a node to the backlog of nodes to survey. inboundPeerIndex and
    // outboundPeerIndex are mandatory for time sliced surveys and indicate
    // which peers the node should report on
    void addNodeToRunningSurveyBacklog(
        SurveyMessageCommandType type,
        std::optional<std::chrono::seconds> surveyDuration,
        NodeID const& nodeToSurvey, std::optional<uint32_t> inboundPeerIndex,
        std::optional<uint32_t> outboundPeerIndex);

    void relayOrProcessResponse(StellarMessage const& msg, Peer::pointer peer);
    void relayOrProcessRequest(StellarMessage const& msg, Peer::pointer peer);
    void clearOldLedgers(uint32_t lastClosedledgerSeq);
    Json::Value const& getJsonResults();

    static std::string getMsgSummary(StellarMessage const& msg);

    StellarMessage makeOldStyleSurveyRequest(NodeID const& nodeToSurvey) const;

    // Start survey collecting with a given nonce. Returns `false` if unable to
    // start a survey due to an ongoing survey on the network. Otherwise returns
    // `true`. Note that a `true` result does not guarantee that the survey will
    // be successful. It is possible that a survey is already ongoing that this
    // node does not know about.
    bool broadcastStartSurveyCollecting(uint32_t nonce);

    void relayStartSurveyCollecting(StellarMessage const& msg,
                                    Peer::pointer peer);

    // Stop survey collecting. Uses nonce of the currently running survey.
    // Returns `false` if no survey is currently active.
    bool broadcastStopSurveyCollecting();

    void relayStopSurveyCollecting(StellarMessage const& msg,
                                   Peer::pointer peer);

    // The following functions expose functions by the same name in
    // `mSurveyDataManager`
    void modifyNodeData(std::function<void(CollectingNodeData&)> f);
    void modifyPeerData(Peer const& peer,
                        std::function<void(CollectingPeerData&)> f);
    void recordDroppedPeer(Peer const& peer);
    void updateSurveyPhase(std::map<NodeID, Peer::pointer> const& inboundPeers,
                           std::map<NodeID, Peer::pointer> const& outboundPeers,
                           Config const& config);

#ifdef BUILD_TESTS
    // Get a reference to the internal `SurveyDataManager` (for testing only)
    SurveyDataManager& getSurveyDataManagerForTesting();
#endif

  private:
    // topology specific methods
    void sendTopologyRequest(NodeID const& nodeToSurvey);
    void processOldStyleTopologyResponse(NodeID const& surveyedPeerID,
                                         SurveyResponseBody const& body);
    void
    processOldStyleTopologyRequest(SurveyRequestMessage const& request) const;
    void processTimeSlicedTopologyResponse(NodeID const& surveyedPeerID,
                                           SurveyResponseBody const& body);
    void processTimeSlicedTopologyRequest(
        TimeSlicedSurveyRequestMessage const& request);

    // Populate `response` with the data from the other parameters.  Returns
    // `false` on encryption failure.
    bool populateSurveyResponseMessage(SurveyRequestMessage const& request,
                                       SurveyMessageCommandType type,
                                       SurveyResponseBody const& body,
                                       SurveyResponseMessage& response) const;

    // Populate `request` with the data from the other parameters
    void populateSurveyRequestMessage(NodeID const& nodeToSurvey,
                                      SurveyMessageCommandType type,
                                      SurveyRequestMessage& request) const;

    void broadcast(StellarMessage const& msg) const;
    void populatePeerStats(std::vector<Peer::pointer> const& peers,
                           PeerStatList& results,
                           VirtualClock::time_point now) const;
    void recordResults(Json::Value& jsonResultList,
                       PeerStatList const& peerList) const;

    void topOffRequests(SurveyMessageCommandType type);
    void updateOldStyleSurveyExpiration(std::chrono::seconds surveyDuration);

    // Add `nodeToSurvey` to the survey backlog. Throws if the node is
    // already queued up to survey, or if the node itself is the surveyor.
    void addPeerToBacklog(NodeID const& nodeToSurvey);

    // returns true if signature is valid
    bool dropPeerIfSigInvalid(PublicKey const& key, Signature const& signature,
                              ByteSlice const& bin, Peer::pointer peer);

    static std::string commandTypeName(SurveyMessageCommandType type);

    // Validate a survey response message. Returns the message if it is valid
    // and nullopt otherwise.
    std::optional<SurveyResponseMessage>
    validateSurveyResponse(StellarMessage const& msg, Peer::pointer peer);

    // Validate a time sliced survey response message. Returns the message if it
    // is valid and nullopt otherwise.
    std::optional<SurveyResponseMessage>
    validateTimeSlicedSurveyResponse(StellarMessage const& msg,
                                     Peer::pointer peer);

    // Returns `true` if this node's configuration allows it to be surveyed by
    // `surveyorID`
    bool surveyorPermitted(NodeID const& surveyorID) const;

    // Returns `true` if the survey has finished the reporting phase
    bool surveyIsFinishedReporting();

    Application& mApp;

    std::unique_ptr<VirtualTimer> mSurveyThrottleTimer;
    VirtualClock::time_point mSurveyExpirationTime;

    uint32_t const NUM_LEDGERS_BEFORE_IGNORE;
    uint32_t const MAX_REQUEST_LIMIT_PER_LEDGER;

    // If a survey is in the reporting phase, this will be set to the type of
    // the running survey
    std::optional<SurveyMessageCommandType> mRunningSurveyReportingPhaseType;
    Curve25519Secret mCurve25519SecretKey;
    Curve25519Public mCurve25519PublicKey;
    SurveyMessageLimiter mMessageLimiter;

    UnorderedSet<NodeID> mPeersToSurvey;
    std::queue<NodeID> mPeersToSurveyQueue;

    // Indices to use when surveying peers for time sliced surveys
    std::unordered_map<NodeID, uint32_t> mInboundPeerIndices;
    std::unordered_map<NodeID, uint32_t> mOutboundPeerIndices;

    std::chrono::seconds const SURVEY_THROTTLE_TIMEOUT_SEC;

    UnorderedSet<NodeID> mBadResponseNodes;
    Json::Value mResults;

    // Manager for time-sliced survey data
    SurveyDataManager mSurveyDataManager;
};
}
