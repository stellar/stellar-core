#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Config.h"
#include "util/Logging.h"

namespace stellar
{

class ApplicationEditableVersion;
class Config;

Config const& getTestConfig(uint32_t instanceNumber = 0u,
                            Config::TestDbMode mode = Config::TESTDB_DEFAULT);
int test(int argc, char* const* argv, el::Level logLevel,
         std::vector<std::string> const& metrics);

extern bool force_sqlite;

void for_versions_to(uint32_t to, ApplicationEditableVersion& app,
                     std::function<void(void)> const& f);

void for_versions_from(uint32_t from, ApplicationEditableVersion& app,
                       std::function<void(void)> const& f);

void for_versions_from(std::vector<uint32_t> const& versions,
                       ApplicationEditableVersion& app,
                       std::function<void(void)> const& f);

void for_all_versions(ApplicationEditableVersion& app,
                      std::function<void(void)> const& f);

void for_versions(uint32_t from, uint32_t to, ApplicationEditableVersion& app,
                  std::function<void(void)> const& f);

void for_versions(std::vector<uint32_t> const& versions,
                  ApplicationEditableVersion& app,
                  std::function<void(void)> const& f);
}
