// Copyright 2014-2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "history/HistoryMaster.h"
// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "main/Application.h"
#include "main/Config.h"
#include "clf/BucketList.h"
#include "clf/CLFMaster.h"
#include "ledger/LedgerMaster.h"
#include "generated/StellarXDR.h"
#include "history/HistoryArchive.h"
#include "history/PublishStateMachine.h"
#include "history/CatchupStateMachine.h"
#include "process/ProcessGateway.h"
#include "util/make_unique.h"
#include "util/Logging.h"
#include "util/TmpDir.h"
#include "crypto/SHA.h"
#include "crypto/Hex.h"
#include "lib/util/format.h"

#include "xdrpp/marshal.h"

#include <fstream>
#include <system_error>

namespace stellar
{

using namespace std;

const uint32_t
HistoryMaster::kCheckpointFrequency = 64;

class
HistoryMaster::Impl
{
    Application& mApp;
    unique_ptr<TmpDir> mWorkDir;
    unique_ptr<PublishStateMachine> mPublish;
    unique_ptr<CatchupStateMachine> mCatchup;
    friend class HistoryMaster;
public:
    Impl(Application &app)
        : mApp(app)
        , mWorkDir(nullptr)
        , mPublish(nullptr)
        , mCatchup(nullptr)
        {}

};

HistoryMaster::HistoryMaster(Application& app)
    : mImpl(make_unique<Impl>(app))
{
}

bool
HistoryMaster::initializeHistoryArchive(Application& app, std::string arch)
{
    auto const& cfg = app.getConfig();
    auto i = cfg.HISTORY.find(arch);
    if (i == cfg.HISTORY.end())
    {
        CLOG(WARNING, "History") << "Can't initialize unknown history archive '" << arch << "'";
        return false;
    }
    HistoryArchiveState has;
    CLOG(INFO, "History") << "Initializing history archive '" << arch << "'";
    bool ok = true;
    bool done = false;
    i->second->putState(
        app, has,
        [arch, &done, &ok](asio::error_code const& ec)
        {
            if (ec)
            {
                ok = false;
                CLOG(WARNING, "History") << "Failed to initialize history archive '" << arch << "'";
            }
            else
            {
                CLOG(INFO, "History") << "Initialized history archive '" << arch << "'";
            }
            done = true;
        });

    while (!done && !app.getClock().getIOService().stopped())
    {
        app.getClock().crank(false);
    }
    return ok;
}

HistoryMaster::~HistoryMaster()
{
}

string const&
HistoryMaster::getTmpDir()
{
    if (!mImpl->mWorkDir)
    {
        TmpDir t = mImpl->mApp.getTmpDirMaster().tmpDir("history");
        mImpl->mWorkDir = make_unique<TmpDir>(std::move(t));
    }
    return mImpl->mWorkDir->getName();
}

std::string
HistoryMaster::localFilename(std::string const& basename)
{
    return this->getTmpDir() + "/" + basename;
}


static void
checkGzipSuffix(string const& filename)
{
    string suf(".gz");
    if (!(filename.size() >= suf.size() &&
          equal(suf.rbegin(), suf.rend(), filename.rbegin())))
    {
        throw runtime_error("filename does not end in .gz");
    }
}

static void
checkNoGzipSuffix(string const& filename)
{
    string suf(".gz");
    if (filename.size() >= suf.size() &&
        equal(suf.rbegin(), suf.rend(), filename.rbegin()))
    {
        throw runtime_error("filename ends in .gz");
    }
}


uint64_t
HistoryMaster::nextCheckpointLedger(uint64_t ledger)
{
    uint64_t res = static_cast<uint64_t>(kCheckpointFrequency);
    return ((ledger + res - 1) / res) * res;
}


void
HistoryMaster::verifyHash(std::string const& filename,
                          uint256 const& hash,
                          std::function<void(asio::error_code const&)> handler)
{
    checkNoGzipSuffix(filename);
    Application& app = this->mImpl->mApp;
    app.getWorkerIOService().post(
        [&app, filename, handler, hash]()
        {
            SHA256 hasher;
            char buf[4096];
            ifstream in(filename, ofstream::binary);
            while (in)
            {
                in.read(buf, sizeof(buf));
                hasher.add(ByteSlice(buf, in.gcount()));
            }
            uint256 vHash = hasher.finish();
            asio::error_code ec;
            if (vHash == hash)
            {
                LOG(DEBUG) << "Verified hash ("
                           << hexAbbrev(hash)
                           << ") for " << filename;
            }
            else
            {
                LOG(WARNING) << "FAILED verifying hash for " << filename;
                LOG(WARNING) << "expected hash: " << binToHex(hash);
                LOG(WARNING) << "computed hash: " << binToHex(vHash);
                ec = std::make_error_code(std::errc::io_error);
            }
            app.getClock().getIOService().post([ec, handler]() { handler(ec); });
        });
}

void
HistoryMaster::decompress(std::string const& filename_gz,
                          std::function<void(asio::error_code const&)> handler,
                          bool keepExisting)
{
    checkGzipSuffix(filename_gz);
    std::string filename = filename_gz.substr(0, filename_gz.size() - 3);
    Application& app = this->mImpl->mApp;
    std::string commandLine("gzip -d ");
    std::string outputFile;
    if (keepExisting)
    {
        // Leave input intact, write output to stdout.
        commandLine += "-c ";
        outputFile = filename;
    }
    commandLine += filename_gz;
    auto exit = app.getProcessGateway().runProcess(commandLine, outputFile);
    exit.async_wait(
        [&app, filename_gz, filename, handler](asio::error_code const& ec)
        {
            if (ec)
            {
                LOG(WARNING) << "'gzip -d " << filename_gz << "' failed,"
                             << " removing " << filename_gz
                             << " and " << filename;
                std::remove(filename_gz.c_str());
                std::remove(filename.c_str());
            }
            handler(ec);
        });
}


void
HistoryMaster::compress(std::string const& filename_nogz,
                        std::function<void(asio::error_code const&)> handler,
                        bool keepExisting)
{
    checkNoGzipSuffix(filename_nogz);
    std::string filename = filename_nogz + ".gz";
    Application& app = this->mImpl->mApp;
    std::string commandLine("gzip ");
    std::string outputFile;
    if (keepExisting)
    {
        // Leave input intact, write output to stdout.
        commandLine += "-c ";
        outputFile = filename;
    }
    commandLine += filename_nogz;
    auto exit = app.getProcessGateway().runProcess(commandLine, outputFile);
    exit.async_wait(
        [&app, filename_nogz, filename, handler](asio::error_code const& ec)
        {
            if (ec)
            {
                LOG(WARNING) << "'gzip " << filename_nogz << "' failed,"
                             << " removing " << filename_nogz
                             << " and " << filename;
                std::remove(filename_nogz.c_str());
                std::remove(filename.c_str());
            }
            handler(ec);
        });
}

void
HistoryMaster::putFile(std::shared_ptr<HistoryArchive> archive,
                       string const& filename,
                       string const& basename,
                       function<void(asio::error_code const& ec)> handler)
{
    assert(archive->hasPutCmd());
    auto cmd = archive->putFileCmd(filename, basename);
    auto exit = this->mImpl->mApp.getProcessGateway().runProcess(cmd);
    exit.async_wait(handler);
}

void
HistoryMaster::getFile(std::shared_ptr<HistoryArchive> archive,
                       string const& basename,
                       string const& filename,
                       function<void(asio::error_code const& ec)> handler)
{
    assert(archive->hasGetCmd());
    auto cmd = archive->getFileCmd(basename, filename);
    auto exit = this->mImpl->mApp.getProcessGateway().runProcess(cmd);
    exit.async_wait(handler);
}


void
HistoryMaster::mkdir(std::shared_ptr<HistoryArchive> archive,
                     std::string const& hexdir,
                     std::function<void(asio::error_code const&)> handler)
{
    assert(archive->hasMkdirCmd());
    auto cmd = archive->mkdirCmd(hexdir);
    auto exit = this->mImpl->mApp.getProcessGateway().runProcess(cmd);
    exit.async_wait(handler);
}


HistoryArchiveState
HistoryMaster::getLastClosedHistoryArchiveState() const
{
    HistoryArchiveState has;
    has.currentLedger = mImpl->mApp.getLedgerMaster().getLastClosedLedgerHeader().header.ledgerSeq;
    auto &bl = mImpl->mApp.getCLFMaster().getBucketList();
    for (size_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        has.currentBuckets.at(i).curr = binToHex(bl.getLevel(i).getCurr()->getHash());
        has.currentBuckets.at(i).snap = binToHex(bl.getLevel(i).getSnap()->getHash());
    }
    return has;
}

void
HistoryMaster::publishHistory(std::function<void(asio::error_code const&)> handler)
{
    if (mImpl->mPublish)
    {
        throw std::runtime_error("History publication already in progress");
    }
    mImpl->mPublish = make_unique<PublishStateMachine>(
        mImpl->mApp,
        [this, handler](asio::error_code const& ec)
        {
            // Tear down the publish state machine when complete then call our
            // caller's handler. Must keep the state machine alive long enough
            // for the callback, though, to avoid killing things living in lambdas.
            std::unique_ptr<PublishStateMachine> m(std::move(this->mImpl->mPublish));
            handler(ec);
        });
}

void
HistoryMaster::snapshotTaken(asio::error_code const& ec,
                             std::shared_ptr<StateSnapshot> snap)
{
    if (mImpl->mPublish)
    {
        mImpl->mPublish->snapshotTaken(ec, snap);
    }
    else
    {
        CLOG(WARNING, "History") << "Publish state machine torn down while taking snapshot";
    }
}

void
HistoryMaster::catchupHistory(uint64_t lastLedger,
                              uint64_t initLedger,
                              ResumeMode mode,
                              std::function<void(asio::error_code const& ec,
                                                 uint64_t nextLedger)> handler)
{
    if (mImpl->mCatchup)
    {
        throw std::runtime_error("Catchup already in progress");
    }
    mImpl->mCatchup = make_unique<CatchupStateMachine>(
        mImpl->mApp,
        lastLedger,
        initLedger,
        mode,
        [this, handler](asio::error_code const& ec, uint64_t nextLedger)
        {
            // Tear down the catchup state machine when complete then call our
            // caller's handler. Must keep the state machine alive long enough
            // for the callback, though, to avoid killing things living in lambdas.
            std::unique_ptr<CatchupStateMachine> m(std::move(this->mImpl->mCatchup));
            handler(ec, nextLedger);
        });
}


}
