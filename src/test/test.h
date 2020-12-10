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

Config const& getTestConfig(int instanceNumber = 0,
                            Config::TestDbMode mode = Config::TESTDB_DEFAULT);

int runTest(CommandLineArgs const& args);

extern int gBaseInstance;
extern bool force_sqlite;

void for_versions_to(uint32 to, Application& app,
                     std::function<void(void)> const& f);

void for_versions_from(uint32 from, Application& app,
                       std::function<void(void)> const& f);

void for_versions_from(std::vector<uint32> const& versions, Application& app,
                       std::function<void(void)> const& f);

void for_all_versions(Application& app, std::function<void(void)> const& f);

void for_all_versions(Config const& cfg,
                      std::function<void(Config const&)> const& f);

void for_versions(uint32 from, uint32 to, Application& app,
                  std::function<void(void)> const& f);

void for_versions(std::vector<uint32> const& versions, Application& app,
                  std::function<void(void)> const& f);

void for_versions(uint32 from, uint32 to, Config const& cfg,
                  std::function<void(Config const&)> const& f);

void for_versions(std::vector<uint32> const& versions, Config const& cfg,
                  std::function<void(Config const&)> const& f);

void for_all_versions_except(std::vector<uint32> const& versions,
                             Application& app,
                             std::function<void(void)> const& f);
}
