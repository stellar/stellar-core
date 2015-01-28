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
#include "util/TmpDir.h"
#include "lib/util/format.h"

#include "xdrpp/marshal.h"

#include <fstream>
#include <system_error>

namespace stellar
{

using namespace std;

class
HistoryMaster::Impl
{
    Application& mApp;
    unique_ptr<TmpDir> mWorkDir;
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

template<typename T> void
HistoryMaster::saveAndCompressAndPut(string const& basename,
                                     shared_ptr<T> xdrp,
                                     function<void(asio::error_code const&)>
                                     handler)
{
    this->mImpl->mApp.getWorkerIOService().post(
        [this, basename, handler, xdrp]()
        {
            string fullname = this->getTmpDir() + "/" + basename;
            ofstream out(fullname, ofstream::binary);
            auto m = xdr::xdr_to_msg(*xdrp);
            out.write(m->raw_data(), m->raw_size());
            this->mImpl->mApp.getMainIOService().post(
                [this, basename, fullname, handler]()
                {
                    auto& app = this->mImpl->mApp;
                    auto exit = app.getProcessGateway().runProcess(
                        "gzip " + fullname);
                    exit.async_wait(
                        [this, basename, fullname, handler](
                            asio::error_code const& ec) {
                            if (ec)
                            {
                                LOG(DEBUG) << "'gzip "
                                           << fullname << "' failed";
                                handler(ec);
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
checkGzipSuffix(string const& filename)
{
    string suf(".gz");
    if (!(filename.size() >= suf.size() &&
          equal(suf.rbegin(), suf.rend(), filename.rbegin())))
    {
        throw runtime_error("filename does not end in .gz");
    }
}

template<typename T> void
HistoryMaster::getAndDecompressAndLoad(string const& basename,
                                       function<void(asio::error_code const&,
                                                     shared_ptr<T>)> handler)
{

    string fullname = getTmpDir() + "/" + basename + ".gz";
    getFile(
        basename + ".gz", fullname,
        [this, fullname, handler](asio::error_code const& ec)
        {
            if (ec)
            {
                handler(ec, shared_ptr<T>(nullptr));
                return;
            }
            checkGzipSuffix(fullname);
            Application& app = this->mImpl->mApp;
            auto exit = app.getProcessGateway().runProcess(
                "gunzip " + fullname);
            exit.async_wait(
                [&app, fullname, handler](asio::error_code const& ec)
                {
                    if (ec)
                    {
                        LOG(DEBUG) << "Removing incoming file "
                                   << fullname;
                        handler(ec, shared_ptr<T>(nullptr));
                        return;
                    }
                    app.getWorkerIOService().post(
                        [&app, fullname, handler]()
                        {
                            asio::error_code ec2;
                            shared_ptr<T> t;
                            try
                            {
                                checkGzipSuffix(fullname);
                                string stem =
                                    fullname.substr(0, fullname.size() - 3);
                                ifstream in(stem, ofstream::binary);
                                in.exceptions(ifstream::failbit);
                                in.seekg(0, in.end);
                                ifstream::pos_type length = in.tellg();
                                in.seekg(0, in.beg);
                                xdr::msg_ptr m = xdr::message_t::alloc(
                                    length -
                                    static_cast<ifstream::pos_type>(4));
                                in.read(m->raw_data(), m->raw_size());
                                in.close();
                                t = make_shared<T>();
                                xdr::xdr_from_msg(m, *t);
                                LOG(DEBUG) << "Removing decompressed file "
                                           << stem;
                                std::remove(stem.c_str());
                            }
                            catch (...)
                            {
                                ec2 = std::make_error_code(
                                    std::errc::io_error);
                            }
                            app.getMainIOService().post(
                                [ec2, handler, t]()
                                {
                                    handler(ec2, t);
                                });
                        });
                });
        });
}

static void
runCommands(Application &app,
            shared_ptr<vector<string>> cmds,
            function<void(asio::error_code const&)> handler,
            size_t i = 0,
            asio::error_code ec = asio::error_code())
{
    if (i < cmds->size())
    {
        auto exit = app.getProcessGateway().runProcess((*cmds)[i]);
        exit.async_wait(
            [&app, cmds, handler, i](asio::error_code const& ec)
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
HistoryMaster::putFile(string const& filename,
                       string const& basename,
                       function<void(asio::error_code const& ec)> handler)
{
    auto const& hist = mImpl->mApp.getConfig().HISTORY;
    auto commands = make_shared<vector<string>>();
    for (auto const& pair : hist)
    {
        auto s = pair.second->putFileCmd(filename, basename);
        if (!s.empty())
        {
            commands->push_back(s);
        }
    }
    runCommands(mImpl->mApp, commands,
                [filename, handler](asio::error_code const& ec)
                {
                    LOG(DEBUG) << "Removing temporary file " << filename;
                    std::remove(filename.c_str());
                    handler(ec);
                });
}

void
HistoryMaster::getFile(string const& basename,
                       string const& filename,
                       function<void(asio::error_code const& ec)> handler)
{
    auto const& hist = mImpl->mApp.getConfig().HISTORY;
    auto commands = make_shared<vector<string>>();
    for (auto const& pair : hist)
    {
        auto s = pair.second->getFileCmd(basename, filename);
        if (!s.empty())
        {
            commands->push_back(s);
        }
    }
    runCommands(mImpl->mApp, commands, handler);
}

static string
bucketBasename(uint64_t ledgerSeq, uint32_t ledgerCount)
{
    return fmt::format("bucket_{:d}_{:d}.xdr",
                       ledgerSeq, ledgerCount);
}

static string
historyBasename(uint64_t fromLedger, uint64_t toLedger)
{
    return fmt::format("history_{:d}_{:d}.xdr",
                       fromLedger, toLedger);
}

void
HistoryMaster::archiveBucket(shared_ptr<CLFBucket> bucket,
                             function<void(asio::error_code const&)> handler)
{
    string basename = bucketBasename(bucket->header.ledgerSeq,
                                     bucket->header.ledgerCount);
    saveAndCompressAndPut<CLFBucket>(basename, bucket, handler);
}

void
HistoryMaster::archiveHistory(shared_ptr<History> hist,
                              function<void(asio::error_code const&)> handler)
{
    string basename = historyBasename(hist->fromLedger,
                                      hist->toLedger);
    saveAndCompressAndPut<History>(basename, hist, handler);
}

void
HistoryMaster::acquireBucket(uint64_t ledgerSeq,
                             uint32_t ledgerCount,
                             function<void(asio::error_code const&,
                                           shared_ptr<CLFBucket>)> handler)
{
    string basename = bucketBasename(ledgerSeq, ledgerCount);
    getAndDecompressAndLoad<CLFBucket>(basename, handler);
}

void
HistoryMaster::acquireHistory(uint64_t fromLedger,
                              uint64_t toLedger,
                              function<void(asio::error_code const&,
                                            shared_ptr<History>)> handler)
{
    string basename = historyBasename(fromLedger, toLedger);
    getAndDecompressAndLoad<History>(basename, handler);
}

}
