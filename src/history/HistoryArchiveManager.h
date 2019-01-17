#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include <vector>

namespace stellar
{
class Application;
class Config;
class HistoryArchive;

class HistoryArchiveManager
{
  public:
    explicit HistoryArchiveManager(Application& app);

    // Check that config settings are at least somewhat reasonable.
    bool checkSensibleConfig() const;

    // Select any readable history archive. If there are more than one,
    // select one at random.
    std::shared_ptr<HistoryArchive> selectRandomReadableHistoryArchive() const;

    // Initialize a named history archive by writing
    // .well-known/stellar-history.json to it.
    bool initializeHistoryArchive(std::string const& arch) const;

    // Returns whether or not the HistoryManager has any writable history
    // archives (those configured with both a `get` and `put` command).
    bool hasAnyWritableHistoryArchive() const;

    // Returns history archive with given name or nullptr.
    std::shared_ptr<HistoryArchive>
    getHistoryArchive(std::string const& name) const;

    // Returns all writable history archives (those configured with both a `get`
    // and `put` command).
    std::vector<std::shared_ptr<HistoryArchive>>
    getWritableHistoryArchives() const;

    double getFailureRate() const;

  private:
    Application& mApp;
    std::vector<std::shared_ptr<HistoryArchive>> mArchives;
};
}
