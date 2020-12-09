// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SurveyManager.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"

namespace stellar
{

uint32_t const SurveyManager::SURVEY_THROTTLE_TIMEOUT_MULT(3);

SurveyManager::SurveyManager(Application& app)
    : mApp(app)
    , mSurveyThrottleTimer(std::make_unique<VirtualTimer>(mApp))
    , NUM_LEDGERS_BEFORE_IGNORE(6)
    , MAX_REQUEST_LIMIT_PER_LEDGER(10)
    , mMessageLimiter(app, NUM_LEDGERS_BEFORE_IGNORE,
                      MAX_REQUEST_LIMIT_PER_LEDGER)
    , SURVEY_THROTTLE_TIMEOUT_SEC(
          mApp.getConfig().getExpectedLedgerCloseTime() *
          SURVEY_THROTTLE_TIMEOUT_MULT)
{
}

bool
SurveyManager::startSurvey(SurveyMessageCommandType type,
                           std::chrono::seconds surveyDuration)
{
    if (mRunningSurveyType)
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

    mRunningSurveyType = make_optional<SurveyMessageCommandType>(type);

    mCurve25519SecretKey = curve25519RandomSecret();
    mCurve25519PublicKey = curve25519DerivePublic(mCurve25519SecretKey);

    updateSurveyExpiration(surveyDuration);
    // starts timer
    topOffRequests(type);

    return true;
}

void
SurveyManager::stopSurvey()
{
    // do nothing if survey isn't running
    if (!mRunningSurveyType)
    {
        return;
    }

    mRunningSurveyType.reset();
    mSurveyThrottleTimer->cancel();

    clearCurve25519Keys(mCurve25519PublicKey, mCurve25519SecretKey);

    CLOG_INFO(Overlay, "SurveyResults {}", getJsonResults().toStyledString());
}

void
SurveyManager::addNodeToRunningSurveyBacklog(
    SurveyMessageCommandType type, std::chrono::seconds surveyDuration,
    NodeID const& nodeToSurvey)
{
    if (!mRunningSurveyType || *mRunningSurveyType != type)
    {
        throw std::runtime_error("addNodeToRunningSurveyBacklog failed");
    }

    addPeerToBacklog(nodeToSurvey);
    updateSurveyExpiration(surveyDuration);
}

void
SurveyManager::relayOrProcessResponse(StellarMessage const& msg,
                                      Peer::pointer peer)
{
    assert(msg.type() == SURVEY_RESPONSE);
    auto const& signedResponse = msg.signedSurveyResponseMessage();
    auto const& response = signedResponse.response;

    auto onSuccessValidation = [&]() -> bool {
        return dropPeerIfSigInvalid(response.surveyedPeerID,
                                    signedResponse.responseSignature,
                                    xdr::xdr_to_opaque(response), peer);
    };

    if (!mMessageLimiter.recordAndValidateResponse(response,
                                                   onSuccessValidation))
    {
        return;
    }

    // mMessageLimiter doesn't filter out responses for the requesting node, so
    // it's possible for this message to be a duplicate.
    if (!mApp.getOverlayManager().recvFloodedMsg(msg, peer))
    {
        return;
    }

    if (response.surveyorPeerID == mApp.getConfig().NODE_SEED.getPublicKey())
    {
        // only process if survey is still running and we haven't seen the
        // response
        if (mRunningSurveyType && *mRunningSurveyType == response.commandType)
        {
            try
            {
                xdr::opaque_vec<> opaqueDecrypted = curve25519Decrypt(
                    mCurve25519SecretKey, mCurve25519PublicKey,
                    response.encryptedBody);

                SurveyResponseBody body;
                xdr::xdr_from_opaque(opaqueDecrypted, body);

                processTopologyResponse(response.surveyedPeerID, body);
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
        // messageLimiter guarantees we only flood the response if we've seen
        // the request
        broadcast(msg);
    }
}

void
SurveyManager::relayOrProcessRequest(StellarMessage const& msg,
                                     Peer::pointer peer)
{
    assert(msg.type() == SURVEY_REQUEST);
    SignedSurveyRequestMessage const& signedRequest =
        msg.signedSurveyRequestMessage();

    SurveyRequestMessage const& request = signedRequest.request;

    // perform all validation checks before signature validation so we don't
    // waste time verifying signatures
    auto const& surveyorKeys = mApp.getConfig().SURVEYOR_KEYS;
    if (surveyorKeys.empty())
    {
        auto const& quorumMap = mApp.getHerder().getCurrentlyTrackedQuorum();
        if (quorumMap.count(request.surveyorPeerID) == 0)
        {
            return;
        }
    }
    else
    {
        if (surveyorKeys.count(request.surveyorPeerID) == 0)
        {
            return;
        }
    }

    auto onSuccessValidation = [&]() -> bool {
        return dropPeerIfSigInvalid(request.surveyorPeerID,
                                    signedRequest.requestSignature,
                                    xdr::xdr_to_opaque(request), peer);
    };

    if (!mMessageLimiter.addAndValidateRequest(request, onSuccessValidation))
    {
        return;
    }

    mApp.getOverlayManager().recvFloodedMsg(msg, peer);

    if (request.surveyedPeerID == mApp.getConfig().NODE_SEED.getPublicKey())
    {
        processTopologyRequest(request);
    }
    else
    {
        broadcast(msg);
    }
}

void
SurveyManager::sendTopologyRequest(NodeID const& nodeToSurvey) const
{
    StellarMessage newMsg;
    newMsg.type(SURVEY_REQUEST);

    auto& signedRequest = newMsg.signedSurveyRequestMessage();

    auto& request = signedRequest.request;
    request.ledgerNum = mApp.getHerder().getCurrentLedgerSeq();
    request.surveyorPeerID = mApp.getConfig().NODE_SEED.getPublicKey();

    request.surveyedPeerID = nodeToSurvey;
    request.encryptionKey = mCurve25519PublicKey;
    request.commandType = SURVEY_TOPOLOGY;

    auto sigBody = xdr::xdr_to_opaque(request);
    signedRequest.requestSignature = mApp.getConfig().NODE_SEED.sign(sigBody);

    broadcast(newMsg);
}

void
SurveyManager::processTopologyResponse(NodeID const& surveyedPeerID,
                                       SurveyResponseBody const& body)
{
    assert(body.type() == SURVEY_TOPOLOGY);

    auto const& topologyBody = body.topologyResponseBody();
    auto& peerResults =
        mResults["topology"][KeyUtils::toStrKey(surveyedPeerID)];

    auto& inboundResults = peerResults["inboundPeers"];
    auto& outboundResults = peerResults["outboundPeers"];

    peerResults["numTotalInboundPeers"] = topologyBody.totalInboundPeerCount;
    peerResults["numTotalOutboundPeers"] = topologyBody.totalOutboundPeerCount;

    recordResults(inboundResults, topologyBody.inboundPeers);
    recordResults(outboundResults, topologyBody.outboundPeers);
}

void
SurveyManager::processTopologyRequest(SurveyRequestMessage const& request) const
{
    CLOG_TRACE(Overlay, "Responding to Topology request from {}",
               mApp.getConfig().toShortString(request.surveyorPeerID));

    StellarMessage newMsg;
    newMsg.type(SURVEY_RESPONSE);

    auto& signedResponse = newMsg.signedSurveyResponseMessage();
    auto& response = signedResponse.response;

    response.ledgerNum = request.ledgerNum;
    response.surveyorPeerID = request.surveyorPeerID;
    response.surveyedPeerID = mApp.getConfig().NODE_SEED.getPublicKey();
    response.commandType = SURVEY_TOPOLOGY;

    SurveyResponseBody body;
    body.type(SURVEY_TOPOLOGY);

    auto& topologyBody = body.topologyResponseBody();

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

    try
    {
        response.encryptedBody = curve25519Encrypt<EncryptedBody::max_size()>(
            request.encryptionKey, xdr::xdr_to_opaque(body));
    }
    catch (std::exception const& e)
    {
        CLOG_ERROR(Overlay, "curve25519Encrypt failed: {}", e.what());
        return;
    }

    auto sigBody = xdr::xdr_to_opaque(response);
    signedResponse.responseSignature = mApp.getConfig().NODE_SEED.sign(sigBody);

    broadcast(newMsg);
}

void
SurveyManager::broadcast(StellarMessage const& msg) const
{
    mApp.getOverlayManager().broadcastMessage(msg, false);
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
                now - peerMetrics.mConnectedTime)
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

        jsonResultList.append(peerInfo);
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
    mResults["surveyInProgress"] = mRunningSurveyType != nullptr;

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
    default:
        throw std::runtime_error(
            "invalid call of SurveyManager::getMsgSummary");
    }
    return summary + commandTypeName(commandType);
}

void
SurveyManager::topOffRequests(SurveyMessageCommandType type)
{
    if (mApp.getClock().now() > mSurveyExpirationTime)
    {
        stopSurvey();
        return;
    }

    // we only send up to MAX_REQUEST_LIMIT_PER_LEDGER requests and wait
    // mSurveyThrottleTimeoutSec between topoffs as to reduce the
    // chance of having more than MAX_REQUEST_LIMIT_PER_LEDGER (which is the
    // rate limit) on any node relaying requests on the network (NB: can still
    // happen if some connections get congested)

    uint32_t requestsSentInSchedule = 0;
    while (mRunningSurveyType &&
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
SurveyManager::updateSurveyExpiration(std::chrono::seconds surveyDuration)
{
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
        return;
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

    if (!success)
    {
        // we drop the connection to keep a bad peer from pegging the CPU with
        // signature verification
        peer->sendErrorAndDrop(ERR_MISC, "Survey has invalid signature",
                               Peer::DropMode::IGNORE_WRITE_QUEUE);
    }
    return success;
}

std::string
SurveyManager::commandTypeName(SurveyMessageCommandType type)
{
    switch (type)
    {
    case SURVEY_TOPOLOGY:
        return "SURVEY_TOPOLOGY";
    default:
        throw std::runtime_error(fmt::format("Unknown commandType={}", type));
    }
}
}
