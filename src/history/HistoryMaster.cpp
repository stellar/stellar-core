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
#include <regex>

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
HistoryMaster::bucketBasename(std::string const& bucketHexHash)
{
    return "bucket-" + bucketHexHash + ".xdr";
}

std::string
HistoryMaster::bucketHexHash(std::string const& bucketBasename)
{
    std::regex rx("bucket-([:xdigit:]{64})\\.xdr");
    std::smatch sm;
    if (!std::regex_match(bucketBasename, sm, rx))
    {
        throw std::runtime_error("Unknown form of bucket basename");
    }
    return sm[1];
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
    auto exit = app.getProcessGateway().runProcess("gzip -d " + filename_gz);
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

}
