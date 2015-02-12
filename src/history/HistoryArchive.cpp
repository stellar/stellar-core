// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "history/HistoryArchive.h"
#include "history/HistoryMaster.h"
#include "process/ProcessGateway.h"
#include "main/Application.h"
#include "util/make_unique.h"
#include "util/Logging.h"
#include <cereal/cereal.hpp>
#include <cereal/archives/json.hpp>
#include <cereal/types/vector.hpp>
#include "lib/util/format.h"

#include <iostream>
#include <fstream>

namespace stellar
{

class HistoryArchive::Impl
{
public:
    std::string mName;
    std::string mGetCmd;
    std::string mPutCmd;
    Impl(std::string const& name,
         std::string const& getCmd,
         std::string const& putCmd)
        : mName(name)
        , mGetCmd(getCmd)
        , mPutCmd(putCmd)
        {}
};

void
HistoryArchiveState::save(std::string const& outFile) const
{
    std::ofstream out(outFile);
    cereal::JSONOutputArchive ar(out);
    serialize(ar);
}

void
HistoryArchiveState::load(std::string const& inFile)
{
    std::ifstream in(inFile);
    cereal::JSONInputArchive ar(in);
    serialize(ar);
}

std::string
HistoryArchiveState::basename()
{
    return std::string("stellar-history.json");
}

HistoryArchive::HistoryArchive(std::string const& name,
                               std::string const& getCmd,
                               std::string const& putCmd)
    : mImpl(make_unique<Impl>(name, getCmd, putCmd))
{
}

HistoryArchive::~HistoryArchive()
{
}

std::string
HistoryArchive::qualifiedFilename(Application& app,
                                  std::string const& basename) const
{
    return app.getHistoryMaster().localFilename(mImpl->mName + "-" + basename);
}

void
HistoryArchive::getState(Application& app,
                         std::function<void(asio::error_code const&,
                                            HistoryArchiveState const&)> handler) const
{
    auto basename = HistoryArchiveState::basename();
    auto filename = qualifiedFilename(app, basename);
    auto cmd = getFileCmd(basename, filename);
    auto exit = app.getProcessGateway().runProcess(cmd);
    auto archiveName = mImpl->mName;
    exit.async_wait(
        [handler, filename, basename, archiveName](asio::error_code const& ec)
        {
            HistoryArchiveState has;
            if (ec)
            {
                LOG(WARNING) << "failed to get " << basename
                             << " from history archive '" << archiveName << "'";
            }
            else
            {
                LOG(DEBUG) << "got " << basename
                           << " from history archive '" << archiveName << "'";
                has.load(filename);
            }
            std::remove(filename.c_str());
            handler(ec, has);
        });
}

void
HistoryArchive::putState(Application& app,
                         HistoryArchiveState const& s,
                         std::function<void(asio::error_code const&)> handler) const
{
    auto basename = HistoryArchiveState::basename();
    auto filename = qualifiedFilename(app, basename);
    s.save(filename);
    auto cmd = putFileCmd(filename, basename);
    auto exit = app.getProcessGateway().runProcess(cmd);
    auto archiveName = mImpl->mName;
    exit.async_wait(
        [handler, basename, filename, archiveName](asio::error_code const& ec)
        {
            if (ec)
            {
                LOG(WARNING) << "failed to put " << basename
                             << " in history archive '" << archiveName << "'";
            }
            else
            {
                LOG(DEBUG) << "put " << basename
                             << " in history archive '" << archiveName << "'";
            }
            std::remove(filename.c_str());
            handler(ec);
        });
}

std::string
HistoryArchive::getFileCmd(std::string const& basename, std::string const& filename) const
{
    if (mImpl->mGetCmd.empty())
        return "";
    return fmt::format(mImpl->mGetCmd, basename, filename);
}

std::string
HistoryArchive::putFileCmd(std::string const& filename, std::string const& basename) const
{
    if (mImpl->mPutCmd.empty())
        return "";
    return fmt::format(mImpl->mPutCmd, filename, basename);
}


}
