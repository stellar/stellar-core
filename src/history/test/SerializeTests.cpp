// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryArchive.h"
#include "lib/catch.hpp"

#include <fstream>
#include <string>

using namespace stellar;

TEST_CASE("Serialization round trip", "[history]")
{
    std::vector<std::string> testFiles = {
        "stellar-history.testnet.6714239.json",
        "stellar-history.livenet.15686975.json",
        "stellar-history.testnet.6714239.networkPassphrase.json"};
    for (int i = 0; i < testFiles.size(); i++)
    {
        std::string fnPath = "testdata/";
        std::string testFilePath = fnPath + testFiles[i];
        SECTION("Serialize " + testFilePath)
        {
            std::ifstream in(testFilePath);
            REQUIRE(in);
            in.exceptions(std::ios::badbit);
            std::string hasString((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());

            // Test fromString
            HistoryArchiveState has;
            has.fromString(hasString);
            REQUIRE(hasString == has.toString());

            // Test load
            HistoryArchiveState hasLoad;
            hasLoad.load(testFilePath);
            REQUIRE(hasString == hasLoad.toString());
        }
    }
}
