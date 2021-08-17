// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "overlay/SurveyMessageLimiter.h"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/test.h"

using namespace stellar;

TEST_CASE("messagelimiter", "[overlay][survey][messagelimiter]")
{
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);

    Curve25519Public temp;

    Config cfg(getTestConfig());
    cfg.NODE_SEED = v1SecretKey;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    // we need to pass a lower ledgerNum into the rate limiter to test the
    // window,  so make sure this is not 0
    REQUIRE(app->getHerder().trackingConsensusLedgerIndex() == 1);

    const uint32_t ledgerNumWindow = 0;
    const uint32_t surveyorRequestLimit = 2;
    SurveyMessageLimiter rm(*app, ledgerNumWindow, surveyorRequestLimit);

    auto ledgerNum = app->getHerder().trackingConsensusLedgerIndex();
    SurveyRequestMessage firstRequest(v0SecretKey.getPublicKey(),
                                      v1SecretKey.getPublicKey(), ledgerNum,
                                      temp, SURVEY_TOPOLOGY);
    // Second request with a _different_ surveyor, ensure it's processed
    // correctly
    SurveyRequestMessage secondRequest(v3SecretKey.getPublicKey(),
                                       v1SecretKey.getPublicKey(), ledgerNum,
                                       temp, SURVEY_TOPOLOGY);

    auto success = [&]() -> bool { return true; };

    auto failure = [&]() -> bool { return false; };

    REQUIRE(rm.addAndValidateRequest(firstRequest, success));
    REQUIRE(rm.addAndValidateRequest(secondRequest, success));
    // adding same request again should fail
    REQUIRE(!rm.addAndValidateRequest(firstRequest, success));
    REQUIRE(!rm.addAndValidateRequest(secondRequest, success));

    SECTION("survey request limits are enforced")
    {
        SurveyRequestMessage thirdRequest(v0SecretKey.getPublicKey(),
                                          v2SecretKey.getPublicKey(), ledgerNum,
                                          temp, SURVEY_TOPOLOGY);

        SurveyRequestMessage fourthRequest(v0SecretKey.getPublicKey(),
                                           v3SecretKey.getPublicKey(),
                                           ledgerNum, temp, SURVEY_TOPOLOGY);

        REQUIRE(rm.addAndValidateRequest(thirdRequest, success));
        // Hit the surveyed node limit
        REQUIRE(!rm.addAndValidateRequest(fourthRequest, success));
    }
    SECTION("allow self to start survey when at capacity")
    {
        // Reject other nodes when at capacity
        SurveyRequestMessage otherRequest(v2SecretKey.getPublicKey(),
                                          v1SecretKey.getPublicKey(), ledgerNum,
                                          temp, SURVEY_TOPOLOGY);
        REQUIRE(!rm.addAndValidateRequest(otherRequest, success));

        // Allow self to start survey
        SurveyRequestMessage selfRequest(v1SecretKey.getPublicKey(),
                                         v3SecretKey.getPublicKey(), ledgerNum,
                                         temp, SURVEY_TOPOLOGY);
        REQUIRE(rm.addAndValidateRequest(selfRequest, success));

        // Allow self to survey more nodes
        SurveyRequestMessage selfRequest2(v1SecretKey.getPublicKey(),
                                          v0SecretKey.getPublicKey(), ledgerNum,
                                          temp, SURVEY_TOPOLOGY);
        REQUIRE(rm.addAndValidateRequest(selfRequest2, success));

        SurveyRequestMessage selfRequest3(v1SecretKey.getPublicKey(),
                                          v2SecretKey.getPublicKey(), ledgerNum,
                                          temp, SURVEY_TOPOLOGY);
        REQUIRE(rm.addAndValidateRequest(selfRequest3, success));
    }
    SECTION("receive corresponding response")
    {
        SurveyResponseMessage response(v0SecretKey.getPublicKey(),
                                       v1SecretKey.getPublicKey(), ledgerNum,
                                       SURVEY_TOPOLOGY, 0);

        // validation should fail, so state should not change
        REQUIRE(!rm.recordAndValidateResponse(response, failure));
        REQUIRE(rm.recordAndValidateResponse(response, success));
        // same response again should fail
        REQUIRE(!rm.recordAndValidateResponse(response, success));
    }

    SECTION("corresponding response ledgernum too high")
    {
        SurveyResponseMessage response(v0SecretKey.getPublicKey(),
                                       v1SecretKey.getPublicKey(),
                                       ledgerNum + 2, SURVEY_TOPOLOGY, 0);
        REQUIRE(!rm.recordAndValidateResponse(response, success));
    }

    SECTION("corresponding response ledgernum too low")
    {
        SurveyResponseMessage response(v0SecretKey.getPublicKey(),
                                       v1SecretKey.getPublicKey(),
                                       ledgerNum - 1, SURVEY_TOPOLOGY, 0);
        REQUIRE(!rm.recordAndValidateResponse(response, success));
    }

    SECTION("surveyor receives response")
    {
        SurveyResponseMessage response(v0SecretKey.getPublicKey(),
                                       v1SecretKey.getPublicKey(), ledgerNum,
                                       SURVEY_TOPOLOGY, 0);

        // rate limiter will return false for a response to the requestor if
        // onSuccessValidation fails
        REQUIRE(!rm.recordAndValidateResponse(response, failure));
        REQUIRE(rm.recordAndValidateResponse(response, success));
    }

    SECTION("receive unknown response")
    {
        // different surveyor
        SurveyResponseMessage response(v2SecretKey.getPublicKey(),
                                       v1SecretKey.getPublicKey(), ledgerNum,
                                       SURVEY_TOPOLOGY, 0);
        REQUIRE(!rm.recordAndValidateResponse(response, success));
    }

    SECTION("request ledgernum too high")
    {
        SurveyRequestMessage request(v0SecretKey.getPublicKey(),
                                     v1SecretKey.getPublicKey(), ledgerNum + 2,
                                     temp, SURVEY_TOPOLOGY);
        REQUIRE(!rm.addAndValidateRequest(request, success));
    }

    SECTION("request ledgernum too low")
    {
        SurveyRequestMessage request(v0SecretKey.getPublicKey(),
                                     v1SecretKey.getPublicKey(), ledgerNum - 1,
                                     temp, SURVEY_TOPOLOGY);
        REQUIRE(!rm.addAndValidateRequest(request, success));
    }

    SECTION("hit request limit, clear old ledgers, and send new request")
    {
        // Hit the surveyor node limit
        SurveyRequestMessage request(v2SecretKey.getPublicKey(),
                                     v3SecretKey.getPublicKey(), ledgerNum,
                                     temp, SURVEY_TOPOLOGY);
        REQUIRE(!rm.addAndValidateRequest(request, success));

        rm.clearOldLedgers(ledgerNum + 1);

        ++request.ledgerNum;
        REQUIRE(rm.addAndValidateRequest(request, success));
    }
}