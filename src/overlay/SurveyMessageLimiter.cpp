// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SurveyMessageLimiter.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "overlay/SurveyDataManager.h"

namespace stellar
{

SurveyMessageLimiter::SurveyMessageLimiter(Application& app,
                                           uint32_t numLedgersBeforeIgnore,
                                           uint32_t maxRequestLimit)
    : mNumLedgersBeforeIgnore(numLedgersBeforeIgnore)
    , mMaxRequestLimit(maxRequestLimit)
    , mApp(app)
{
}

bool
SurveyMessageLimiter::addAndValidateRequest(
    SurveyRequestMessage const& request,
    std::function<bool()> onSuccessValidation)
{
    if (!surveyLedgerNumValid(request.ledgerNum))
    {
        return false;
    }

    auto ledgerIt = mRecordMap.find(request.ledgerNum);
    if (ledgerIt == mRecordMap.end())
    {
        if (!onSuccessValidation())
        {
            return false;
        }

        SurveyorMap surveyorMap = {
            {request.surveyorPeerID, {{request.surveyedPeerID, false}}}};
        mRecordMap.emplace(request.ledgerNum, surveyorMap);
        return true;
    }

    bool surveyorIsSelf =
        request.surveyorPeerID == mApp.getConfig().NODE_SEED.getPublicKey();
    auto& surveyorToSurveyedMap = ledgerIt->second;
    auto surveyorIt = surveyorToSurveyedMap.find(request.surveyorPeerID);
    if (surveyorIt == surveyorToSurveyedMap.end())
    {
        // The number of unique surveyors is at limit, toss
        // Allow self even if the surveyor map is at capacity
        if (!surveyorIsSelf && surveyorToSurveyedMap.size() >= mMaxRequestLimit)
        {
            return false;
        }

        if (!onSuccessValidation())
        {
            return false;
        }

        SurveyedMap surveyedMap = {{request.surveyedPeerID, false}};
        surveyorToSurveyedMap.emplace(request.surveyorPeerID, surveyedMap);
        return true;
    }

    auto& surveyedMap = surveyorIt->second;

    // limit by number of requests for this surveyor. We can only send 1 request
    // per node, so # of requests == # of surveyed nodes
    if (!surveyorIsSelf && surveyedMap.size() >= mMaxRequestLimit)
    {
        return false;
    }

    auto surveyedIt = surveyedMap.find(request.surveyedPeerID);
    if (surveyedIt == surveyedMap.end())
    {
        if (!onSuccessValidation())
        {
            return false;
        }

        surveyedMap.emplace(request.surveyedPeerID, false);
        return true;
    }

    // request was already seen
    return false;
}

bool
SurveyMessageLimiter::recordAndValidateResponse(
    SurveyResponseMessage const& response,
    std::function<bool()> onSuccessValidation)
{
    if (!surveyLedgerNumValid(response.ledgerNum))
    {
        return false;
    }

    auto ledgerIt = mRecordMap.find(response.ledgerNum);
    if (ledgerIt == mRecordMap.end())
    {
        // request not seen, toss
        return false;
    }

    auto& surveyorToSurveyedMap = ledgerIt->second;
    auto surveyorIt = surveyorToSurveyedMap.find(response.surveyorPeerID);
    if (surveyorIt == surveyorToSurveyedMap.end())
    {
        // request not seen, toss
        return false;
    }

    auto& surveyedMap = surveyorIt->second;
    auto surveyedIt = surveyedMap.find(response.surveyedPeerID);
    if (surveyedIt == surveyedMap.end())
    {
        // request not seen, toss
        return false;
    }

    if (surveyedIt->second)
    {
        // this response was already seen, toss
        return false;
    }

    if (!onSuccessValidation())
    {
        return false;
    }

    // mark response as seen
    surveyedIt->second = true;
    return true;
}

bool
SurveyMessageLimiter::validateStartSurveyCollecting(
    TimeSlicedSurveyStartCollectingMessage const& startSurvey,
    SurveyDataManager& surveyDataManager,
    std::function<bool()> onSuccessValidation)
{
    if (!surveyLedgerNumValid(startSurvey.ledgerNum))
    {
        // Request too old (or otherwise invalid)
        return false;
    }

    if (surveyDataManager.surveyIsActive())
    {
        // A survey already active, toss. Only one survey may be active at a
        // time.
        return false;
    }

    if (!onSuccessValidation())
    {
        return false;
    }

    return true;
}

bool
SurveyMessageLimiter::validateStopSurveyCollecting(
    TimeSlicedSurveyStopCollectingMessage const& stopSurvey,
    std::function<bool()> onSuccessValidation)
{
    if (!surveyLedgerNumValid(stopSurvey.ledgerNum))
    {
        // Request too old (or otherwise invalid)
        return false;
    }

    if (!onSuccessValidation())
    {
        return false;
    }

    return true;
}

bool
SurveyMessageLimiter::surveyLedgerNumValid(uint32_t ledgerNum)
{
    uint32_t localLedgerNum = mApp.getHerder().trackingConsensusLedgerIndex();
    return ledgerNum + mNumLedgersBeforeIgnore >= localLedgerNum &&
           ledgerNum <=
               localLedgerNum + std::max<uint32_t>(mNumLedgersBeforeIgnore, 1);
}

void
SurveyMessageLimiter::clearOldLedgers(uint32_t lastClosedledgerSeq)
{
    for (auto it = mRecordMap.cbegin(); it != mRecordMap.cend();)
    {
        // clean up saved requests
        if (it->first + mNumLedgersBeforeIgnore < lastClosedledgerSeq)
        {
            it = mRecordMap.erase(it);
        }
        else
        {
            break;
        }
    }
}
}