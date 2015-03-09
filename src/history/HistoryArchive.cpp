// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "history/HistoryArchive.h"
#include "clf/BucketList.h"
#include "crypto/Hex.h"
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
#include <set>

namespace stellar
{

class HistoryArchive::Impl
{
public:
    std::string mName;
    std::string mGetCmd;
    std::string mPutCmd;
    std::string mMkdirCmd;
    Impl(std::string const& name,
         std::string const& getCmd,
         std::string const& putCmd,
         std::string const& mkdirCmd)
        : mName(name)
        , mGetCmd(getCmd)
        , mPutCmd(putCmd)
        , mMkdirCmd(mkdirCmd)
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

std::vector<std::string>
HistoryArchiveState::differingBuckets(HistoryArchiveState const& other) const
{
    std::set<std::string> inhibit;
    uint256 zero;
    inhibit.insert(binToHex(zero));
    for (auto b : other.currentBuckets)
    {
        inhibit.insert(b.curr);
        inhibit.insert(b.snap);
    }
    std::vector<std::string> ret;
    for (size_t i = BucketList::kNumLevels; i != 0; --i)
    {
        auto const& s = currentBuckets[i-1].snap;
        auto const& c = currentBuckets[i-1].curr;
        if (inhibit.find(s) == inhibit.end())
        {
            ret.push_back(s);
            inhibit.insert(s);
        }
        if (inhibit.find(c) == inhibit.end())
        {
            ret.push_back(c);
            inhibit.insert(c);
        }
    }
    return ret;
}


HistoryArchiveState::HistoryArchiveState()
{
    uint256 u;
    std::string s = binToHex(u);
    HistoryStateBucket b;
    b.curr = s;
    b.snap = s;
    while (currentBuckets.size() < BucketList::kNumLevels)
    {
        currentBuckets.push_back(b);
    }
}


HistoryArchive::HistoryArchive(std::string const& name,
                               std::string const& getCmd,
                               std::string const& putCmd,
                               std::string const& mkdirCmd)
    : mImpl(make_unique<Impl>(name, getCmd, putCmd, mkdirCmd))
{
}

HistoryArchive::~HistoryArchive()
{
}


bool
HistoryArchive::hasGetCmd() const
{
    return !mImpl->mGetCmd.empty();
}

bool
HistoryArchive::hasPutCmd() const
{
    return !mImpl->mPutCmd.empty();
}

bool
HistoryArchive::hasMkdirCmd() const
{
    return !mImpl->mMkdirCmd.empty();
}

std::string const&
HistoryArchive::getName() const
{
    return mImpl->mName;
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
                CLOG(WARNING, "History")
                    << "failed to get " << basename
                    << " from history archive '" << archiveName << "'";
            }
            else
            {
                CLOG(DEBUG, "History")
                    << "got " << basename
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
                CLOG(WARNING, "History")
                    << "failed to put " << basename
                    << " in history archive '" << archiveName << "'";
            }
            else
            {
                CLOG(DEBUG, "History")
                    << "put " << basename
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

std::string
HistoryArchive::mkdirCmd(std::string const& dirname) const
{
    if (mImpl->mMkdirCmd.empty())
        return "";
    return fmt::format(mImpl->mMkdirCmd, dirname);
}


}
