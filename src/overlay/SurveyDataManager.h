#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "main/Config.h"
#include "overlay/Peer.h"
#include "util/NonCopyable.h"
#include "util/Timer.h"

#include "medida/histogram.h"
#include "medida/meter.h"
#include "xdr/Stellar-overlay.h"
#include "xdr/Stellar-types.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace stellar
{

enum class SurveyPhase
{
    // Survey is currently collecting data
    COLLECTING,
    // Collecting complete. Survey data is available for reporting.
    REPORTING,
    // No active survey in progress. No data is being collected or reported.
    INACTIVE
};

struct CollectingNodeData
{
    CollectingNodeData(uint64_t initialLostSyncCount,
                       Application::State initialState);

    // Peer change data
    uint32_t mAddedAuthenticatedPeers = 0;
    uint32_t mDroppedAuthenticatedPeers = 0;

    // SCP stats (in milliseconds)
    medida::Histogram mSCPFirstToSelfLatencyMsHistogram;
    medida::Histogram mSCPSelfToOtherLatencyMsHistogram;

    // To compute how many times the node lost sync in the time slice
    uint64_t const mInitialLostSyncCount;

    // State of the node at the start of the survey
    Application::State const mInitialState;
};

// Data about a peer
struct CollectingPeerData
{
    CollectingPeerData(Peer::PeerMetrics const& peerMetrics);

    // Metrics at the start of the survey
    uint64_t const mInitialMessageRead;
    uint64_t const mInitialMessageWrite;
    uint64_t const mInitialByteRead;
    uint64_t const mInitialByteWrite;
    uint64_t const mInitialUniqueFloodBytesRecv;
    uint64_t const mInitialDuplicateFloodBytesRecv;
    uint64_t const mInitialUniqueFetchBytesRecv;
    uint64_t const mInitialDuplicateFetchBytesRecv;
    uint64_t const mInitialUniqueFloodMessageRecv;
    uint64_t const mInitialDuplicateFloodMessageRecv;
    uint64_t const mInitialUniqueFetchMessageRecv;
    uint64_t const mInitialDuplicateFetchMessageRecv;

    // For computing average latency (in milliseconds)
    medida::Histogram mLatencyMsHistogram;
};

/*
 * Manage data collection during time sliced overlay surveys. This class is
 * thread-safe.
 */
class SurveyDataManager : public NonMovableOrCopyable
{
  public:
    // Create a survey manager. `clock` must be the Application's clock.
    // `lostSyncMeter` is a meter to track how many times the node lost sync.
    SurveyDataManager(std::function<VirtualClock::time_point()> const& getNow,
                      medida::Meter const& lostSyncMeter, Config const& cfg);

    // Start the collecting phase of a survey. Ignores requests if a survey is
    // already active. `inboundPeers` and `outboundPeers` should collectively
    // contain the `NodeID`s of all connected peers. Returns `true` if this
    // successfully starts a survey.
    bool
    startSurveyCollecting(TimeSlicedSurveyStartCollectingMessage const& msg,
                          std::map<NodeID, Peer::pointer> const& inboundPeers,
                          std::map<NodeID, Peer::pointer> const& outboundPeers,
                          Application::State initialState);

    // Stop the collecting phase of a survey and enter the reporting phase.
    // Ignores request if no survey is active or if nonce does not match the
    // active survey. Returns `true` if this successfully stops a survey.
    bool
    stopSurveyCollecting(TimeSlicedSurveyStopCollectingMessage const& msg,
                         std::map<NodeID, Peer::pointer> const& inboundPeers,
                         std::map<NodeID, Peer::pointer> const& outboundPeers,
                         Config const& config);

    // Apply `f` to the data for this node. Does nothing if the survey is not in
    // the collecting phase.
    void modifyNodeData(std::function<void(CollectingNodeData&)> f);

    // Apply `f` to the data for `peer`. Does nothing if the survey is not in
    // the collecting phase or if `peer` is not in the current time slice data.
    void modifyPeerData(Peer const& peer,
                        std::function<void(CollectingPeerData&)> f);

    // Record that `peer` was dropped from the overlay. Does nothing if the
    // survey is not in the collecting phase.
    void recordDroppedPeer(Peer const& peer);

    // Get nonce of current survey, if one is active.
    std::optional<uint32_t> getNonce() const;

    // Returns `true` if the `nonce` matches the survey in the reporting phase.
    bool nonceIsReporting(uint32_t nonce) const;

    // Fills `response` with the results of the survey, provided that the survey
    // corresponding to the request's nonce is in the reporting phase. Returns
    // `true` on success.
    bool fillSurveyData(TimeSlicedSurveyRequestMessage const& request,
                        TopologyResponseBodyV2& response);

    // Returns `true` iff there is currently an active survey
    bool surveyIsActive() const;

    // Checks and updates the phase of the survey if necessary. Resets the
    // survey state upon transition to `SurveyPhase::INACTIVE`. Takes peer and
    // config info in case the collecting phase times out and the survey
    // automatically transitions to the reporting phase.
    void updateSurveyPhase(std::map<NodeID, Peer::pointer> const& inboundPeers,
                           std::map<NodeID, Peer::pointer> const& outboundPeers,
                           Config const& config);

#ifdef BUILD_TESTS
    // Call to use the provided duration as max for both collecting and
    // reporting phase instead of the normal max phase durations
    void setPhaseMaxDurationsForTesting(std::chrono::minutes maxPhaseDuration);
#endif // BUILD_TESTS

  private:
    // Get the current time
    std::function<VirtualClock::time_point()> const mGetNow;

    // Metric tracking sync status
    medida::Meter const& mLostSyncMeter;

    // Start and stop times for the collecting phase
    std::optional<VirtualClock::time_point> mCollectStartTime = std::nullopt;
    std::optional<VirtualClock::time_point> mCollectEndTime = std::nullopt;

    // Nonce of the active survey (if any)
    std::optional<uint32_t> mNonce = std::nullopt;

    // Surveyor running active survey (if any)
    std::optional<NodeID> mSurveyor = std::nullopt;

    // Data about this node captured during the collecting phase
    std::optional<CollectingNodeData> mCollectingNodeData = std::nullopt;

    // Finalized reporting phase data about this node
    std::optional<TimeSlicedNodeData> mFinalNodeData = std::nullopt;

    // Data about peers during collecting phase
    std::unordered_map<NodeID, CollectingPeerData> mCollectingInboundPeerData;
    std::unordered_map<NodeID, CollectingPeerData> mCollectingOutboundPeerData;

    // Finalized reporting phase data about peers
    std::vector<TimeSlicedPeerData> mFinalInboundPeerData;
    std::vector<TimeSlicedPeerData> mFinalOutboundPeerData;

    // The current survey phase
    SurveyPhase mPhase = SurveyPhase::INACTIVE;

#ifdef BUILD_TESTS
    // Override maximum phase durations for testing
    std::optional<std::chrono::minutes> mMaxPhaseDurationForTesting =
        std::nullopt;
#endif // BUILD_TESTS

    // Reset survey data. Intended to be called when survey data expires.
    void reset();

    // Transition to the reporting phase. Should only be called from the
    // collecting phase. Returns `false` if transition fails.
    bool
    startReportingPhase(std::map<NodeID, Peer::pointer> const& inboundPeers,
                        std::map<NodeID, Peer::pointer> const& outboundPeers,
                        Config const& config);

    // Function to call when the impossible occurs. Logs an error and resets
    // the survey. Use instead of `releaseAssert` as an overlay survey
    // failure is not important enough to crash the program.
    void emitInconsistencyError(std::string const& where);

    // Finalize node data into `mFinalNodeData`. Should only be called after
    // finalizing peer data.
    void finalizeNodeData(Config const& config);

    // Finalize peer data into `finalPeerData`
    void finalizePeerData(std::map<NodeID, Peer::pointer> const peers,
                          std::unordered_map<NodeID, CollectingPeerData> const&
                              collectingPeerData,
                          std::vector<TimeSlicedPeerData>& finalPeerData);

    // Get the max phase durations for the collecting and reporting phases
    // respectively
    std::chrono::minutes getCollectingPhaseMaxDuration() const;
    std::chrono::minutes getReportingPhaseMaxDuration() const;
};

} // namespace stellar