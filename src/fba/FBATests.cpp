#include "lib/catch.hpp"
#include "generated/StellarXDR.h"
#include "fba/Ballot.h"
#include "main/test.h"
#include "main/Application.h"
using namespace stellar;

// addStatement when we already have it
// checkRatState?
//   we need an application
TEST_CASE("node tests", "[fba]")
{

    SECTION("simple")
    {
        stellarxdr::uint256 nodeID;
        Node testNode(nodeID);

        stellarxdr::FBAEnvelope sxdr1;
        sxdr1.nodeID = nodeID;
        sxdr1.contents.body.type(stellarxdr::FBAStatementType::PREPARE);
        sxdr1.contents.slotBallot.ballot.baseFee = 5;
        StatementPtr s1 = std::make_shared<Statement>(sxdr1);

        REQUIRE(!testNode.hasStatement(s1));
        REQUIRE(!testNode.getHighestStatement(
            stellarxdr::FBAStatementType::PREPARE));
        REQUIRE(!testNode.getHighestStatement(
            stellarxdr::FBAStatementType::PREPARED));
        REQUIRE(!testNode.getHighestStatement(
            stellarxdr::FBAStatementType::COMMIT));
        REQUIRE(!testNode.getHighestStatement(
            stellarxdr::FBAStatementType::COMMITTED));

        REQUIRE(testNode.addStatement(s1));
        REQUIRE(testNode.hasStatement(s1));

        REQUIRE(testNode.getHighestStatement(
                    stellarxdr::FBAStatementType::PREPARE) == s1);
        REQUIRE(!testNode.getHighestStatement(
            stellarxdr::FBAStatementType::PREPARED));
        REQUIRE(!testNode.getHighestStatement(
            stellarxdr::FBAStatementType::COMMIT));
        REQUIRE(!testNode.getHighestStatement(
            stellarxdr::FBAStatementType::COMMITTED));

        sxdr1.contents.slotBallot.ballot.baseFee = 10;
        StatementPtr s2 = std::make_shared<Statement>(sxdr1);
        REQUIRE(testNode.addStatement(s2));
        REQUIRE(testNode.hasStatement(s1));
        REQUIRE(testNode.hasStatement(s2));
        REQUIRE(testNode.getHighestStatement(
                    stellarxdr::FBAStatementType::PREPARE) == s2);
        REQUIRE(!testNode.getHighestStatement(
            stellarxdr::FBAStatementType::PREPARED));
        REQUIRE(!testNode.getHighestStatement(
            stellarxdr::FBAStatementType::COMMIT));
        REQUIRE(!testNode.getHighestStatement(
            stellarxdr::FBAStatementType::COMMITTED));

        sxdr1.contents.slotBallot.ballot.baseFee = 7;
        StatementPtr s3 = std::make_shared<Statement>(sxdr1);
        REQUIRE(testNode.addStatement(s3));
        REQUIRE(testNode.hasStatement(s1));
        REQUIRE(testNode.hasStatement(s2));
        REQUIRE(testNode.hasStatement(s3));
        REQUIRE(testNode.getHighestStatement(
                    stellarxdr::FBAStatementType::PREPARE) == s2);
        REQUIRE(!testNode.getHighestStatement(
            stellarxdr::FBAStatementType::PREPARED));
        REQUIRE(!testNode.getHighestStatement(
            stellarxdr::FBAStatementType::COMMIT));
        REQUIRE(!testNode.getHighestStatement(
            stellarxdr::FBAStatementType::COMMITTED));

        sxdr1.contents.body.type(stellarxdr::FBAStatementType::PREPARED);
        StatementPtr s4 = std::make_shared<Statement>(sxdr1);
        REQUIRE(testNode.addStatement(s4));
        REQUIRE(testNode.hasStatement(s1));
        REQUIRE(testNode.hasStatement(s2));
        REQUIRE(testNode.hasStatement(s3));
        REQUIRE(testNode.hasStatement(s4));
        REQUIRE(testNode.getHighestStatement(
                    stellarxdr::FBAStatementType::PREPARE) == s2);
        REQUIRE(testNode.getHighestStatement(
                    stellarxdr::FBAStatementType::PREPARED) == s4);
        REQUIRE(!testNode.getHighestStatement(
            stellarxdr::FBAStatementType::COMMIT));
        REQUIRE(!testNode.getHighestStatement(
            stellarxdr::FBAStatementType::COMMITTED));
    }
    SECTION("checkRatState")
    {

        Config const& cfg = getTestConfig();
        Application app(cfg);

        stellarxdr::uint256 nodeID;
        Node testNode(nodeID);
        BallotPtr ballot = std::make_shared<stellarxdr::Ballot>();

        REQUIRE(Node::NOTPLEDGING_STATE ==
                testNode.checkRatState(stellarxdr::FBAStatementType::PREPARE,
                                       ballot, 1, 1, app));
    }
}

TEST_CASE("ballot tests", "[fba]")
{

    stellarxdr::Ballot b1;

    b1.baseFee = 10;
    b1.closeTime = 10;
    b1.index = 1;
    hashStr("hello", b1.txSetHash);

    SECTION("compare")
    {
        stellarxdr::Ballot b3 = b1;

        REQUIRE(!ballot::compare(b1, b3));
        REQUIRE(!ballot::compare(b3, b1));
        REQUIRE(!ballot::compareValue(b1, b3));
        REQUIRE(!ballot::compareValue(b3, b1));
        b3.baseFee++;
        REQUIRE(!ballot::compare(b1, b3));
        REQUIRE(ballot::compare(b3, b1));
        REQUIRE(!ballot::compareValue(b1, b3));
        REQUIRE(ballot::compareValue(b3, b1));
        b1.closeTime++;
        REQUIRE(ballot::compare(b1, b3));
        REQUIRE(!ballot::compare(b3, b1));
        REQUIRE(ballot::compareValue(b1, b3));
        REQUIRE(!ballot::compareValue(b3, b1));
        b3.txSetHash[0] += 1;
        REQUIRE(!ballot::compare(b1, b3));
        REQUIRE(ballot::compare(b3, b1));
        REQUIRE(!ballot::compareValue(b1, b3));
        REQUIRE(ballot::compareValue(b3, b1));

        b1.index++;
        REQUIRE(ballot::compare(b1, b3));
        REQUIRE(!ballot::compare(b3, b1));
        REQUIRE(!ballot::compareValue(b1, b3));
        REQUIRE(ballot::compareValue(b3, b1));
    }
    SECTION("isCompatible")
    {
        stellarxdr::Ballot b3 = b1;

        REQUIRE(ballot::isCompatible(b1, b3));
        REQUIRE(ballot::isCompatible(b3, b1));
        b3.index++;
        REQUIRE(ballot::isCompatible(b1, b3));
        REQUIRE(ballot::isCompatible(b3, b1));
        b3.baseFee++;
        REQUIRE(!ballot::isCompatible(b1, b3));
        b3.baseFee--;
        b3.closeTime++;
        REQUIRE(!ballot::isCompatible(b1, b3));
        b3.closeTime--;
        b3.txSetHash[0] += 1;
        REQUIRE(!ballot::isCompatible(b1, b3));
    }
}
