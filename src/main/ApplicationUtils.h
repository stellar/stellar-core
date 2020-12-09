#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryArchive.h"
#include "main/Application.h"

namespace stellar
{

class CatchupConfiguration;

int runWithConfig(Config cfg, optional<CatchupConfiguration> cc);
void setForceSCPFlag();
void initializeDatabase(Config cfg);
void httpCommand(std::string const& command, unsigned short port);
int selfCheck(Config cfg);
void showOfflineInfo(Config cfg);
int reportLastHistoryCheckpoint(Config cfg, std::string const& outputFile);
#ifdef BUILD_TESTS
void loadXdr(Config cfg, std::string const& bucketFile);
int rebuildLedgerFromBuckets(Config cfg);
#endif
void genSeed();
int initializeHistories(Config cfg,
                        std::vector<std::string> const& newHistories);
void writeCatchupInfo(Json::Value const& catchupInfo,
                      std::string const& outputFile);
int catchup(Application::pointer app, CatchupConfiguration cc,
            Json::Value& catchupInfo, std::shared_ptr<HistoryArchive> archive);
int publish(Application::pointer app);
}
