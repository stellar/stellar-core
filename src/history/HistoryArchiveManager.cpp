// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryArchiveManager.h"
#include "history/HistoryArchive.h"
#include "history/HistoryArchiveReportWork.h"
#include "historywork/CheckSingleLedgerHeaderWork.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "historywork/PutHistoryArchiveStateWork.h"
#include "lib/util/stdrandom.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "work/WorkScheduler.h"
#include "work/WorkSequence.h"

#include <vector>

namespace stellar
{

HistoryArchiveManager::HistoryArchiveManager(Application& app) : mApp{app}
{
    for (auto const& archiveConfiguration : mApp.getConfig().HISTORY)
        mArchives.push_back(
            std::make_shared<HistoryArchive>(archiveConfiguration.second));
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
        CLOG_FATAL(
            History,
            "Archive '{}' has no 'get' or 'put' command, will not function", a);
        badArchives = true;
    }

    for (auto const& a : writeOnlyArchives)
    {
        CLOG_FATAL(
            History,
            "Archive '{}' has 'put' but no 'get' command, will be unwritable",
            a);
        badArchives = true;
    }

    for (auto const& a : readWriteArchives)
    {
        CLOG_INFO(History,
                  "Archive '{}' has 'put' and 'get' commands, will be read and "
                  "written",
                  a);
    }

    for (auto const& a : readOnlyArchives)
    {
        CLOG_INFO(History,
                  "Archive '{}' has 'get' command only, will not be written",
                  a);
    }

    if (readOnlyArchives.empty() && readWriteArchives.empty())
    {
        if (mApp.getConfig().RUN_STANDALONE)
        {
            CLOG_INFO(History, "No readable archives configured.");
        }
        else
        {
            CLOG_FATAL(History,
                       "No readable archives configured, catchup will fail.");
            badArchives = true;
        }
    }

    if (readWriteArchives.empty())
    {
        CLOG_WARNING(
            History,
            "No writable archives configured, history will not be written.");
    }

    if (badArchives)
    {
        CLOG_ERROR(History, "History archives misconfigured.");
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
        CLOG_DEBUG(History, "Fetching from sole readable history archive '{}'",
                   archives[0]->getName());
        return archives[0];
    }
    else
    {
        stellar::uniform_int_distribution<size_t> dist(0, archives.size() - 1);
        size_t i = dist(getGlobalRandomEngine());
        CLOG_DEBUG(History, "Fetching from readable history archive #{}, '{}'",
                   i, archives[i]->getName());
        return archives[i];
    }
}

std::shared_ptr<BasicWork>
HistoryArchiveManager::getHistoryArchiveReportWork() const
{
    std::vector<std::shared_ptr<GetHistoryArchiveStateWork>> hasWorks;
    for (auto const& archive : mArchives)
    {
        hasWorks.push_back(std::make_shared<GetHistoryArchiveStateWork>(
            mApp, 0, archive, false, BasicWork::RETRY_NEVER));
    }
    return std::make_shared<HistoryArchiveReportWork>(mApp, hasWorks);
};

std::shared_ptr<BasicWork>
HistoryArchiveManager::getCheckLedgerHeaderWork(
    LedgerHeaderHistoryEntry const& lhhe) const
{
    std::vector<std::shared_ptr<BasicWork>> checkWorks;
    for (auto const& archive : mArchives)
    {
        checkWorks.emplace_back(
            std::make_shared<CheckSingleLedgerHeaderWork>(mApp, archive, lhhe));
    }
    return std::make_shared<WorkSequence>(mApp, "archive-ledger-check",
                                          checkWorks, BasicWork::RETRY_NEVER,
                                          /*stopOnFirstFailure=*/false);
}

bool
HistoryArchiveManager::initializeHistoryArchive(std::string const& arch) const
{
    auto archive = getHistoryArchive(arch);
    if (!archive)
    {
        CLOG_FATAL(History, "Can't initialize unknown history archive '{}'",
                   arch);
        return false;
    }

    auto& ws = mApp.getWorkScheduler();

    // First check that there's no existing HAS in the archive
    CLOG_INFO(History, "Probing history archive '{}' for existing state", arch);
    auto getHas =
        ws.executeWork<GetHistoryArchiveStateWork>(0, archive, "hist-init", 0);
    if (getHas->getState() == BasicWork::State::WORK_SUCCESS)
    {
        CLOG_ERROR(History, "History archive '{}' already initialized!", arch);
        return false;
    }
    CLOG_INFO(History, "History archive '{}' appears uninitialized", arch);

    HistoryArchiveState has;
    CLOG_INFO(History, "Initializing history archive '{}'", arch);
    has.resolveAllFutures();
    has.networkPassphrase = mApp.getConfig().NETWORK_PASSPHRASE;

    auto putHas = ws.executeWork<PutHistoryArchiveStateWork>(has, archive);
    if (putHas->getState() == BasicWork::State::WORK_SUCCESS)
    {
        CLOG_INFO(History, "Initialized history archive '{}'", arch);
        return true;
    }
    else
    {
        CLOG_FATAL(History, "Failed to initialize history archive '{}'", arch);
        return false;
    }
}

bool
HistoryArchiveManager::publishEnabled() const
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
}
