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

uint32_t constexpr TIME_SLICED_SURVEY_MIN_OVERLAY_PROTOCOL_VERSION = 34;

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

// Extract a reference to the SurveyRequestMessage within a StellarMessage
SurveyRequestMessage const&
extractSurveyRequestMessage(StellarMessage const& msg)
{
    switch (msg.type())
    {
    case SURVEY_REQUEST:
        return msg.signedSurveyRequestMessage().request;
    case TIME_SLICED_SURVEY_REQUEST:
        return msg.signedTimeSlicedSurveyRequestMessage().request.request;
    default:
        releaseAssert(false);
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
SurveyManager::startSurveyReporting(
    SurveyMessageCommandType type,
    std::optional<std::chrono::seconds> surveyDuration)
{
    if (mRunningSurveyReportingPhaseType)
    {
        return false;
    }

    // results are only cleared when we start the NEXT survey so we can query
    // the results  after the survey closes
    mResults.clear();
    mBadResponseNodes.clear();

    // queued peers are only cleared when we start the NEXT survey so we know
    // which peers were in our backlog before we stopped
    mPeersToSurvey.clear();
    mPeersToSurveyQueue = std::queue<NodeID>();

    mRunningSurveyReportingPhaseType =
        std::make_optional<SurveyMessageCommandType>(type);

    mCurve25519SecretKey = curve25519RandomSecret();
    mCurve25519PublicKey = curve25519DerivePublic(mCurve25519SecretKey);

    // Check surveyDuration (should only be set for old style surveys; time
    // sliced surveys use a builtin timeout)
    switch (type)
    {
    case SURVEY_TOPOLOGY:
        if (!surveyDuration.has_value())
        {
            throw std::runtime_error(
                "startSurveyReporting failed: missing survey duration");
        }
        updateOldStyleSurveyExpiration(surveyDuration.value());
        break;
    case TIME_SLICED_SURVEY_TOPOLOGY:
        // Time sliced surveys have a built-in timeout, so one should not be
        // passed in.
        if (surveyDuration.has_value())
        {
            throw std::runtime_error(
                "startSurveyReporting failed: unexpected survey duration");
        }
        break;
    }

    // starts timer
    topOffRequests(type);

    return true;
}

void
SurveyManager::stopSurveyReporting()
{
    // do nothing if survey isn't running in reporting phase
    if (!mRunningSurveyReportingPhaseType)
    {
        return;
    }

    mRunningSurveyReportingPhaseType.reset();
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
SurveyManager::addNodeToRunningSurveyBacklog(
    SurveyMessageCommandType type,
    std::optional<std::chrono::seconds> surveyDuration,
    NodeID const& nodeToSurvey, std::optional<uint32_t> inboundPeersIndex,
    std::optional<uint32_t> outboundPeersIndex)
{
    if (!mRunningSurveyReportingPhaseType ||
        *mRunningSurveyReportingPhaseType != type)
    {
        throw std::runtime_error("addNodeToRunningSurveyBacklog failed");
    }

    addPeerToBacklog(nodeToSurvey);

    switch (type)
    {
    case SURVEY_TOPOLOGY:
        if (!surveyDuration.has_value())
        {
            throw std::runtime_error("addNodeToRunningSurveyBacklog failed: "
                                     "missing survey duration");
        }
        updateOldStyleSurveyExpiration(surveyDuration.value());
        break;
    case TIME_SLICED_SURVEY_TOPOLOGY:
        // Time sliced surveys have a built-in timeout, so one should not be
        // passed in.
        if (surveyDuration.has_value())
        {
            throw std::runtime_error("addNodeToRunningSurveyBacklog failed: "
                                     "unexpected survey duration");
        }

        if (!inboundPeersIndex.has_value() || !outboundPeersIndex.has_value())
        {
            throw std::runtime_error(
                "addNodeToRunningSurveyBacklog failed: missing peer indices");
        }

        mInboundPeerIndices[nodeToSurvey] = inboundPeersIndex.value();
        mOutboundPeerIndices[nodeToSurvey] = outboundPeersIndex.value();
        break;
    }
}

std::optional<SurveyResponseMessage>
SurveyManager::validateSurveyResponse(StellarMessage const& msg,
                                      Peer::pointer peer)
{
    releaseAssert(msg.type() == SURVEY_RESPONSE);
    auto const& signedResponse = msg.signedSurveyResponseMessage();
    auto const& response = signedResponse.response;

    auto onSuccessValidation = [&]() -> bool {
        return dropPeerIfSigInvalid(response.surveyedPeerID,
                                    signedResponse.responseSignature,
                                    xdr::xdr_to_opaque(response), peer);
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

std::optional<SurveyResponseMessage>
SurveyManager::validateTimeSlicedSurveyResponse(StellarMessage const& msg,
                                                Peer::pointer peer)
{
    releaseAssert(msg.type() == TIME_SLICED_SURVEY_RESPONSE);
    auto const& signedResponse = msg.signedTimeSlicedSurveyResponseMessage();
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
    std::optional<SurveyResponseMessage> maybeResponse;
    switch (msg.type())
    {
    case SURVEY_RESPONSE:
        maybeResponse = validateSurveyResponse(msg, peer);
        break;
    case TIME_SLICED_SURVEY_RESPONSE:
        maybeResponse = validateTimeSlicedSurveyResponse(msg, peer);
        break;
    default:
        releaseAssert(false);
    }

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
        if (mRunningSurveyReportingPhaseType &&
            *mRunningSurveyReportingPhaseType == response.commandType)
        {
            try
            {
                xdr::opaque_vec<> opaqueDecrypted = curve25519Decrypt(
                    mCurve25519SecretKey, mCurve25519PublicKey,
                    response.encryptedBody);

                SurveyResponseBody body;
                xdr::xdr_from_opaque(opaqueDecrypted, body);
                switch (msg.type())
                {
                case SURVEY_RESPONSE:
                {
                    processOldStyleTopologyResponse(response.surveyedPeerID,
                                                    body);
                }
                break;
                case TIME_SLICED_SURVEY_RESPONSE:
                {
                    processTimeSlicedTopologyResponse(response.surveyedPeerID,
                                                      body);
                }
                break;
                default:
                    releaseAssert(false);
                }
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
    SurveyRequestMessage const& request = extractSurveyRequestMessage(msg);

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
        bool res;
        switch (msg.type())
        {
        case SURVEY_REQUEST:
            res = dropPeerIfSigInvalid(
                request.surveyorPeerID,
                msg.signedSurveyRequestMessage().requestSignature,
                xdr::xdr_to_opaque(request), peer);
            break;
        case TIME_SLICED_SURVEY_REQUEST:
        {
            SignedTimeSlicedSurveyRequestMessage const& signedRequest =
                msg.signedTimeSlicedSurveyRequestMessage();
            // check nonce
            res = mSurveyDataManager.nonceIsReporting(
                signedRequest.request.nonce);
            if (res)
            {
                // Check signature
                res = dropPeerIfSigInvalid(
                    request.surveyorPeerID, signedRequest.requestSignature,
                    xdr::xdr_to_opaque(signedRequest.request), peer);
            }
        }
        break;
        default:
            releaseAssert(false);
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
        switch (msg.type())
        {
        case SURVEY_REQUEST:
            processOldStyleTopologyRequest(request);
            break;
        case TIME_SLICED_SURVEY_REQUEST:
            processTimeSlicedTopologyRequest(
                msg.signedTimeSlicedSurveyRequestMessage().request);
            break;
        default:
            releaseAssert(false);
        }
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

StellarMessage
SurveyManager::makeOldStyleSurveyRequest(NodeID const& nodeToSurvey) const
{
    StellarMessage newMsg;
    newMsg.type(SURVEY_REQUEST);

    auto& signedRequest = newMsg.signedSurveyRequestMessage();

    auto& request = signedRequest.request;
    populateSurveyRequestMessage(nodeToSurvey, SURVEY_TOPOLOGY, request);
    auto sigBody = xdr::xdr_to_opaque(request);
    signedRequest.requestSignature = mApp.getConfig().NODE_SEED.sign(sigBody);

    return newMsg;
}

void
SurveyManager::sendTopologyRequest(NodeID const& nodeToSurvey)
{
    if (!mRunningSurveyReportingPhaseType.has_value())
    {
        CLOG_ERROR(Overlay, "Tried to send survey request when no survey is "
                            "running in reporting phase");
        return;
    }

    StellarMessage newMsg;
    switch (mRunningSurveyReportingPhaseType.value())
    {
    case SURVEY_TOPOLOGY:
        newMsg = makeOldStyleSurveyRequest(nodeToSurvey);
        break;
    case TIME_SLICED_SURVEY_TOPOLOGY:
    {
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
            return;
        }

        outerRequest.nonce = maybeNonce.value();
        outerRequest.inboundPeersIndex = mInboundPeerIndices.at(nodeToSurvey);
        outerRequest.outboundPeersIndex = mOutboundPeerIndices.at(nodeToSurvey);

        auto sigBody = xdr::xdr_to_opaque(outerRequest);
        signedRequest.requestSignature =
            mApp.getConfig().NODE_SEED.sign(sigBody);
    }
    break;
    default:
        releaseAssert(false);
    }
    // Record the request in message limiter and broadcast
    relayOrProcessRequest(newMsg, nullptr);
}

void
SurveyManager::processOldStyleTopologyResponse(NodeID const& surveyedPeerID,
                                               SurveyResponseBody const& body)
{
    releaseAssert(body.type() == SURVEY_TOPOLOGY_RESPONSE_V0 ||
                  body.type() == SURVEY_TOPOLOGY_RESPONSE_V1);
    auto& peerResults =
        mResults["topology"][KeyUtils::toStrKey(surveyedPeerID)];
    auto populatePeerResults = [&](auto const& topologyBody) {
        auto& inboundResults = peerResults["inboundPeers"];
        auto& outboundResults = peerResults["outboundPeers"];

        peerResults["numTotalInboundPeers"] =
            topologyBody.totalInboundPeerCount;
        peerResults["numTotalOutboundPeers"] =
            topologyBody.totalOutboundPeerCount;

        recordResults(inboundResults, topologyBody.inboundPeers);
        recordResults(outboundResults, topologyBody.outboundPeers);
    };

    bool extendedSurveyType = body.type() == SURVEY_TOPOLOGY_RESPONSE_V1;
    if (extendedSurveyType)
    {
        auto const& topologyBody = body.topologyResponseBodyV1();
        populatePeerResults(topologyBody);
        peerResults["maxInboundPeerCount"] = topologyBody.maxInboundPeerCount;
        peerResults["maxOutboundPeerCount"] = topologyBody.maxOutboundPeerCount;
    }
    else
    {
        auto const& topologyBody = body.topologyResponseBodyV0();
        populatePeerResults(topologyBody);
    }
}

void
SurveyManager::processTimeSlicedTopologyResponse(NodeID const& surveyedPeerID,
                                                 SurveyResponseBody const& body)
{
    releaseAssert(body.type() == SURVEY_TOPOLOGY_RESPONSE_V2);
    auto& peerResults =
        mResults["topology"][KeyUtils::toStrKey(surveyedPeerID)];

    // Fill in node data
    auto const& topologyBody = body.topologyResponseBodyV2();
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
    SurveyRequestMessage const& request, SurveyMessageCommandType type,
    SurveyResponseBody const& body, SurveyResponseMessage& response) const
{
    response.ledgerNum = request.ledgerNum;
    response.surveyorPeerID = request.surveyorPeerID;
    response.surveyedPeerID = mApp.getConfig().NODE_SEED.getPublicKey();
    response.commandType = type;

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
SurveyManager::processOldStyleTopologyRequest(
    SurveyRequestMessage const& request) const
{
    CLOG_TRACE(Overlay, "Responding to Topology request from {}",
               mApp.getConfig().toShortString(request.surveyorPeerID));

    StellarMessage newMsg;
    newMsg.type(SURVEY_RESPONSE);

    auto& signedResponse = newMsg.signedSurveyResponseMessage();

    SurveyResponseBody body;
    body.type(SURVEY_TOPOLOGY_RESPONSE_V1);

    auto& topologyBody = body.topologyResponseBodyV1();

    auto const& randomInboundPeers =
        mApp.getOverlayManager().getRandomInboundAuthenticatedPeers();
    auto const& randomOutboundPeers =
        mApp.getOverlayManager().getRandomOutboundAuthenticatedPeers();

    populatePeerStats(randomInboundPeers, topologyBody.inboundPeers,
                      mApp.getClock().now());

    populatePeerStats(randomOutboundPeers, topologyBody.outboundPeers,
                      mApp.getClock().now());

    topologyBody.totalInboundPeerCount =
        static_cast<uint32_t>(randomInboundPeers.size());
    topologyBody.totalOutboundPeerCount =
        static_cast<uint32_t>(randomOutboundPeers.size());
    topologyBody.maxInboundPeerCount =
        mApp.getConfig().MAX_ADDITIONAL_PEER_CONNECTIONS;
    topologyBody.maxOutboundPeerCount =
        mApp.getConfig().TARGET_PEER_CONNECTIONS;

    auto& response = signedResponse.response;
    if (!populateSurveyResponseMessage(request, SURVEY_TOPOLOGY, body,
                                       response))
    {
        return;
    }

    auto sigBody = xdr::xdr_to_opaque(response);
    signedResponse.responseSignature = mApp.getConfig().NODE_SEED.sign(sigBody);

    broadcast(newMsg);
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
    if (!populateSurveyResponseMessage(
            request.request, TIME_SLICED_SURVEY_TOPOLOGY, body, innerResponse))
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
    uint32_t minOverlayVersion = 0;
    switch (msg.type())
    {
    case SURVEY_REQUEST:
    case SURVEY_RESPONSE:
        // Do nothing. All nodes on the network can understand these messages.
        break;
    case TIME_SLICED_SURVEY_START_COLLECTING:
    case TIME_SLICED_SURVEY_STOP_COLLECTING:
    case TIME_SLICED_SURVEY_REQUEST:
    case TIME_SLICED_SURVEY_RESPONSE:
        // Only send messages to nodes that can understand them.
        minOverlayVersion = TIME_SLICED_SURVEY_MIN_OVERLAY_PROTOCOL_VERSION;
        break;
    default:
        releaseAssert(false);
    }
    mApp.getOverlayManager().broadcastMessage(
        std::make_shared<StellarMessage const>(msg), /*hash*/ std::nullopt,
        minOverlayVersion);
}

void
SurveyManager::populatePeerStats(std::vector<Peer::pointer> const& peers,
                                 PeerStatList& results,
                                 VirtualClock::time_point now) const
{
    size_t resultSize = std::min<size_t>(peers.size(), results.max_size());
    results.reserve(resultSize);

    for (size_t i = 0; i < resultSize; ++i)
    {
        auto& peer = peers[i];
        auto& peerMetrics = peer->getPeerMetrics();

        PeerStats stats;
        stats.id = peer->getPeerID();
        stats.versionStr = peer->getRemoteVersion();
        stats.messagesRead = peerMetrics.mMessageRead;
        stats.messagesWritten = peerMetrics.mMessageWrite;
        stats.bytesRead = peerMetrics.mByteRead;
        stats.bytesWritten = peerMetrics.mByteWrite;

        stats.uniqueFloodBytesRecv = peerMetrics.mUniqueFloodBytesRecv;
        stats.duplicateFloodBytesRecv = peerMetrics.mDuplicateFloodBytesRecv;
        stats.uniqueFetchBytesRecv = peerMetrics.mUniqueFetchBytesRecv;
        stats.duplicateFetchBytesRecv = peerMetrics.mDuplicateFetchBytesRecv;

        stats.uniqueFloodMessageRecv = peerMetrics.mUniqueFloodMessageRecv;
        stats.duplicateFloodMessageRecv =
            peerMetrics.mDuplicateFloodMessageRecv;
        stats.uniqueFetchMessageRecv = peerMetrics.mUniqueFetchMessageRecv;
        stats.duplicateFetchMessageRecv =
            peerMetrics.mDuplicateFetchMessageRecv;

        stats.secondsConnected =
            std::chrono::duration_cast<std::chrono::seconds>(
                now - peerMetrics.mConnectedTime.load())
                .count();

        results.emplace_back(stats);
    }
}

void
SurveyManager::recordResults(Json::Value& jsonResultList,
                             PeerStatList const& peerList) const
{
    for (auto const& peer : peerList)
    {
        jsonResultList.append(peerStatsToJson(peer));
    }
}

void
SurveyManager::clearOldLedgers(uint32_t lastClosedledgerSeq)
{
    mMessageLimiter.clearOldLedgers(lastClosedledgerSeq);
}

Json::Value const&
SurveyManager::getJsonResults()
{
    mResults["surveyInProgress"] = mRunningSurveyReportingPhaseType.has_value();

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
    case SURVEY_REQUEST:
        summary = "SURVEY_REQUEST:";
        commandType = msg.signedSurveyRequestMessage().request.commandType;
        break;
    case SURVEY_RESPONSE:
        summary = "SURVEY_RESPONSE:";
        commandType = msg.signedSurveyResponseMessage().response.commandType;
        break;
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
SurveyManager::topOffRequests(SurveyMessageCommandType type)
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
    while (mRunningSurveyReportingPhaseType &&
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
    auto handler = [weak, type]() {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }

        self->topOffRequests(type);
    };

    // schedule next top off
    mSurveyThrottleTimer->expires_from_now(SURVEY_THROTTLE_TIMEOUT_SEC);
    mSurveyThrottleTimer->async_wait(handler, &VirtualTimer::onFailureNoop);
}

void
SurveyManager::updateOldStyleSurveyExpiration(
    std::chrono::seconds surveyDuration)
{
    // This function should only be called for old style surveys
    releaseAssert(mRunningSurveyReportingPhaseType.value() == SURVEY_TOPOLOGY);
    mSurveyExpirationTime = mApp.getClock().now() + surveyDuration;
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
    if (!mRunningSurveyReportingPhaseType.has_value())
    {
        return true;
    }

    switch (mRunningSurveyReportingPhaseType.value())
    {
    case SURVEY_TOPOLOGY:
        // Survey is finished if the survey duration has passed and there are no
        // remaining peers to survey
        return mApp.getClock().now() > mSurveyExpirationTime &&
               mPeersToSurvey.empty();
    case TIME_SLICED_SURVEY_TOPOLOGY:
    {
        // Survey is finished when reporting phase ends
        std::optional<uint32_t> maybeNonce = mSurveyDataManager.getNonce();
        if (!maybeNonce.has_value())
        {
            return true;
        }
        return !mSurveyDataManager.nonceIsReporting(maybeNonce.value());
    }
    }
}

#ifdef BUILD_TESTS
SurveyDataManager&
SurveyManager::getSurveyDataManagerForTesting()
{
    return mSurveyDataManager;
}
#endif

}
