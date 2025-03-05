// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SurveyManager.h"
#include "crypto/Curve25519.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "main/ErrorMessages.h"
#include "medida/metrics_registry.h"
#include "overlay/OverlayManager.h"
#include "overlay/SurveyDataManager.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"

namespace stellar
{

uint32_t const SurveyManager::SURVEY_THROTTLE_TIMEOUT_MULT(3);

namespace
{
// Generate JSON for a single peer
Json::Value
peerStatsToJson(PeerStats const& peer)
{
    Json::Value peerInfo;
    peerInfo["nodeId"] = KeyUtils::toStrKey(peer.id);
    peerInfo["version"] = peer.versionStr;
    peerInfo["messagesRead"] = static_cast<Json::UInt64>(peer.messagesRead);
    peerInfo["messagesWritten"] =
        static_cast<Json::UInt64>(peer.messagesWritten);
    peerInfo["bytesRead"] = static_cast<Json::UInt64>(peer.bytesRead);
    peerInfo["bytesWritten"] = static_cast<Json::UInt64>(peer.bytesWritten);
    peerInfo["secondsConnected"] =
        static_cast<Json::UInt64>(peer.secondsConnected);

    peerInfo["uniqueFloodBytesRecv"] =
        static_cast<Json::UInt64>(peer.uniqueFloodBytesRecv);
    peerInfo["duplicateFloodBytesRecv"] =
        static_cast<Json::UInt64>(peer.duplicateFloodBytesRecv);
    peerInfo["uniqueFetchBytesRecv"] =
        static_cast<Json::UInt64>(peer.uniqueFetchBytesRecv);
    peerInfo["duplicateFetchBytesRecv"] =
        static_cast<Json::UInt64>(peer.duplicateFetchBytesRecv);

    peerInfo["uniqueFloodMessageRecv"] =
        static_cast<Json::UInt64>(peer.uniqueFloodMessageRecv);
    peerInfo["duplicateFloodMessageRecv"] =
        static_cast<Json::UInt64>(peer.duplicateFloodMessageRecv);
    peerInfo["uniqueFetchMessageRecv"] =
        static_cast<Json::UInt64>(peer.uniqueFetchMessageRecv);
    peerInfo["duplicateFetchMessageRecv"] =
        static_cast<Json::UInt64>(peer.duplicateFetchMessageRecv);
    return peerInfo;
}

// Generate JSON for each peer in `peerList` and append to `jsonResultList`
void
recordTimeSlicedLinkResults(Json::Value& jsonResultList,
                            TimeSlicedPeerDataList const& peerList)
{
    for (auto const& peer : peerList)
    {
        Json::Value peerInfo = peerStatsToJson(peer.peerStats);
        peerInfo["averageLatencyMs"] = peer.averageLatencyMs;
        jsonResultList.append(peerInfo);
    }
}
} // namespace

SurveyManager::SurveyManager(Application& app)
    : mApp(app)
    , mSurveyThrottleTimer(std::make_unique<VirtualTimer>(mApp))
    , NUM_LEDGERS_BEFORE_IGNORE(
          6) // ~30 seconds ahead of or behind the current ledger
    , MAX_REQUEST_LIMIT_PER_LEDGER(10)
    , mMessageLimiter(app, NUM_LEDGERS_BEFORE_IGNORE,
                      MAX_REQUEST_LIMIT_PER_LEDGER)
    , SURVEY_THROTTLE_TIMEOUT_SEC(
          mApp.getConfig().getExpectedLedgerCloseTime() *
          SURVEY_THROTTLE_TIMEOUT_MULT)
    , mSurveyDataManager(
          [this]() { return mApp.getClock().now(); },
          mApp.getMetrics().NewMeter({"scp", "sync", "lost"}, "sync"),
          mApp.getConfig())
{
}

bool
SurveyManager::startSurveyReporting()
{
    if (mRunningSurveyReportingPhase)
    {
        // Survey already reporting
        return false;
    }
    mRunningSurveyReportingPhase = true;

    // results are only cleared when we start the NEXT survey so we can query
    // the results  after the survey closes
    mResults.clear();
    mBadResponseNodes.clear();

    // queued peers are only cleared when we start the NEXT survey so we know
    // which peers were in our backlog before we stopped
    mPeersToSurvey.clear();
    mPeersToSurveyQueue = std::queue<NodeID>();

    mCurve25519SecretKey = curve25519RandomSecret();
    mCurve25519PublicKey = curve25519DerivePublic(mCurve25519SecretKey);

    // starts timer
    topOffRequests();

    return true;
}

void
SurveyManager::stopSurveyReporting()
{
    // do nothing if survey isn't running in reporting phase
    if (!mRunningSurveyReportingPhase)
    {
        return;
    }

    mRunningSurveyReportingPhase = false;
    mSurveyThrottleTimer->cancel();

    clearCurve25519Keys(mCurve25519PublicKey, mCurve25519SecretKey);

    CLOG_INFO(Overlay, "SurveyResults {}", getJsonResults().toStyledString());
}

bool
SurveyManager::broadcastStartSurveyCollecting(uint32_t nonce)
{
    if (mSurveyDataManager.surveyIsActive())
    {
        CLOG_ERROR(
            Overlay,
            "Cannot start survey with nonce {} because another survey is "
            "already active",
            nonce);
        return false;
    }
    StellarMessage newMsg;
    newMsg.type(TIME_SLICED_SURVEY_START_COLLECTING);
    auto& signedStartCollecting =
        newMsg.signedTimeSlicedSurveyStartCollectingMessage();
    auto& startCollecting = signedStartCollecting.startCollecting;

    startCollecting.surveyorID = mApp.getConfig().NODE_SEED.getPublicKey();
    startCollecting.nonce = nonce;
    startCollecting.ledgerNum = mApp.getHerder().trackingConsensusLedgerIndex();

    auto sigBody = xdr::xdr_to_opaque(startCollecting);
    signedStartCollecting.signature = mApp.getConfig().NODE_SEED.sign(sigBody);

    relayStartSurveyCollecting(newMsg, nullptr);
    return true;
}

void
SurveyManager::relayStartSurveyCollecting(StellarMessage const& msg,
                                          Peer::pointer peer)
{
    releaseAssert(msg.type() == TIME_SLICED_SURVEY_START_COLLECTING);
    auto const& signedStartCollecting =
        msg.signedTimeSlicedSurveyStartCollectingMessage();
    auto const& startCollecting = signedStartCollecting.startCollecting;

    auto surveyorIsSelf =
        startCollecting.surveyorID == mApp.getConfig().NODE_SEED.getPublicKey();
    if (!surveyorIsSelf)
    {
        releaseAssert(peer);

        if (!surveyorPermitted(startCollecting.surveyorID))
        {
            return;
        }
    }

    auto onSuccessValidation = [&]() -> bool {
        // Check signature
        return dropPeerIfSigInvalid(startCollecting.surveyorID,
                                    signedStartCollecting.signature,
                                    xdr::xdr_to_opaque(startCollecting), peer);
    };

    if (!mMessageLimiter.validateStartSurveyCollecting(
            startCollecting, mSurveyDataManager, onSuccessValidation))
    {
        return;
    }

    OverlayManager& om = mApp.getOverlayManager();
    if (!mSurveyDataManager.startSurveyCollecting(
            startCollecting, om.getInboundAuthenticatedPeers(),
            om.getOutboundAuthenticatedPeers(), mApp.getState()))
    {
        return;
    }

    if (peer)
    {
        om.recvFloodedMsg(msg, peer);
    }

    broadcast(msg);
}

bool
SurveyManager::broadcastStopSurveyCollecting()
{
    std::optional<uint32_t> maybeNonce = mSurveyDataManager.getNonce();
    if (!maybeNonce.has_value())
    {
        return false;
    }

    StellarMessage newMsg;
    newMsg.type(TIME_SLICED_SURVEY_STOP_COLLECTING);
    auto& signedStopCollecting =
        newMsg.signedTimeSlicedSurveyStopCollectingMessage();
    auto& stopCollecting = signedStopCollecting.stopCollecting;

    stopCollecting.surveyorID = mApp.getConfig().NODE_SEED.getPublicKey();
    stopCollecting.nonce = maybeNonce.value();
    stopCollecting.ledgerNum = mApp.getHerder().trackingConsensusLedgerIndex();

    auto sigBody = xdr::xdr_to_opaque(stopCollecting);
    signedStopCollecting.signature = mApp.getConfig().NODE_SEED.sign(sigBody);

    relayStopSurveyCollecting(newMsg, nullptr);

    return true;
}

void
SurveyManager::relayStopSurveyCollecting(StellarMessage const& msg,
                                         Peer::pointer peer)
{
    releaseAssert(msg.type() == TIME_SLICED_SURVEY_STOP_COLLECTING);
    auto const& signedStopCollecting =
        msg.signedTimeSlicedSurveyStopCollectingMessage();
    auto const& stopCollecting = signedStopCollecting.stopCollecting;

    auto surveyorIsSelf =
        stopCollecting.surveyorID == mApp.getConfig().NODE_SEED.getPublicKey();
    if (!surveyorIsSelf)
    {
        releaseAssert(peer);

        if (!surveyorPermitted(stopCollecting.surveyorID))
        {
            return;
        }
    }

    auto onSuccessValidation = [&]() -> bool {
        // Check signature
        return dropPeerIfSigInvalid(stopCollecting.surveyorID,
                                    signedStopCollecting.signature,
                                    xdr::xdr_to_opaque(stopCollecting), peer);
    };

    if (!mMessageLimiter.validateStopSurveyCollecting(stopCollecting,
                                                      onSuccessValidation))
    {
        return;
    }

    OverlayManager& om = mApp.getOverlayManager();
    if (!mSurveyDataManager.stopSurveyCollecting(
            stopCollecting, om.getInboundAuthenticatedPeers(),
            om.getOutboundAuthenticatedPeers(), mApp.getConfig()))
    {
        return;
    }

    if (peer)
    {
        mApp.getOverlayManager().recvFloodedMsg(msg, peer);
    }

    broadcast(msg);
}

void
SurveyManager::addNodeToRunningSurveyBacklog(NodeID const& nodeToSurvey,
                                             uint32_t inboundPeersIndex,
                                             uint32_t outboundPeersIndex)
{
    if (!mRunningSurveyReportingPhase)
    {
        throw std::runtime_error("addNodeToRunningSurveyBacklog failed");
    }

    addPeerToBacklog(nodeToSurvey);

    mInboundPeerIndices[nodeToSurvey] = inboundPeersIndex;
    mOutboundPeerIndices[nodeToSurvey] = outboundPeersIndex;
}

std::optional<SurveyResponseMessage>
SurveyManager::validateTimeSlicedSurveyResponse(
    SignedTimeSlicedSurveyResponseMessage const& signedResponse,
    Peer::pointer peer)
{
    auto const& response = signedResponse.response.response;

    auto onSuccessValidation = [&]() -> bool {
        // Check nonce
        if (!mSurveyDataManager.nonceIsReporting(signedResponse.response.nonce))
        {
            return false;
        }

        // Check signature
        return dropPeerIfSigInvalid(
            response.surveyedPeerID, signedResponse.responseSignature,
            xdr::xdr_to_opaque(signedResponse.response), peer);
    };

    if (mMessageLimiter.recordAndValidateResponse(response,
                                                  onSuccessValidation))
    {
        return response;
    }
    else
    {
        return std::nullopt;
    }
}

void
SurveyManager::relayOrProcessResponse(StellarMessage const& msg,
                                      Peer::pointer peer)
{
    releaseAssert(msg.type() == TIME_SLICED_SURVEY_RESPONSE);
    auto const& signedResponse = msg.signedTimeSlicedSurveyResponseMessage();
    std::optional<SurveyResponseMessage> maybeResponse =
        validateTimeSlicedSurveyResponse(signedResponse, peer);

    if (!maybeResponse.has_value())
    {
        // Validation failed
        return;
    }

    auto const& response = maybeResponse.value();

    // mMessageLimiter filters out duplicates, so here we are guaranteed
    // to record the message for the first time
    mApp.getOverlayManager().recvFloodedMsg(msg, peer);

    if (response.surveyorPeerID == mApp.getConfig().NODE_SEED.getPublicKey())
    {
        // only process if survey is still running and we haven't seen the
        // response
        if (mRunningSurveyReportingPhase)
        {
            try
            {
                xdr::opaque_vec<> opaqueDecrypted = curve25519Decrypt(
                    mCurve25519SecretKey, mCurve25519PublicKey,
                    response.encryptedBody);

                SurveyResponseBody body;
                xdr::xdr_from_opaque(opaqueDecrypted, body);
                processTimeSlicedTopologyResponse(response.surveyedPeerID,
                                                  body);
            }
            catch (std::exception const& e)
            {
                CLOG_ERROR(Overlay, "processing survey response failed: {}",
                           e.what());

                mBadResponseNodes.emplace(response.surveyedPeerID);
                return;
            }
        }
    }
    else
    {
        // messageLimiter guarantees we only flood the response if we've
        // seen the request
        broadcast(msg);
    }
}

void
SurveyManager::relayOrProcessRequest(StellarMessage const& msg,
                                     Peer::pointer peer)
{
    releaseAssert(msg.type() == TIME_SLICED_SURVEY_REQUEST);
    auto const& signedRequest = msg.signedTimeSlicedSurveyRequestMessage();
    SurveyRequestMessage const& request = signedRequest.request.request;

    auto surveyorIsSelf =
        request.surveyorPeerID == mApp.getConfig().NODE_SEED.getPublicKey();
    if (!surveyorIsSelf)
    {
        releaseAssert(peer);

        if (!surveyorPermitted(request.surveyorPeerID))
        {
            return;
        }
    }

    auto onSuccessValidation = [&]() -> bool {
        // check nonce
        bool res =
            mSurveyDataManager.nonceIsReporting(signedRequest.request.nonce);
        if (res)
        {
            // Check signature
            res = dropPeerIfSigInvalid(
                request.surveyorPeerID, signedRequest.requestSignature,
                xdr::xdr_to_opaque(signedRequest.request), peer);
        }
        if (!res && surveyorIsSelf)
        {
            CLOG_ERROR(Overlay, "Unexpected invalid survey request: {} ",
                       REPORT_INTERNAL_BUG);
        }
        return res;
    };

    if (!mMessageLimiter.addAndValidateRequest(request, onSuccessValidation))
    {
        return;
    }

    if (peer)
    {
        mApp.getOverlayManager().recvFloodedMsg(msg, peer);
    }

    if (request.surveyedPeerID == mApp.getConfig().NODE_SEED.getPublicKey())
    {
        processTimeSlicedTopologyRequest(signedRequest.request);
    }
    else
    {
        broadcast(msg);
    }
}

void
SurveyManager::populateSurveyRequestMessage(NodeID const& nodeToSurvey,
                                            SurveyMessageCommandType type,
                                            SurveyRequestMessage& request) const
{
    request.ledgerNum = mApp.getHerder().trackingConsensusLedgerIndex();
    request.surveyorPeerID = mApp.getConfig().NODE_SEED.getPublicKey();

    request.surveyedPeerID = nodeToSurvey;
    request.encryptionKey = mCurve25519PublicKey;
    request.commandType = type;
}

std::optional<StellarMessage>
SurveyManager::createTimeSlicedSurveyRequest(NodeID const& nodeToSurvey) const
{
    StellarMessage newMsg;
    newMsg.type(TIME_SLICED_SURVEY_REQUEST);

    auto& signedRequest = newMsg.signedTimeSlicedSurveyRequestMessage();
    auto& outerRequest = signedRequest.request;
    auto& innerRequest = outerRequest.request;
    populateSurveyRequestMessage(nodeToSurvey, TIME_SLICED_SURVEY_TOPOLOGY,
                                 innerRequest);

    auto maybeNonce = mSurveyDataManager.getNonce();
    if (!maybeNonce.has_value())
    {
        // Reporting phase has ended. Drop the request.
        return std::nullopt;
    }

    outerRequest.nonce = maybeNonce.value();
    outerRequest.inboundPeersIndex = mInboundPeerIndices.at(nodeToSurvey);
    outerRequest.outboundPeersIndex = mOutboundPeerIndices.at(nodeToSurvey);

    auto sigBody = xdr::xdr_to_opaque(outerRequest);
    signedRequest.requestSignature = mApp.getConfig().NODE_SEED.sign(sigBody);

    return newMsg;
}

void
SurveyManager::sendTopologyRequest(NodeID const& nodeToSurvey)
{
    if (!mRunningSurveyReportingPhase)
    {
        CLOG_ERROR(Overlay, "Tried to send survey request when no survey is "
                            "running in reporting phase");
        return;
    }

    std::optional<StellarMessage> newMsg =
        createTimeSlicedSurveyRequest(nodeToSurvey);

    if (newMsg.has_value())
    {
        // Record the request in message limiter and broadcast
        relayOrProcessRequest(newMsg.value(), nullptr);
    }
}

void
SurveyManager::processTimeSlicedTopologyResponse(NodeID const& surveyedPeerID,
                                                 SurveyResponseBody const& body)
{
    auto& peerResults =
        mResults["topology"][KeyUtils::toStrKey(surveyedPeerID)];

    // SURVEY_TOPOLOGY_RESPONSE_V2 is the only type of survey
    // response remaining in the XDR union for SurveyResponseBody
    TopologyResponseBodyV2 const& topologyBody = body.topologyResponseBodyV2();

    // Fill in node data
    TimeSlicedNodeData const& node = topologyBody.nodeData;
    peerResults["addedAuthenticatedPeers"] = node.addedAuthenticatedPeers;
    peerResults["droppedAuthenticatedPeers"] = node.droppedAuthenticatedPeers;
    peerResults["numTotalInboundPeers"] = node.totalInboundPeerCount;
    peerResults["numTotalOutboundPeers"] = node.totalOutboundPeerCount;
    peerResults["p75SCPFirstToSelfLatencyMs"] = node.p75SCPFirstToSelfLatencyMs;
    peerResults["p75SCPSelfToOtherLatencyMs"] = node.p75SCPSelfToOtherLatencyMs;
    peerResults["lostSyncCount"] = node.lostSyncCount;
    peerResults["isValidator"] = node.isValidator;
    peerResults["maxInboundPeerCount"] = node.maxInboundPeerCount;
    peerResults["maxOutboundPeerCount"] = node.maxOutboundPeerCount;

    // Fill in link data
    auto& inboundResults = peerResults["inboundPeers"];
    auto& outboundResults = peerResults["outboundPeers"];
    recordTimeSlicedLinkResults(inboundResults, topologyBody.inboundPeers);
    recordTimeSlicedLinkResults(outboundResults, topologyBody.outboundPeers);
}

bool
SurveyManager::populateSurveyResponseMessage(
    SurveyRequestMessage const& request, SurveyResponseBody const& body,
    SurveyResponseMessage& response) const
{
    response.ledgerNum = request.ledgerNum;
    response.surveyorPeerID = request.surveyorPeerID;
    response.surveyedPeerID = mApp.getConfig().NODE_SEED.getPublicKey();
    response.commandType = TIME_SLICED_SURVEY_TOPOLOGY;

    try
    {
        response.encryptedBody = curve25519Encrypt<EncryptedBody::max_size()>(
            request.encryptionKey, xdr::xdr_to_opaque(body));
    }
    catch (std::exception const& e)
    {
        CLOG_ERROR(Overlay, "curve25519Encrypt failed: {}", e.what());
        return false;
    }
    return true;
}

void
SurveyManager::processTimeSlicedTopologyRequest(
    TimeSlicedSurveyRequestMessage const& request)
{
    std::string const peerIdStr =
        mApp.getConfig().toShortString(request.request.surveyorPeerID);
    CLOG_TRACE(Overlay, "Responding to Topology request from {}", peerIdStr);

    SurveyResponseBody body;
    body.type(SURVEY_TOPOLOGY_RESPONSE_V2);
    if (!mSurveyDataManager.fillSurveyData(request,
                                           body.topologyResponseBodyV2()))
    {
        // This shouldn't happen because nonce and phase should have already
        // been checked prior to calling this function
        CLOG_ERROR(Overlay,
                   "Failed to respond to TimeSlicedTopology request from {} "
                   "due to unexpected nonce mismatch or survey phase mismatch",
                   peerIdStr);
        return;
    }

    StellarMessage newMsg;
    newMsg.type(TIME_SLICED_SURVEY_RESPONSE);
    auto& signedResponse = newMsg.signedTimeSlicedSurveyResponseMessage();

    auto& outerResponse = signedResponse.response;
    outerResponse.nonce = request.nonce;

    auto& innerResponse = outerResponse.response;
    if (!populateSurveyResponseMessage(request.request, body, innerResponse))
    {
        return;
    }

    auto sigBody = xdr::xdr_to_opaque(outerResponse);
    signedResponse.responseSignature = mApp.getConfig().NODE_SEED.sign(sigBody);

    broadcast(newMsg);
}

void
SurveyManager::broadcast(StellarMessage const& msg) const
{
    mApp.getOverlayManager().broadcastMessage(
        std::make_shared<StellarMessage const>(msg));
}

void
SurveyManager::clearOldLedgers(uint32_t lastClosedledgerSeq)
{
    mMessageLimiter.clearOldLedgers(lastClosedledgerSeq);
}

Json::Value const&
SurveyManager::getJsonResults()
{
    mResults["surveyInProgress"] = mRunningSurveyReportingPhase;

    auto& jsonBacklog = mResults["backlog"];
    jsonBacklog.clear();

    for (auto const& peer : mPeersToSurvey)
    {
        jsonBacklog.append(KeyUtils::toStrKey(peer));
    }

    auto& badResponseNodes = mResults["badResponseNodes"];
    badResponseNodes.clear();

    for (auto const& peer : mBadResponseNodes)
    {
        badResponseNodes.append(KeyUtils::toStrKey(peer));
    }

    return mResults;
}

std::string
SurveyManager::getMsgSummary(StellarMessage const& msg)
{
    std::string summary;
    SurveyMessageCommandType commandType;
    switch (msg.type())
    {
    case TIME_SLICED_SURVEY_REQUEST:
        summary = "TIME_SLICED_SURVEY_REQUEST:";
        commandType = msg.signedTimeSlicedSurveyRequestMessage()
                          .request.request.commandType;
        break;
    case TIME_SLICED_SURVEY_RESPONSE:
        summary = "TIME_SLICED_SURVEY_RESPONSE:";
        commandType = msg.signedTimeSlicedSurveyResponseMessage()
                          .response.response.commandType;
        break;
    case TIME_SLICED_SURVEY_START_COLLECTING:
        return "TIME_SLICED_SURVEY_START_COLLECTING";
    case TIME_SLICED_SURVEY_STOP_COLLECTING:
        return "TIME_SLICED_SURVEY_STOP_COLLECTING";
    default:
        throw std::runtime_error(
            "invalid call of SurveyManager::getMsgSummary");
    }
    return summary + commandTypeName(commandType);
}

void
SurveyManager::topOffRequests()
{
    if (surveyIsFinishedReporting())
    {
        stopSurveyReporting();
        return;
    }

    // we only send up to MAX_REQUEST_LIMIT_PER_LEDGER requests and wait
    // mSurveyThrottleTimeoutSec between topoffs as to reduce the
    // chance of having more than MAX_REQUEST_LIMIT_PER_LEDGER (which is the
    // rate limit) on any node relaying requests on the network (NB: can still
    // happen if some connections get congested)

    uint32_t requestsSentInSchedule = 0;
    while (mRunningSurveyReportingPhase &&
           requestsSentInSchedule < MAX_REQUEST_LIMIT_PER_LEDGER &&
           !mPeersToSurvey.empty())
    {
        if (mPeersToSurveyQueue.empty())
        {
            throw std::runtime_error("mPeersToSurveyQueue unexpectedly empty");
        }
        auto key = mPeersToSurveyQueue.front();
        mPeersToSurvey.erase(key);
        mPeersToSurveyQueue.pop();

        sendTopologyRequest(key);

        ++requestsSentInSchedule;
    }

    std::weak_ptr<SurveyManager> weak = shared_from_this();
    auto handler = [weak]() {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }

        self->topOffRequests();
    };

    // schedule next top off
    mSurveyThrottleTimer->expires_from_now(SURVEY_THROTTLE_TIMEOUT_SEC);
    mSurveyThrottleTimer->async_wait(handler, &VirtualTimer::onFailureNoop);
}

void
SurveyManager::addPeerToBacklog(NodeID const& nodeToSurvey)
{
    // filter conditions-
    // 1. already queued
    // 2. node would survey itself
    // This ensures that mPeersToSurveyQueue doesn't contain any duplicates.
    if (mPeersToSurvey.count(nodeToSurvey) != 0 ||
        nodeToSurvey == mApp.getConfig().NODE_SEED.getPublicKey())
    {
        throw std::runtime_error("addPeerToBacklog failed: Peer is already in "
                                 "the backlog, or peer is self.");
    }

    mBadResponseNodes.erase(nodeToSurvey);

    // we clear the results because it's possible to send and receive
    // multiple requests and responses for a surveyor-surveyed node pair. We
    // expect the user to save any previous results before sending the
    // duplicate requests, so we can just overwrite the previous result
    mResults["topology"][KeyUtils::toStrKey(nodeToSurvey)].clear();

    mPeersToSurvey.emplace(nodeToSurvey);
    mPeersToSurveyQueue.emplace(nodeToSurvey);
}

bool
SurveyManager::dropPeerIfSigInvalid(PublicKey const& key,
                                    Signature const& signature,
                                    ByteSlice const& bin, Peer::pointer peer)
{
    bool success = PubKeyUtils::verifySig(key, signature, bin);

    if (!success && peer)
    {
        // we drop the connection to keep a bad peer from pegging the CPU with
        // signature verification
        peer->sendErrorAndDrop(ERR_MISC, "Survey has invalid signature");
    }
    return success;
}

std::string
SurveyManager::commandTypeName(SurveyMessageCommandType type)
{
    return xdr::xdr_traits<SurveyMessageCommandType>::enum_name(type);
}

bool
SurveyManager::surveyorPermitted(NodeID const& surveyorID) const
{
    auto const& surveyorKeys = mApp.getConfig().SURVEYOR_KEYS;

    if (surveyorKeys.empty())
    {
        auto const& quorumMap = mApp.getHerder().getCurrentlyTrackedQuorum();
        return quorumMap.count(surveyorID) != 0;
    }

    return surveyorKeys.count(surveyorID) != 0;
}

void
SurveyManager::modifyNodeData(std::function<void(CollectingNodeData&)> f)
{
    mSurveyDataManager.modifyNodeData(f);
}

void
SurveyManager::modifyPeerData(Peer const& peer,
                              std::function<void(CollectingPeerData&)> f)
{
    mSurveyDataManager.modifyPeerData(peer, f);
}

void
SurveyManager::recordDroppedPeer(Peer const& peer)
{
    mSurveyDataManager.recordDroppedPeer(peer);
}

void
SurveyManager::updateSurveyPhase(
    std::map<NodeID, Peer::pointer> const& inboundPeers,
    std::map<NodeID, Peer::pointer> const& outboundPeers, Config const& config)
{
    mSurveyDataManager.updateSurveyPhase(inboundPeers, outboundPeers, config);
}

bool
SurveyManager::surveyIsFinishedReporting()
{
    if (!mRunningSurveyReportingPhase)
    {
        return true;
    }

    // Survey is finished when reporting phase ends
    std::optional<uint32_t> maybeNonce = mSurveyDataManager.getNonce();
    if (!maybeNonce.has_value())
    {
        return true;
    }
    return !mSurveyDataManager.nonceIsReporting(maybeNonce.value());
}

#ifdef BUILD_TESTS
SurveyDataManager&
SurveyManager::getSurveyDataManagerForTesting()
{
    return mSurveyDataManager;
}

std::optional<StellarMessage>
SurveyManager::createTimeSlicedSurveyRequestForTesting(
    NodeID const& nodeToSurvey)
{
    mInboundPeerIndices[nodeToSurvey] = 0;
    mOutboundPeerIndices[nodeToSurvey] = 0;
    return createTimeSlicedSurveyRequest(nodeToSurvey);
}
#endif

}
