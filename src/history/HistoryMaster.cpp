// Copyright 2014-2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include "main/Application.h"
#include "main/Config.h"
#include "generated/StellarXDR.h"
#include "history/HistoryMaster.h"
#include "history/HistoryArchive.h"
#include "process/ProcessGateway.h"
#include "util/make_unique.h"
#include "util/Logging.h"
#include "util/TempDir.h"
#include "lib/util/format.h"

#include "xdrpp/marshal.h"

#include <fstream>

namespace stellar
{

class
HistoryMaster::Impl
{
    Application& mApp;
    std::unique_ptr<TempDir> mWorkDir;
    friend class HistoryMaster;
public:
    Impl(Application &app)
        : mApp(app)
        , mWorkDir(nullptr)
        {}

};


HistoryMaster::HistoryMaster(Application& app)
    : mImpl(make_unique<Impl>(app))
{
}

HistoryMaster::~HistoryMaster()
{
}

/*
 * HistoryMaster behaves differently depending on application state.
 *
 * When application is in anything before CATCHING_UP_STATE, HistoryMaster
 * should be idle.
 *
 * When application is in CATCHING_UP_STATE, HistoryMaster picks an "anchor"
 * ledger X that it's trying to build a complete bucketlist for, as well as a
 * history-suffix from [X,current] as current advances. It downloads buckets
 * for X and accumulates history entries from X forward.
 *
 * Once it has a full set of buckets for X, it will force-apply them to the
 * database (overwrite objects) in order to acquire state X. If successful
 * it will then play transactions forward from X, as fast as it can, until
 * X==current. At that point it will switch to SYNCED_STATE.
 *
 * in SYNCED_STATE it accumulates history entries as they arrive, in the
 * database, periodically flushes those to a file on disk, copies it to s3 (or
 * wherever) and deletes the archived entries. It also flushes buckets to
 * s3 as they are evicted from the CLF.
 */

std::string const&
HistoryMaster::getTempDir()
{
    if (!mImpl->mWorkDir)
    {
        mImpl->mWorkDir = make_unique<TempDir>("history");
    }
    return mImpl->mWorkDir->getName();
}

template<typename T> void
HistoryMaster::saveAndCompressAndPut(std::string const& basename,
                                     std::shared_ptr<T> xdrp,
                                     std::function<void(std::string const&)> handler)
{
    this->mImpl->mApp.getWorkerIOService().post(
        [this, basename, handler, xdrp]()
        {
            std::string fullname = this->getTempDir() + "/" + basename;
            std::ofstream out(fullname, std::ofstream::binary);
            auto m = xdr::xdr_to_msg(*xdrp);
            out.write(m->raw_data(), m->raw_size());
            this->mImpl->mApp.getMainIOService().post(
                [this, basename, fullname, handler]()
                {
                    auto& app = this->mImpl->mApp;
                    auto exit = app.getProcessGateway().runProcess("gzip " + fullname);
                    exit.async_wait(
                        [this, basename, fullname, handler](asio::error_code ec) {
                            if (ec)
                            {
                                LOG(DEBUG) << "'gzip " << fullname << "' failed";
                            }
                            else
                            {
                                this->putFile(fullname + ".gz",
                                              basename  + ".gz", handler);
                            }
                        });
                });
        });
}

void
checkGzipSuffix(std::string const& filename)
{
    std::string suf(".gz");
    if (!(filename.size() >= suf.size() &&
          std::equal(suf.rbegin(), suf.rend(), filename.rbegin())))
    {
        throw std::runtime_error("filename does not end in .gz");
    }
}

template<typename T> void
HistoryMaster::getAndDecompressAndLoad(std::string const& basename,
                                       std::function<void(std::shared_ptr<T>)> handler)
{

    std::string fullname = getTempDir() + "/" + basename + ".gz";
    getFile(
        basename + ".gz", fullname,
        [this, handler](std::string const& fullname)
        {
            checkGzipSuffix(fullname);
            Application& app = this->mImpl->mApp;
            auto exit = app.getProcessGateway().runProcess("gunzip " + fullname);
            exit.async_wait(
                [&app, fullname, handler](asio::error_code ec)
                {
                    if (!ec)
                    {
                        app.getWorkerIOService().post(
                            [&app, fullname, handler]()
                            {
                                checkGzipSuffix(fullname);
                                std::string stem = fullname.substr(0, fullname.size() - 3);
                                std::ifstream in(stem, std::ofstream::binary);
                                in.exceptions(std::ifstream::failbit);
                                in.seekg(0, in.end);
                                std::ifstream::pos_type length = in.tellg();
                                in.seekg(0, in.beg);
                                xdr::msg_ptr m = xdr::message_t::alloc(
                                    length - static_cast<std::ifstream::pos_type>(4));
                                in.read(m->raw_data(), m->raw_size());
                                in.close();
                                std::shared_ptr<T> t = std::make_shared<T>();
                                xdr::xdr_from_msg(m, *t);
                                app.getMainIOService().post(
                                    [handler, t]()
                                    {
                                        handler(t);
                                    });
                            });
                    }
                });
        });
}

static void
runCommands(Application &app,
            std::shared_ptr<std::vector<std::string>> cmds,
            std::function<void(asio::error_code)> handler,
            size_t i = 0,
            asio::error_code ec = asio::error_code())
{
    if (i < cmds->size())
    {
        auto exit = app.getProcessGateway().runProcess((*cmds)[i]);
        exit.async_wait(
            [&app, cmds, handler, i](asio::error_code ec)
            {
                if (ec)
                {
                    handler(ec);
                }
                else
                {
                    runCommands(app, cmds, handler, i+1, ec);
                }
            });
    }
    else
    {
        handler(ec);
    }
}

void
HistoryMaster::putFile(std::string const& filename,
                       std::string const& basename,
                       std::function<void(std::string const&)> handler)
{
    auto const& hist = mImpl->mApp.getConfig().HISTORY;
    auto commands = std::make_shared<std::vector<std::string>>();
    for (auto const& pair : hist)
    {
        auto s = pair.second->putFileCmd(filename, basename);
        if (!s.empty())
        {
            commands->push_back(s);
        }
    }
    runCommands(mImpl->mApp, commands,
                [handler, filename](asio::error_code ec)
                {
                    if (!ec)
                        handler(filename);
                });
}

void
HistoryMaster::getFile(std::string const& basename,
                       std::string const& filename,
                       std::function<void(std::string const&)> handler)
{
    auto const& hist = mImpl->mApp.getConfig().HISTORY;
    auto commands = std::make_shared<std::vector<std::string>>();
    for (auto const& pair : hist)
    {
        auto s = pair.second->getFileCmd(basename, filename);
        if (!s.empty())
        {
            commands->push_back(s);
        }
    }
    runCommands(mImpl->mApp, commands,
                [handler, filename](asio::error_code ec)
                {
                    if (!ec)
                        handler(filename);
                });
}

static std::string
bucketBasename(uint64_t ledgerSeq, uint32_t ledgerCount)
{
    return fmt::format("bucket_{:d}_{:d}.xdr",
                       ledgerSeq, ledgerCount);
}

static std::string
historyBasename(uint64_t fromLedger, uint64_t toLedger)
{
    return fmt::format("history_{:d}_{:d}.xdr",
                       fromLedger, toLedger);
}

void
HistoryMaster::archiveBucket(std::shared_ptr<CLFBucket> bucket,
                             std::function<void(std::string const&)> handler)
{
    std::string basename = bucketBasename(bucket->header.ledgerSeq,
                                          bucket->header.ledgerCount);
    saveAndCompressAndPut<CLFBucket>(basename, bucket, handler);
}

void
HistoryMaster::archiveHistory(std::shared_ptr<History> hist,
                              std::function<void(std::string const&)> handler)
{
    std::string basename = historyBasename(hist->fromLedger,
                                           hist->toLedger);
    saveAndCompressAndPut<History>(basename, hist, handler);
}

void
HistoryMaster::acquireBucket(uint64_t ledgerSeq,
                             uint32_t ledgerCount,
                             std::function<void(std::shared_ptr<CLFBucket>)> handler)
{
    std::string basename = bucketBasename(ledgerSeq, ledgerCount);
    getAndDecompressAndLoad<CLFBucket>(basename, handler);
}

void
HistoryMaster::acquireHistory(uint64_t fromLedger,
                              uint64_t toLedger,
                              std::function<void(std::shared_ptr<History>)> handler)
{
    std::string basename = historyBasename(fromLedger, toLedger);
    getAndDecompressAndLoad<History>(basename, handler);
}

}
