// Copyright 2014-2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include "main/Application.h"
#include "main/Config.h"
#include "clf/BucketList.h"
#include "clf/CLFManager.h"
#include "ledger/LedgerManagerImpl.h"
#include "generated/StellarXDR.h"
#include "history/HistoryArchive.h"
#include "history/HistoryManagerImpl.h"
#include "history/PublishStateMachine.h"
#include "history/CatchupStateMachine.h"
#include "process/ProcessGateway.h"
#include "util/make_unique.h"
#include "util/Logging.h"
#include "util/TmpDir.h"
#include "crypto/SHA.h"
#include "crypto/Hex.h"
#include "lib/util/format.h"
#include "medida/metrics_registry.h"
#include "medida/meter.h"
#include "xdrpp/marshal.h"

#include <fstream>
#include <system_error>

namespace stellar
{

using namespace std;

const uint32_t HistoryManager::kCheckpointFrequency = 64;

bool
HistoryManager::initializeHistoryArchive(Application& app, std::string arch)
{
    auto const& cfg = app.getConfig();
    auto i = cfg.HISTORY.find(arch);
    if (i == cfg.HISTORY.end())
    {
        CLOG(FATAL, "History") << "Can't initialize unknown history archive '"
                               << arch << "'";
        return false;
    }
    HistoryArchiveState has;
    CLOG(INFO, "History") << "Initializing history archive '" << arch << "'";
    bool ok = true;
    bool done = false;
    i->second->putState(app, has, [arch, &done, &ok](asio::error_code const& ec)
                        {
                            if (ec)
                            {
                                ok = false;
                                CLOG(FATAL, "History")
                                    << "Failed to initialize history archive '"
                                    << arch << "'";
                            }
                            else
                            {
                                CLOG(INFO, "History")
                                    << "Initialized history archive '" << arch
                                    << "'";
                            }
                            done = true;
                        });

    while (!done && !app.getClock().getIOService().stopped())
    {
        app.getClock().crank(false);
    }
    return ok;
}

uint32_t
HistoryManager::nextCheckpointLedger(uint32_t ledger)
{
    if (ledger == 0)
        return kCheckpointFrequency;
    uint64_t res = static_cast<uint64_t>(kCheckpointFrequency);
    return static_cast<uint32_t>(((ledger + res - 1) / res) * res);
}

std::unique_ptr<HistoryManager>
HistoryManager::create(Application& app)
{
    return make_unique<HistoryManagerImpl>(app);
}


HistoryManagerImpl::HistoryManagerImpl(Application& app)
    : mApp(app)
    , mWorkDir(nullptr)
    , mPublish(nullptr)
    , mCatchup(nullptr)

    , mPublishSkip(app.getMetrics().NewMeter({"history", "publish", "skip"},
                                             "event"))
    , mPublishQueue(app.getMetrics().NewMeter({"history", "publish", "queue"},
                                              "event"))
    , mPublishDelay(app.getMetrics().NewMeter({"history", "publish", "delay"},
                                              "event"))
    , mPublishStart(app.getMetrics().NewMeter(
                        {"history", "publish", "start"}, "event"))
    , mPublishSuccess(app.getMetrics().NewMeter(
                          {"history", "publish", "success"}, "event"))
    , mPublishFailure(app.getMetrics().NewMeter(
                          {"history", "publish", "failure"}, "event"))
    , mCatchupStart(app.getMetrics().NewMeter(
                        {"history", "catchup", "start"}, "event"))
    , mCatchupSuccess(app.getMetrics().NewMeter(
                          {"history", "catchup", "success"}, "event"))
    , mCatchupFailure(app.getMetrics().NewMeter(
                          {"history", "catchup", "failure"}, "event"))
{
}

HistoryManagerImpl::~HistoryManagerImpl()
{
}

string const&
HistoryManagerImpl::getTmpDir()
{
    if (!mWorkDir)
    {
        TmpDir t = mApp.getTmpDirMaster().tmpDir("history");
        mWorkDir = make_unique<TmpDir>(std::move(t));
    }
    return mWorkDir->getName();
}

std::string
HistoryManagerImpl::localFilename(std::string const& basename)
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

void
HistoryManagerImpl::verifyHash(
    std::string const& filename, uint256 const& hash,
    std::function<void(asio::error_code const&)> handler) const
{
    checkNoGzipSuffix(filename);
    Application& app = this->mApp;
    app.getWorkerIOService().post(
        [&app, filename, handler, hash]()
        {
            auto hasher = SHA256::create();
            char buf[4096];
            ifstream in(filename, ofstream::binary);
            while (in)
            {
                in.read(buf, sizeof(buf));
                hasher->add(ByteSlice(buf, in.gcount()));
            }
            uint256 vHash = hasher->finish();
            asio::error_code ec;
            if (vHash == hash)
            {
                LOG(DEBUG) << "Verified hash (" << hexAbbrev(hash) << ") for "
                           << filename;
            }
            else
            {
                LOG(WARNING) << "FAILED verifying hash for " << filename;
                LOG(WARNING) << "expected hash: " << binToHex(hash);
                LOG(WARNING) << "computed hash: " << binToHex(vHash);
                ec = std::make_error_code(std::errc::io_error);
            }
            app.getClock().getIOService().post([ec, handler]()
                                               {
                                                   handler(ec);
                                               });
        });
}

void
HistoryManagerImpl::decompress(std::string const& filename_gz,
                          std::function<void(asio::error_code const&)> handler,
                          bool keepExisting) const
{
    checkGzipSuffix(filename_gz);
    std::string filename = filename_gz.substr(0, filename_gz.size() - 3);
    Application& app = this->mApp;
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
                             << " removing " << filename_gz << " and "
                             << filename;
                std::remove(filename_gz.c_str());
                std::remove(filename.c_str());
            }
            handler(ec);
        });
}

void
HistoryManagerImpl::compress(std::string const& filename_nogz,
                        std::function<void(asio::error_code const&)> handler,
                        bool keepExisting) const
{
    checkNoGzipSuffix(filename_nogz);
    std::string filename = filename_nogz + ".gz";
    Application& app = this->mApp;
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
                             << " removing " << filename_nogz << " and "
                             << filename;
                std::remove(filename_nogz.c_str());
                std::remove(filename.c_str());
            }
            handler(ec);
        });
}

void
HistoryManagerImpl::putFile(std::shared_ptr<HistoryArchive const> archive,
                       string const& local, string const& remote,
                       function<void(asio::error_code const& ec)> handler) const
{
    assert(archive->hasPutCmd());
    auto cmd = archive->putFileCmd(local, remote);
    auto exit = this->mApp.getProcessGateway().runProcess(cmd);
    exit.async_wait(handler);
}

void
HistoryManagerImpl::getFile(std::shared_ptr<HistoryArchive const> archive,
                       string const& remote, string const& local,
                       function<void(asio::error_code const& ec)> handler) const
{
    assert(archive->hasGetCmd());
    auto cmd = archive->getFileCmd(remote, local);
    auto exit = this->mApp.getProcessGateway().runProcess(cmd);
    exit.async_wait(handler);
}

void
HistoryManagerImpl::mkdir(std::shared_ptr<HistoryArchive const> archive,
                     std::string const& dir,
                     std::function<void(asio::error_code const&)> handler) const
{
    if (archive->hasMkdirCmd())
    {
        auto cmd = archive->mkdirCmd(dir);
        auto exit = this->mApp.getProcessGateway().runProcess(cmd);
        exit.async_wait(handler);
    }
    else
    {
        asio::error_code ec;
        handler(ec);
    }
}

HistoryArchiveState
HistoryManagerImpl::getLastClosedHistoryArchiveState() const
{
    HistoryArchiveState has;
    has.currentLedger = mApp.getLedgerManagerImpl()
                            .getLastClosedLedgerHeader()
                            .header.ledgerSeq;
    auto& bl = mApp.getCLFManager().getBucketList();
    for (size_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        has.currentBuckets.at(i).curr =
            binToHex(bl.getLevel(i).getCurr()->getHash());
        has.currentBuckets.at(i).snap =
            binToHex(bl.getLevel(i).getSnap()->getHash());
    }
    return has;
}

bool
HistoryManagerImpl::hasAnyWritableHistoryArchive()
{
    auto const& hist = mApp.getConfig().HISTORY;
    for (auto const& pair : hist)
    {
        if (pair.second->hasGetCmd() && pair.second->hasPutCmd())
            return true;
    }
    return false;
}

bool
HistoryManagerImpl::maybePublishHistory(
    std::function<void(asio::error_code const&)> handler)
{
    uint32_t seq =
        mApp.getLedgerManagerImpl().getCurrentLedgerHeader().ledgerSeq;
    if (seq != nextCheckpointLedger(seq))
    {
        return false;
    }

    if (!hasAnyWritableHistoryArchive())
    {
        mPublishSkip.Mark();
        CLOG(WARNING, "History")
            << "Skipping checkpoint, no writable history archives";
        return false;
    }

    publishHistory(handler);
    return true;
}

void
HistoryManagerImpl::publishHistory(
    std::function<void(asio::error_code const&)> handler)
{
    if (!mPublish)
    {
        mPublish = make_unique<PublishStateMachine>(mApp);
    }
    mPublishQueue.Mark();
    auto snap = PublishStateMachine::takeSnapshot(mApp);
    if (mPublish->queueSnapshot(
        snap,
        [this, handler](asio::error_code const& ec)
        {
            if (ec)
            {
                this->mPublishFailure.Mark();
            }
            else
            {
                this->mPublishSuccess.Mark();
            }
            handler(ec);
        }))
    {
        // Only bump this counter if there's a delay building up.
        mPublishDelay.Mark();
    }
}

void
HistoryManagerImpl::snapshotWritten(asio::error_code const& ec)
{
    assert(mPublish);
    mPublishStart.Mark();
    mPublish->snapshotWritten(ec);
}

void
HistoryManagerImpl::catchupHistory(
    uint32_t initLedger, ResumeMode mode,
    std::function<void(asio::error_code const& ec, ResumeMode mode,
                       LedgerHeaderHistoryEntry const& lastClosed)> handler)
{
    if (mCatchup)
    {
        throw std::runtime_error("Catchup already in progress");
    }
    mCatchupStart.Mark();
    mCatchup = make_unique<CatchupStateMachine>(
        mApp, initLedger, mode,
        [this, handler](asio::error_code const& ec,
                        HistoryManagerImpl::ResumeMode mode,
                        LedgerHeaderHistoryEntry const& lastClosed)
        {
            if (ec)
            {
                this->mCatchupFailure.Mark();
            }
            else
            {
                this->mCatchupSuccess.Mark();
            }
            // Tear down the catchup state machine when complete then call our
            // caller's handler. Must keep the state machine alive long enough
            // for the callback, though, to avoid killing things living in
            // lambdas.
            std::unique_ptr<CatchupStateMachine> m(
                std::move(this->mCatchup));
            handler(ec, mode, lastClosed);
        });
}

uint64_t
HistoryManagerImpl::getPublishSkipCount()
{
    return mPublishSkip.count();
}

uint64_t
HistoryManagerImpl::getPublishQueueCount()
{
    return mPublishQueue.count();
}

uint64_t
HistoryManagerImpl::getPublishDelayCount()
{
    return mPublishDelay.count();
}

uint64_t
HistoryManagerImpl::getPublishStartCount()
{
    return mPublishStart.count();
}

uint64_t
HistoryManagerImpl::getPublishSuccessCount()
{
    return mPublishSuccess.count();
}

uint64_t
HistoryManagerImpl::getPublishFailureCount()
{
    return mPublishFailure.count();
}

uint64_t
HistoryManagerImpl::getCatchupStartCount()
{
    return mCatchupStart.count();
}

uint64_t
HistoryManagerImpl::getCatchupSuccessCount()
{
    return mCatchupSuccess.count();
}

uint64_t
HistoryManagerImpl::getCatchupFailureCount()
{
    return mCatchupFailure.count();
}
}
