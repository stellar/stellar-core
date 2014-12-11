#include "lib/catch.hpp"
#include "generated/stellar.hh"
#include "fba/Ballot.h"
using namespace stellar;

TEST_CASE("ballot tests", "[ballot]") {

    stellarxdr::Ballot b1;

    b1.baseFee = 10;
    b1.closeTime = 10;
    b1.index = 1;
    hashStr("hello", b1.txSetHash);

    SECTION("compare") {
        stellarxdr::Ballot b3 = b1;
        
        REQUIRE(!ballot::compare(b1, b3));
        REQUIRE(!ballot::compare(b3, b1));
        b3.baseFee++;
        REQUIRE(!ballot::compare(b1, b3));
        REQUIRE(ballot::compare(b3, b1));
        b1.closeTime++;
        REQUIRE(ballot::compare(b1, b3));
        REQUIRE(!ballot::compare(b3, b1));
        b3.txSetHash[0] += 1;
        REQUIRE(!ballot::compare(b1, b3));
        REQUIRE(ballot::compare(b3, b1));

        b1.index++;
        REQUIRE(ballot::compare(b1, b3));
        REQUIRE(!ballot::compare(b3, b1));

    }
    SECTION("compareValue") {
        stellarxdr::Ballot b3 = b1;

        REQUIRE(!ballot::compareValue(b1, b3));
        REQUIRE(!ballot::compareValue(b3, b1));
        b3.baseFee++;
        REQUIRE(!ballot::compareValue(b1, b3));
        REQUIRE(ballot::compareValue(b3, b1));
        b1.closeTime++;
        REQUIRE(ballot::compareValue(b1, b3));
        REQUIRE(!ballot::compareValue(b3, b1));
        b3.txSetHash[0] += 1;
        REQUIRE(!ballot::compareValue(b1, b3));
        REQUIRE(ballot::compareValue(b3, b1));

        b1.index++;
        REQUIRE(!ballot::compareValue(b1, b3));
        REQUIRE(ballot::compareValue(b3, b1));
    }
    SECTION("isCompatible") {
        stellarxdr::Ballot b3=b1;

        REQUIRE(ballot::isCompatible(b1,b3));
        REQUIRE(ballot::isCompatible(b3,b1));
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
