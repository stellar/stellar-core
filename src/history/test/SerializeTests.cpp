// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryArchive.h"
#include "lib/catch.hpp"

#include <fstream>
#include <string>

using namespace stellar;

TEST_CASE("Serialization round trip", "[history][serialize]")
{
    std::vector<std::string> testFiles = {
        "stellar-history.testnet.6714239.json",
        "stellar-history.livenet.15686975.json"};
    for (auto const& fn : testFiles)
    {
        std::string fnPath = "testdata/";
        fnPath += fn;
        SECTION("Serialize " + fnPath)
        {
            std::ifstream in(fnPath);
            std::string fromFile((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());

            HistoryArchiveState has;
            has.fromString(fromFile);
            REQUIRE(fromFile == has.toString());
        }
    }
}
