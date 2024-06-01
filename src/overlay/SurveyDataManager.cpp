// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/SurveyDataManager.h"

#include "crypto/SecretKey.h"
#include "overlay/Peer.h"
#include "util/Logging.h"
#include "util/numeric.h"

#include <Tracy.hpp>
#include <chrono>

using namespace std::chrono_literals;

namespace stellar
{
namespace
{
// Collecting phase is limited to 30 minutes. If 30 minutes pass without
// receiving a StopSurveyCollecting message the `SurveyDataManager` will
// automatically transition to the reporting phase.
constexpr std::chrono::minutes COLLECTING_PHASE_MAX_DURATION{30};

// Reporting phase is limited to 3 hours, after which the
// `SurveyDataManager` will reset all data and transition to the `INACTIVE`
// phase.
constexpr std::chrono::hours REPORTING_PHASE_MAX_DURATION{3};

// Fill a TimeSlicedPeerDataList with elements from `peerData` starting from
// index `idx` and respecting the max size of the TimeSlicedPeerDataList.
TimeSlicedPeerDataList
fillTimeSlicedPeerDataList(std::vector<TimeSlicedPeerData> const& peerData,
                           size_t idx)
{
    TimeSlicedPeerDataList result;
    if (idx >= peerData.size())
    {
        CLOG_DEBUG(Overlay,
                   "fillTimeSlicedPeerDataList: Received request for peer data "
                   "starting from index {}, but the peers list contains only "
                   "{} peers.",
                   idx, peerData.size());
        return result;
    }
    size_t maxEnd = std::min(peerData.size(), idx + result.max_size());
    result.insert(result.end(), peerData.begin() + idx,
                  peerData.begin() + maxEnd);
    return result;
}

// Initialize a map of peer data with the initial metrics from `peers`
void
initializeCollectingPeerData(
    std::map<NodeID, Peer::pointer> const& peers,
    std::unordered_map<NodeID, CollectingPeerData>& peerData)
{
    releaseAssert(peerData.empty());
    for (auto const& [id, peer] : peers)
    {
        // Copy initial peer metrics
        peerData.try_emplace(id, peer->getPeerMetrics());
    }
}

} // namespace

CollectingNodeData::CollectingNodeData(uint64_t initialLostSyncCount,
                                       Application::State initialState)
    : mSCPFirstToSelfLatencyMsHistogram(
          medida::SamplingInterface::SampleType::kSliding)
    , mSCPSelfToOtherLatencyMsHistogram(
          medida::SamplingInterface::SampleType::kSliding)
    , mInitialLostSyncCount(initialLostSyncCount)
    , mInitialState(initialState)
{
}

CollectingPeerData::CollectingPeerData(Peer::PeerMetrics const& peerMetrics)
    : mInitialMessageRead(peerMetrics.mMessageRead)
    , mInitialMessageWrite(peerMetrics.mMessageWrite)
    , mInitialByteRead(peerMetrics.mByteRead)
    , mInitialByteWrite(peerMetrics.mByteWrite)
    , mInitialUniqueFloodBytesRecv(peerMetrics.mUniqueFloodBytesRecv)
    , mInitialDuplicateFloodBytesRecv(peerMetrics.mDuplicateFloodBytesRecv)
    , mInitialUniqueFetchBytesRecv(peerMetrics.mUniqueFetchBytesRecv)
    , mInitialDuplicateFetchBytesRecv(peerMetrics.mDuplicateFetchBytesRecv)
    , mInitialUniqueFloodMessageRecv(peerMetrics.mUniqueFloodMessageRecv)
    , mInitialDuplicateFloodMessageRecv(peerMetrics.mDuplicateFloodMessageRecv)
    , mInitialUniqueFetchMessageRecv(peerMetrics.mUniqueFetchMessageRecv)
    , mInitialDuplicateFetchMessageRecv(peerMetrics.mDuplicateFetchMessageRecv)
    , mLatencyMsHistogram(medida::SamplingInterface::SampleType::kSliding)
{
}

SurveyDataManager::SurveyDataManager(
    std::function<VirtualClock::time_point()> const& getNow,
    medida::Meter const& lostSyncMeter, Config const& cfg)
    : mGetNow(getNow), mLostSyncMeter(lostSyncMeter)
{
#ifdef BUILD_TESTS
    // Override phase durations if set in the config and this build has tests
    // enabled
    std::chrono::minutes maxPhaseDuration =
        cfg.ARTIFICIALLY_SET_SURVEY_PHASE_DURATION_FOR_TESTING;
    if (maxPhaseDuration != 0min)
    {
        setPhaseMaxDurationsForTesting(maxPhaseDuration);
    }
#endif
}

bool
SurveyDataManager::startSurveyCollecting(
    TimeSlicedSurveyStartCollectingMessage const& msg,
    std::map<NodeID, Peer::pointer> const& inboundPeers,
    std::map<NodeID, Peer::pointer> const& outboundPeers,
    Application::State const initialState)
{
    ZoneScoped;

    if (mPhase == SurveyPhase::INACTIVE)
    {
        CLOG_TRACE(Overlay, "Starting survey collecting with nonce {}",
                   msg.nonce);
        mPhase = SurveyPhase::COLLECTING;
        mCollectStartTime = mGetNow();
        mNonce = msg.nonce;
        mSurveyor = msg.surveyorID;
        mCollectingNodeData.emplace(mLostSyncMeter.count(), initialState);
        if (mCollectingInboundPeerData.empty() &&
            mCollectingOutboundPeerData.empty())
        {
            initializeCollectingPeerData(inboundPeers,
                                         mCollectingInboundPeerData);
            initializeCollectingPeerData(outboundPeers,
                                         mCollectingOutboundPeerData);
            return true;
        }

        emitInconsistencyError("startSurveyCollecting");
        return false;
    }

    CLOG_TRACE(Overlay,
               "Ignoring request to start survey collecting with nonce {} "
               "because there is already an active survey",
               msg.nonce);
    return false;
}

bool
SurveyDataManager::startReportingPhase(
    std::map<NodeID, Peer::pointer> const& inboundPeers,
    std::map<NodeID, Peer::pointer> const& outboundPeers, Config const& config)
{
    if (mPhase != SurveyPhase::COLLECTING || !mFinalInboundPeerData.empty() ||
        !mFinalOutboundPeerData.empty())
    {
        emitInconsistencyError("startReportingPhase");
        return false;
    }

    mPhase = SurveyPhase::REPORTING;
    mCollectEndTime = mGetNow();

    // Finalize peer and node data
    finalizePeerData(inboundPeers, mCollectingInboundPeerData,
                     mFinalInboundPeerData);
    finalizePeerData(outboundPeers, mCollectingOutboundPeerData,
                     mFinalOutboundPeerData);
    finalizeNodeData(config);

    // Clear collecting data
    mCollectingInboundPeerData.clear();
    mCollectingOutboundPeerData.clear();

    return true;
}

bool
SurveyDataManager::stopSurveyCollecting(
    TimeSlicedSurveyStopCollectingMessage const& msg,
    std::map<NodeID, Peer::pointer> const& inboundPeers,
    std::map<NodeID, Peer::pointer> const& outboundPeers, Config const& config)
{
    ZoneScoped;

    uint32_t const nonce = msg.nonce;
    if (mPhase == SurveyPhase::COLLECTING && mNonce == nonce &&
        mSurveyor == msg.surveyorID)
    {
        CLOG_TRACE(Overlay, "Stopping survey collecting with nonce {}", nonce);
        return startReportingPhase(inboundPeers, outboundPeers, config);
    }
    CLOG_TRACE(Overlay,
               "Ignoring request to stop survey collecting with nonce {} "
               "because there is no active survey or the nonce does not "
               "match the active survey's nonce",
               nonce);
    return false;
}

void
SurveyDataManager::modifyNodeData(std::function<void(CollectingNodeData&)> f)
{
    ZoneScoped;

    if (mPhase == SurveyPhase::COLLECTING)
    {
        if (mCollectingNodeData.has_value())
        {
            f(mCollectingNodeData.value());
        }
        else
        {
            emitInconsistencyError("modifyNodeData");
        }
    }
}

void
SurveyDataManager::modifyPeerData(Peer const& peer,
                                  std::function<void(CollectingPeerData&)> f)
{
    ZoneScoped;

    if (mPhase == SurveyPhase::COLLECTING)
    {
        auto it = mCollectingInboundPeerData.find(peer.getPeerID());
        if (it != mCollectingInboundPeerData.end())
        {
            f(it->second);
            return;
        }

        it = mCollectingOutboundPeerData.find(peer.getPeerID());
        if (it != mCollectingOutboundPeerData.end())
        {
            f(it->second);
        }
    }
}

void
SurveyDataManager::recordDroppedPeer(Peer const& peer)
{
    ZoneScoped;

    if (mPhase == SurveyPhase::COLLECTING)
    {
        if (mCollectingInboundPeerData.erase(peer.getPeerID()) == 0)
        {
            mCollectingOutboundPeerData.erase(peer.getPeerID());
        }

        if (mCollectingNodeData.has_value())
        {
            ++mCollectingNodeData.value().mDroppedAuthenticatedPeers;
        }
        else
        {
            emitInconsistencyError("recordDroppedPeer");
        }
    }
}

std::optional<uint32_t>
SurveyDataManager::getNonce() const
{
    return mNonce;
}

bool
SurveyDataManager::nonceIsReporting(uint32_t nonce) const
{
    return mPhase == SurveyPhase::REPORTING && mNonce == nonce;
}

bool
SurveyDataManager::fillSurveyData(TimeSlicedSurveyRequestMessage const& request,
                                  TopologyResponseBodyV2& response)
{
    ZoneScoped;

    if (mPhase == SurveyPhase::REPORTING && mNonce == request.nonce &&
        mSurveyor == request.request.surveyorPeerID)
    {
        if (!mFinalNodeData.has_value())
        {
            emitInconsistencyError("getSurveyData");
            return false;
        }

        response.nodeData = mFinalNodeData.value();
        response.inboundPeers = fillTimeSlicedPeerDataList(
            mFinalInboundPeerData,
            static_cast<size_t>(request.inboundPeersIndex));
        response.outboundPeers = fillTimeSlicedPeerDataList(
            mFinalOutboundPeerData,
            static_cast<size_t>(request.outboundPeersIndex));
        return true;
    }
    return false;
}

bool
SurveyDataManager::surveyIsActive() const
{
    return mPhase != SurveyPhase::INACTIVE;
}

#ifdef BUILD_TESTS
void
SurveyDataManager::setPhaseMaxDurationsForTesting(
    std::chrono::minutes maxPhaseDuration)
{
    mMaxPhaseDurationForTesting = maxPhaseDuration;
}
#endif

void
SurveyDataManager::updateSurveyPhase(
    std::map<NodeID, Peer::pointer> const& inboundPeers,
    std::map<NodeID, Peer::pointer> const& outboundPeers, Config const& config)
{
    switch (mPhase)
    {
    case SurveyPhase::COLLECTING:
        if (!mCollectStartTime.has_value() || mCollectEndTime.has_value())
        {
            emitInconsistencyError("updateSurveyPhase");
            return;
        }
        if (mGetNow() >
            mCollectStartTime.value() + getCollectingPhaseMaxDuration())
        {
            CLOG_TRACE(Overlay, "Survey collecting phase has expired. "
                                "Advancing to reporting phase.");
            startReportingPhase(inboundPeers, outboundPeers, config);
        }
        break;
    case SurveyPhase::REPORTING:
        if (!mCollectStartTime.has_value() || !mCollectEndTime.has_value())
        {
            emitInconsistencyError("updateSurveyPhase");
            return;
        }
        if (mGetNow() >
            mCollectEndTime.value() + getReportingPhaseMaxDuration())
        {
            CLOG_TRACE(
                Overlay,
                "Survey reporting phase has expired. Resetting survey data.");
            reset();
        }
        break;
    case SurveyPhase::INACTIVE:
        if (mCollectStartTime.has_value() || mCollectEndTime.has_value())
        {
            emitInconsistencyError("updateSurveyPhase");
            return;
        }
        // Nothing to do
        break;
    }
}

void
SurveyDataManager::reset()
{
    mPhase = SurveyPhase::INACTIVE;
    mCollectStartTime.reset();
    mCollectEndTime.reset();
    mNonce.reset();
    mSurveyor.reset();
    mCollectingNodeData.reset();
    mCollectingInboundPeerData.clear();
    mCollectingOutboundPeerData.clear();
    mFinalNodeData.reset();
    mFinalInboundPeerData.clear();
    mFinalOutboundPeerData.clear();
}

void
SurveyDataManager::emitInconsistencyError(std::string const& where)
{
#ifdef BUILD_TESTS
    // Throw an exception when testing to make the error more visible
    throw std::runtime_error("Encountered inconsistent survey data while "
                             "executing `" +
                             where + "`.");
#endif
    CLOG_ERROR(Overlay,
               "Encountered inconsistent survey data while executing "
               "`{}`. Resetting survey state.",
               where);
    reset();
}

void
SurveyDataManager::finalizeNodeData(Config const& config)
{
    if (mFinalNodeData.has_value() || !mCollectingNodeData.has_value())
    {
        emitInconsistencyError("finalizeNodeData");
        return;
    }

    // Fill in node data
    mFinalNodeData.emplace();
    mFinalNodeData->addedAuthenticatedPeers =
        mCollectingNodeData->mAddedAuthenticatedPeers;
    mFinalNodeData->droppedAuthenticatedPeers =
        mCollectingNodeData->mDroppedAuthenticatedPeers;
    mFinalNodeData->totalInboundPeerCount =
        static_cast<uint32_t>(mFinalInboundPeerData.size());
    mFinalNodeData->totalOutboundPeerCount =
        static_cast<uint32_t>(mFinalOutboundPeerData.size());
    mFinalNodeData->p75SCPFirstToSelfLatencyMs = doubleToClampedUint32(
        mCollectingNodeData->mSCPFirstToSelfLatencyMsHistogram.GetSnapshot()
            .get75thPercentile());
    mFinalNodeData->p75SCPSelfToOtherLatencyMs = doubleToClampedUint32(
        mCollectingNodeData->mSCPSelfToOtherLatencyMsHistogram.GetSnapshot()
            .get75thPercentile());
    mFinalNodeData->lostSyncCount = static_cast<uint32_t>(
        mLostSyncMeter.count() - mCollectingNodeData->mInitialLostSyncCount);
    switch (mCollectingNodeData->mInitialState)
    {
    case Application::APP_ACQUIRING_CONSENSUS_STATE:
    case Application::APP_CATCHING_UP_STATE:
        // Node was out-of-sync at the start of the survey
        ++mFinalNodeData->lostSyncCount;
        break;
    default:
        break;
    }
    mFinalNodeData->isValidator = config.NODE_IS_VALIDATOR;
    mFinalNodeData->maxInboundPeerCount =
        config.MAX_ADDITIONAL_PEER_CONNECTIONS;
    mFinalNodeData->maxOutboundPeerCount = config.TARGET_PEER_CONNECTIONS;

    // Clear collecting data
    mCollectingNodeData.reset();
}

void
SurveyDataManager::finalizePeerData(
    std::map<NodeID, Peer::pointer> const peers,
    std::unordered_map<NodeID, CollectingPeerData> const& collectingPeerData,
    std::vector<TimeSlicedPeerData>& finalPeerData)
{
    for (auto const& [id, peer] : peers)
    {
        auto const it = collectingPeerData.find(id);
        if (it != collectingPeerData.end())
        {
            CollectingPeerData const& collectingData = it->second;
            Peer::PeerMetrics const& peerMetrics = peer->getPeerMetrics();

            TimeSlicedPeerData& finalData = finalPeerData.emplace_back();
            PeerStats& finalStats = finalData.peerStats;

            finalStats.id = id;
            finalStats.versionStr = peer->getRemoteVersion();
            finalStats.messagesRead =
                peerMetrics.mMessageRead - collectingData.mInitialMessageRead;
            finalStats.messagesWritten =
                peerMetrics.mMessageWrite - collectingData.mInitialMessageWrite;
            finalStats.bytesRead =
                peerMetrics.mByteRead - collectingData.mInitialByteRead;
            finalStats.bytesWritten =
                peerMetrics.mByteWrite - collectingData.mInitialByteWrite;
            finalStats.secondsConnected = static_cast<uint64>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    mGetNow() - peerMetrics.mConnectedTime.load())
                    .count());
            finalStats.uniqueFloodBytesRecv =
                peerMetrics.mUniqueFloodBytesRecv -
                collectingData.mInitialUniqueFloodBytesRecv;
            finalStats.duplicateFloodBytesRecv =
                peerMetrics.mDuplicateFloodBytesRecv -
                collectingData.mInitialDuplicateFloodBytesRecv;
            finalStats.uniqueFetchBytesRecv =
                peerMetrics.mUniqueFetchBytesRecv -
                collectingData.mInitialUniqueFetchBytesRecv;
            finalStats.duplicateFetchBytesRecv =
                peerMetrics.mDuplicateFetchBytesRecv -
                collectingData.mInitialDuplicateFetchBytesRecv;
            finalStats.uniqueFloodMessageRecv =
                peerMetrics.mUniqueFloodMessageRecv -
                collectingData.mInitialUniqueFloodMessageRecv;
            finalStats.duplicateFloodMessageRecv =
                peerMetrics.mDuplicateFloodMessageRecv -
                collectingData.mInitialDuplicateFloodMessageRecv;
            finalStats.uniqueFetchMessageRecv =
                peerMetrics.mUniqueFetchMessageRecv -
                collectingData.mInitialUniqueFetchMessageRecv;
            finalStats.duplicateFetchMessageRecv =
                peerMetrics.mDuplicateFetchMessageRecv -
                collectingData.mInitialDuplicateFetchMessageRecv;
            finalData.averageLatencyMs = doubleToClampedUint32(
                collectingData.mLatencyMsHistogram.GetSnapshot().getMedian());
        }
    }
}

std::chrono::minutes
SurveyDataManager::getCollectingPhaseMaxDuration() const
{
#ifdef BUILD_TESTS
    if (mMaxPhaseDurationForTesting.has_value())
    {
        return mMaxPhaseDurationForTesting.value();
    }
#endif
    return std::chrono::duration_cast<std::chrono::minutes>(
        COLLECTING_PHASE_MAX_DURATION);
}

std::chrono::minutes
SurveyDataManager::getReportingPhaseMaxDuration() const
{
#ifdef BUILD_TESTS
    if (mMaxPhaseDurationForTesting.has_value())
    {
        return mMaxPhaseDurationForTesting.value();
    }
#endif
    return std::chrono::duration_cast<std::chrono::minutes>(
        REPORTING_PHASE_MAX_DURATION);
}

} // namespace stellar