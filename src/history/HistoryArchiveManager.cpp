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
    for (auto const& archiveConfiguration : mApp.getConfig().HISTORY)
        mArchives.push_back(
            std::make_shared<HistoryArchive>(app, archiveConfiguration.second));
}

bool
HistoryArchiveManager::checkSensibleConfig() const
{
    // Check reasonable-ness of history archive definitions
    std::vector<std::string> readOnlyArchives;
    std::vector<std::string> readWriteArchives;
    std::vector<std::string> writeOnlyArchives;
    std::vector<std::string> inertArchives;

    for (auto const& archive : mArchives)
    {
        if (archive->hasGetCmd())
        {
            if (archive->hasPutCmd())
            {
                readWriteArchives.push_back(archive->getName());
            }
            else
            {
                readOnlyArchives.push_back(archive->getName());
            }
        }
        else
        {
            if (archive->hasPutCmd())
            {
                writeOnlyArchives.push_back(archive->getName());
            }
            else
            {
                inertArchives.push_back(archive->getName());
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
HistoryArchiveManager::selectRandomReadableHistoryArchive() const
{
    std::vector<std::shared_ptr<HistoryArchive>> archives;

    // First try for archives that _only_ have a get command; they're
    // archives we're explicitly not publishing to, so likely ones we want.
    std::copy_if(std::begin(mArchives), std::end(mArchives),
                 std::back_inserter(archives),
                 [](std::shared_ptr<HistoryArchive> const& x) {
                     return x->hasGetCmd() && !x->hasPutCmd();
                 });

    // If we have none of those, accept those with get+put
    if (archives.size() == 0)
    {
        std::copy_if(std::begin(mArchives), std::end(mArchives),
                     std::back_inserter(archives),
                     [](std::shared_ptr<HistoryArchive> const& x) {
                         return x->hasGetCmd();
                     });
    }

    if (archives.size() == 0)
    {
        throw std::runtime_error("No GET-enabled history archive in config");
    }
    else if (archives.size() == 1)
    {
        CLOG(DEBUG, "History")
            << "Fetching from sole readable history archive '"
            << archives[0]->getName() << "'";
        return archives[0];
    }
    else
    {
        std::uniform_int_distribution<size_t> dist(0, archives.size() - 1);
        size_t i = dist(gRandomEngine);
        CLOG(DEBUG, "History") << "Fetching from readable history archive #"
                               << i << ", '" << archives[i]->getName() << "'";
        return archives[i];
    }
}

bool
HistoryArchiveManager::initializeHistoryArchive(std::string const& arch) const
{
    auto archive = getHistoryArchive(arch);
    if (!archive)
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
        "get-history-archive-state", existing, 0, archive, 0);
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

    auto putHas = wm.executeWork<PutHistoryArchiveStateWork>(has, archive);
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
HistoryArchiveManager::hasAnyWritableHistoryArchive() const
{
    return std::any_of(std::begin(mArchives), std::end(mArchives),
                       [](std::shared_ptr<HistoryArchive> const& x) {
                           return x->hasGetCmd() && x->hasPutCmd();
                       });
}

std::shared_ptr<HistoryArchive>
HistoryArchiveManager::getHistoryArchive(std::string const& name) const
{
    auto it = std::find_if(std::begin(mArchives), std::end(mArchives),
                           [&name](std::shared_ptr<HistoryArchive> const& x) {
                               return x->getName() == name;
                           });
    return it == std::end(mArchives) ? nullptr : *it;
}

std::vector<std::shared_ptr<HistoryArchive>>
HistoryArchiveManager::getWritableHistoryArchives() const
{
    auto result = std::vector<std::shared_ptr<HistoryArchive>>{};
    std::copy_if(std::begin(mArchives), std::end(mArchives),
                 std::back_inserter(result),
                 [](std::shared_ptr<HistoryArchive> const& x) {
                     return x->hasGetCmd() && x->hasPutCmd();
                 });
    return result;
}

double
HistoryArchiveManager::getFailureRate() const
{
    uint64_t successCount{0};
    uint64_t failureCount{0};

    for (auto archive : mArchives)
    {
        successCount += archive->getSuccessCount();
        failureCount += archive->getFailureCount();
    }

    auto total = successCount + failureCount;
    if (total == 0)
    {
        return 0.0;
    }
    else
    {
        return static_cast<double>(failureCount) / total;
    }
}
}
