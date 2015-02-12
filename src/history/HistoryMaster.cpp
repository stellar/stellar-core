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
#include "crypto/SHA.h"
#include "crypto/Hex.h"
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

{
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
HistoryMaster::verifyHash(std::string const& filename,
                          uint256 const& hash,
                          std::function<void(asio::error_code const&)> handler)
{
    checkNoGzipSuffix(filename);
    Application& app = this->mImpl->mApp;
    app.getWorkerIOService().post(
        [&app, filename, handler, hash]()
        {
            SHA512_256 hasher;
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
                ec = std::make_error_code(std::errc::io_error);
            }
            app.getMainIOService().post([ec, handler]() { handler(ec); });
        });
}

void
HistoryMaster::decompress(std::string const& filename_gz,
                          std::function<void(asio::error_code const&)> handler)
{
    checkGzipSuffix(filename_gz);
    Application& app = this->mImpl->mApp;
    auto exit = app.getProcessGateway().runProcess("gunzip " + filename_gz);
    exit.async_wait(
        [&app, filename_gz, handler](asio::error_code const& ec)
        {
            std::string filename = filename_gz.substr(0, filename_gz.size() - 3);
            if (ec)
            {
                LOG(WARNING) << "'gunzip " << filename_gz << "' failed,"
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
                        std::function<void(asio::error_code const&)> handler)
{
    checkNoGzipSuffix(filename_nogz);
    Application& app = this->mImpl->mApp;
    auto exit = app.getProcessGateway().runProcess("gzip " + filename_nogz);
    exit.async_wait(
        [&app, filename_nogz, handler](asio::error_code const& ec)
        {
            if (ec)
            {
                std::string filename = filename_nogz + ".gz";
                LOG(WARNING) << "'gzip " << filename_nogz << "' failed,"
                             << " removing " << filename_nogz
                             << " and " << filename;
                std::remove(filename_nogz.c_str());
                std::remove(filename.c_str());
            }
            handler(ec);
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
    runCommands(mImpl->mApp, commands, handler);
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

}
