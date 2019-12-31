// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SurveyMessageLimiter.h"
#include "herder/Herder.h"
#include "main/Application.h"

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

    if (request.surveyorPeerID == mApp.getConfig().NODE_SEED.getPublicKey())
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

    auto& surveyorToSurveyedMap = ledgerIt->second;

    // limit by number of requests for this surveyor. We can only send 1 request
    // per node, so # of requests == # of surveyed nodes
    if (surveyorToSurveyedMap.size() >= mMaxRequestLimit)
    {
        return false;
    }

    auto surveyorIt = surveyorToSurveyedMap.find(request.surveyorPeerID);
    if (surveyorIt == surveyorToSurveyedMap.end())
    {
        if (!onSuccessValidation())
        {
            return false;
        }

        SurveyedMap surveyedMap = {{request.surveyedPeerID, false}};
        surveyorToSurveyedMap.emplace(request.surveyorPeerID, surveyedMap);
        return true;
    }

    auto& surveyedMap = surveyorIt->second;
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

    // Don't filter responses if node is the requestor
    if (response.surveyorPeerID == mApp.getConfig().NODE_SEED.getPublicKey())
    {
        return onSuccessValidation();
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
SurveyMessageLimiter::surveyLedgerNumValid(uint32_t ledgerNum)
{
    uint32_t curLedgerNum = mApp.getHerder().getCurrentLedgerSeq();
    return ledgerNum + mNumLedgersBeforeIgnore >= curLedgerNum &&
           ledgerNum <= curLedgerNum + 1;
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