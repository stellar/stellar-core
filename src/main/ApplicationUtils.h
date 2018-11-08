#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"

namespace stellar
{

class CatchupConfiguration;

int runWithConfig(Config cfg);
void setForceSCPFlag(Config cfg, bool set);
void initializeDatabase(Config cfg);
void httpCommand(std::string const& command, unsigned short port);
void loadXdr(Config cfg, std::string const& bucketFile);
void showOfflineInfo(Config cfg);
int reportLastHistoryCheckpoint(Config cfg, std::string const& outputFile);
void genSeed();
int initializeHistories(Config cfg,
                        std::vector<std::string> const& newHistories);
void writeCatchupInfo(Json::Value const& catchupInfo,
                      std::string const& outputFile);
int catchup(Application::pointer app, CatchupConfiguration cc,
            Json::Value& catchupInfo);
int publish(Application::pointer app);
}
