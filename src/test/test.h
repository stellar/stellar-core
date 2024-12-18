#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/CommandLine.h"
#include "main/Config.h"
#include "util/Logging.h"

namespace stellar
{

class Application;
class Config;
struct CommandLineArgs;
struct TransactionMeta;

Config const& getTestConfig(int instanceNumber = 0,
                            Config::TestDbMode mode = Config::TESTDB_DEFAULT);

void cleanupTmpDirs();

// Records or checks a TxMetadata value against a persistent record
// of metadata hashes. Each unit-test name and section has a separate
// vector of TxMetadata hashes, containing all the txs in that
// test-and-section.
void recordOrCheckGlobalTestTxMetadata(TransactionMeta const& txMeta);

int runTest(CommandLineArgs const& args);

extern int gBaseInstance;
extern bool force_sqlite;

void test_versions_wrapper(std::function<void(void)> f);

#define TEST_BODY_NAME_INT2(line) testInternalBody##line
#define TEST_BODY_NAME_INT(line) TEST_BODY_NAME_INT2(line)
#define TEST_BODY_NAME TEST_BODY_NAME_INT(__LINE__)

#define TEST_CASE_VERSIONS(testname, filters) \
    static void TEST_BODY_NAME(); \
    TEST_CASE(testname, filters) \
    { \
        test_versions_wrapper([]() { TEST_BODY_NAME(); }); \
    } \
    static void TEST_BODY_NAME()

void for_versions_to(uint32 to, Application& app,
                     std::function<void(void)> const& f);

void for_versions_from(uint32 from, Application& app,
                       std::function<void(void)> const& f);

void for_versions_from(std::vector<uint32> const& versions, Application& app,
                       std::function<void(void)> const& f);

void for_versions_from(uint32 from, Config const& cfg,
                       std::function<void(Config const&)> const& f);

void for_all_versions(Application& app, std::function<void(void)> const& f);

void for_all_versions(Config const& cfg,
                      std::function<void(Config const&)> const& f);

void for_versions(uint32 from, uint32 to, Application& app,
                  std::function<void(void)> const& f);

void for_versions(std::vector<uint32> const& versions, Application& app,
                  std::function<void(void)> const& f);

void for_versions(uint32 from, uint32 to, Config const& cfg,
                  std::function<void(Config const&)> const& f);

void for_versions(uint32 from, uint32 to, Config const& cfg,
                  std::function<void(Config&)> const& f);

void for_versions(std::vector<uint32> const& versions, Config const& cfg,
                  std::function<void(Config const&)> const& f);

void for_versions(std::vector<uint32> const& versions, Config const& cfg,
                  std::function<void(Config&)> const& f);

void for_all_versions_except(std::vector<uint32> const& versions,
                             Application& app,
                             std::function<void(void)> const& f);
}
