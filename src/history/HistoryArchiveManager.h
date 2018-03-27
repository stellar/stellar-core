#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>

namespace stellar
{
class Application;
class Config;
class HistoryArchive;

class HistoryArchiveManager
{
  public:
    // Check that config settings are at least somewhat reasonable.
    static bool checkSensibleConfig(Config const& cfg);

    explicit HistoryArchiveManager(Application& app);
    // Select any readable history archive. If there are more than one,
    // select one at random.
    std::shared_ptr<HistoryArchive> selectRandomReadableHistoryArchive();

    // Initialize a named history archive by writing
    // .well-known/stellar-history.json to it.
    bool initializeHistoryArchive(std::string const& arch);

    // Returns whether or not the HistoryManager has any writable history
    // archives (those configured with both a `get` and `put` command).
    bool hasAnyWritableHistoryArchive();

  private:
    Application& mApp;
};
}
