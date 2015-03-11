// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "history/HistoryArchive.h"
#include "clf/BucketList.h"
#include "crypto/Hex.h"
#include "history/HistoryMaster.h"
#include "history/FileTransferInfo.h"
#include "process/ProcessGateway.h"
#include "main/Application.h"
#include "util/Fs.h"
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
HistoryArchiveState::baseName()
{
    return std::string("stellar-history.json");
}

std::string
HistoryArchiveState::wellKnownRemoteDir()
{
    // The RFC 5785 dir
    return std::string(".well-known");
}

std::string
HistoryArchiveState::wellKnownRemoteName()
{
    return wellKnownRemoteDir() + "/" + baseName();
}

std::string
HistoryArchiveState::remoteDir(uint32_t snapshotNumber)
{
    return fs::remoteDir("history",
                         fs::hexStr(snapshotNumber));
}

std::string
HistoryArchiveState::remoteName(uint32_t snapshotNumber)
{
    return fs::remoteName("history",
                          fs::hexStr(snapshotNumber),
                          "json");
}

std::string
HistoryArchiveState::localName(Application& app, std::string const& archiveName)
{
    return app.getHistoryMaster().localFilename(archiveName + "-" + baseName());
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

void
HistoryArchive::getState(Application& app,
                         std::function<void(asio::error_code const&,
                                            HistoryArchiveState const&)> handler) const
{
    auto remote = HistoryArchiveState::wellKnownRemoteName();
    auto local = HistoryArchiveState::localName(app, mImpl->mName);
    auto cmd = getFileCmd(remote, local);
    auto exit = app.getProcessGateway().runProcess(cmd);
    auto archiveName = mImpl->mName;
    exit.async_wait(
        [handler, local, remote, archiveName](asio::error_code const& ec)
        {
            HistoryArchiveState has;
            if (ec)
            {
                CLOG(WARNING, "History")
                    << "failed to get " << remote
                    << " from history archive '" << archiveName << "'";
            }
            else
            {
                CLOG(DEBUG, "History")
                    << "got " << remote
                    << " from history archive '" << archiveName << "'";
                has.load(local);
            }
            std::remove(local.c_str());
            handler(ec, has);
        });
}

void
HistoryArchive::putState(Application& app,
                         HistoryArchiveState const& s,
                         std::function<void(asio::error_code const&)> handler) const
{
    auto remote = HistoryArchiveState::wellKnownRemoteName();
    auto local = HistoryArchiveState::localName(app, mImpl->mName);
    s.save(local);
    auto &hm = app.getHistoryMaster();
    auto archiveName = mImpl->mName;
    auto self = shared_from_this();

    hm.mkdir(
        self, HistoryArchiveState::wellKnownRemoteDir(),
        [&hm, self, local, remote,
         handler, archiveName](asio::error_code const& ec)
        {
            if (ec)
            {
                CLOG(WARNING, "History")
                    << "failed to make directory "
                    << HistoryArchiveState::wellKnownRemoteDir()
                    << " in history archive '" << archiveName << "'";
                std::remove(local.c_str());
                handler(ec);
            }
            else
            {
                hm.putFile(
                    self, local, remote,
                    [local, remote, archiveName, handler](asio::error_code const& ec2)
                    {
                        if (ec2)
                        {
                            CLOG(DEBUG, "History")
                                << "failed to put " << remote
                                << " in history archive '" << archiveName << "'";
                        }
                        else
                        {
                            CLOG(DEBUG, "History")
                                << "put " << remote
                                << " in history archive '" << archiveName << "'";
                        }
                        std::remove(local.c_str());
                        handler(ec2);
                    });
            }
        });
}

std::string
HistoryArchive::getFileCmd(std::string const& remote, std::string const& local) const
{
    if (mImpl->mGetCmd.empty())
        return "";
    return fmt::format(mImpl->mGetCmd, remote, local);
}

std::string
HistoryArchive::putFileCmd(std::string const& local, std::string const& remote) const
{
    if (mImpl->mPutCmd.empty())
        return "";
    return fmt::format(mImpl->mPutCmd, local, remote);
}

std::string
HistoryArchive::mkdirCmd(std::string const& remoteDir) const
{
    if (mImpl->mMkdirCmd.empty())
        return "";
    return fmt::format(mImpl->mMkdirCmd, remoteDir);
}


}
