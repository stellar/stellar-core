// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryArchiveManager.h"
#include "history/HistoryArchive.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "historywork/PutHistoryArchiveStateWork.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "work/WorkManager.h"

#include <vector>

namespace stellar
{

HistoryArchiveManager::HistoryArchiveManager(Application& app) : mApp{app}
{
}

bool
HistoryArchiveManager::checkSensibleConfig()
{
    // Check reasonable-ness of history archive definitions
    std::vector<std::string> readOnlyArchives;
    std::vector<std::string> readWriteArchives;
    std::vector<std::string> writeOnlyArchives;
    std::vector<std::string> inertArchives;

    for (auto const& pair : mApp.getConfig().HISTORY)
    {
        if (pair.second->hasGetCmd())
        {
            if (pair.second->hasPutCmd())
            {
                readWriteArchives.push_back(pair.first);
            }
            else
            {
                readOnlyArchives.push_back(pair.first);
            }
        }
        else
        {
            if (pair.second->hasPutCmd())
            {
                writeOnlyArchives.push_back(pair.first);
            }
            else
            {
                inertArchives.push_back(pair.first);
            }
        }
    }

    bool badArchives = false;

    for (auto const& a : inertArchives)
    {
        CLOG(FATAL, "History")
            << "Archive '" << a
            << "' has no 'get' or 'put' command, will not function";
        badArchives = true;
    }

    for (auto const& a : writeOnlyArchives)
    {
        CLOG(FATAL, "History")
            << "Archive '" << a
            << "' has 'put' but no 'get' command, will be unwritable";
        badArchives = true;
    }

    for (auto const& a : readWriteArchives)
    {
        CLOG(INFO, "History")
            << "Archive '" << a
            << "' has 'put' and 'get' commands, will be read and written";
    }

    for (auto const& a : readOnlyArchives)
    {
        CLOG(INFO, "History")
            << "Archive '" << a
            << "' has 'get' command only, will not be written";
    }

    if (readOnlyArchives.empty() && readWriteArchives.empty())
    {
        CLOG(FATAL, "History")
            << "No readable archives configured, catchup will fail.";
        badArchives = true;
    }

    if (readWriteArchives.empty())
    {
        CLOG(WARNING, "History")
            << "No writable archives configured, history will not be written.";
    }

    if (badArchives)
    {
        CLOG(ERROR, "History") << "History archives misconfigured.";
        return false;
    }
    return true;
}

std::shared_ptr<HistoryArchive>
HistoryArchiveManager::selectRandomReadableHistoryArchive()
{
    std::vector<std::pair<std::string, std::shared_ptr<HistoryArchive>>>
        archives;

    // First try for archives that _only_ have a get command; they're
    // archives we're explicitly not publishing to, so likely ones we want.
    for (auto const& pair : mApp.getConfig().HISTORY)
    {
        if (pair.second->hasGetCmd() && !pair.second->hasPutCmd())
        {
            archives.push_back(pair);
        }
    }

    // If we have none of those, accept those with get+put
    if (archives.size() == 0)
    {
        for (auto const& pair : mApp.getConfig().HISTORY)
        {
            if (pair.second->hasGetCmd() && pair.second->hasPutCmd())
            {
                archives.push_back(pair);
            }
        }
    }

    if (archives.size() == 0)
    {
        throw std::runtime_error("No GET-enabled history archive in config");
    }
    else if (archives.size() == 1)
    {
        CLOG(DEBUG, "History")
            << "Fetching from sole readable history archive '"
            << archives[0].first << "'";
        return archives[0].second;
    }
    else
    {
        std::uniform_int_distribution<size_t> dist(0, archives.size() - 1);
        size_t i = dist(gRandomEngine);
        CLOG(DEBUG, "History") << "Fetching from readable history archive #"
                               << i << ", '" << archives[i].first << "'";
        return archives[i].second;
    }
}

bool
HistoryArchiveManager::initializeHistoryArchive(std::string const& arch)
{
    auto const& cfg = mApp.getConfig();
    auto i = cfg.HISTORY.find(arch);
    if (i == cfg.HISTORY.end())
    {
        CLOG(FATAL, "History")
            << "Can't initialize unknown history archive '" << arch << "'";
        return false;
    }

    auto& wm = mApp.getWorkManager();

    // First check that there's no existing HAS in the archive
    HistoryArchiveState existing;
    CLOG(INFO, "History") << "Probing history archive '" << arch
                          << "' for existing state";
    auto getHas = wm.executeWork<GetHistoryArchiveStateWork>(
        "get-history-archive-state", existing, 0, std::chrono::seconds(0),
        i->second, 0);
    if (getHas->getState() == Work::WORK_SUCCESS)
    {
        CLOG(ERROR, "History")
            << "History archive '" << arch << "' already initialized!";
        return false;
    }
    CLOG(INFO, "History") << "History archive '" << arch
                          << "' appears uninitialized";

    HistoryArchiveState has;
    CLOG(INFO, "History") << "Initializing history archive '" << arch << "'";
    has.resolveAllFutures();

    auto putHas = wm.executeWork<PutHistoryArchiveStateWork>(has, i->second);
    if (putHas->getState() == Work::WORK_SUCCESS)
    {
        CLOG(INFO, "History") << "Initialized history archive '" << arch << "'";
        return true;
    }
    else
    {
        CLOG(FATAL, "History")
            << "Failed to initialize history archive '" << arch << "'";
        return false;
    }
}

bool
HistoryArchiveManager::hasAnyWritableHistoryArchive()
{
    auto const& hist = mApp.getConfig().HISTORY;
    for (auto const& pair : hist)
    {
        if (pair.second->hasGetCmd() && pair.second->hasPutCmd())
            return true;
    }
    return false;
}
}
