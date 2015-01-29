// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "main/Config.h"
#include "util/TmpDir.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include "crypto/Random.h"
#include "crypto/Hex.h"

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include <cstdio>

namespace stellar
{

class TmpDir::Impl : public std::string
{
public:
    Impl(std::string const& s) : std::string(s)
    {
    }
};

TmpDir::TmpDir(Application& app, std::string const& prefix)
{
    size_t attempts = 0;
    while (true)
    {
        std::string hex = binToHex(randomBytes(8));
        std::string name = app.getConfig().TMP_DIR_PATH + "/" + prefix + "-" + hex;
        if (TmpDir::mkdir(name))
        {
            mImpl = make_unique<Impl>(name);
            break;
        }
        if (++attempts > 100)
        {
            throw std::runtime_error("failed to create TmpDir");
        }
    }
}

TmpDir::TmpDir(TmpDir&& other)
    : mImpl(std::move(other.mImpl))
{
}

std::string const&
TmpDir::getName() const
{
    return *mImpl;
}

#ifdef _WIN32
#include <Windows.h>
#include <Shellapi.h>

bool
TmpDir::exists(std::string const& name)
{
    if (GetFileAttributes(name.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            return false;
        }
        else
        {
            std::string msg("error accessing path: ");
            throw std::runtime_error(msg + name);
        }
    }
    return true;
}

bool
TmpDir::mkdir(std::string const& name)
{
    bool b = _mkdir(name.c_str()) == 0;
    LOG(DEBUG) << "TmpDir " << (b ? "created " : "failed to create ") << name;
    return b;
}

void
TmpDir::deltree(std::string const& d)
{
    SHFILEOPSTRUCT s = {0};
    std::string from = d;
    from.push_back('\0');
    from.push_back('\0');
    s.wFunc = FO_DELETE;
    s.pFrom = from.data();
    s.fFlags = FOF_NO_UI;
    if (SHFileOperation(&s) != 0)
    {
        throw std::runtime_error("SHFileOperation failed in deltree");
    }
}

#else
#include <ftw.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>

bool
TmpDir::exists(std::string const& name)
{
    struct stat buf;
    if (stat(name.c_str(), &buf) == -1)
    {
        if (errno == ENOENT)
        {
            return false;
        }
        else
        {
            std::string msg("error accessing path: ");
            throw std::runtime_error(msg + name);
        }
    }
    return true;
}

bool
TmpDir::mkdir(std::string const& name)
{
    bool b = ::mkdir(name.c_str(), 0700) == 0;
    LOG(DEBUG) << "TmpDir " << (b ? "created " : "failed to create ") << name;
    return b;
}

int
callback(char const* name,
         struct stat const* st,
         int flag,
         struct FTW *ftw)
{
    LOG(DEBUG) << "TmpDir deleting: " << name;
    if (flag == FTW_DP)
    {
        if (rmdir(name) != 0)
        {
            throw std::runtime_error("rmdir failed");
        }
    }
    else
    {
        if (std::remove(name) != 0)
        {
            throw std::runtime_error("std::remove failed");
        }
    }
    return 0;
}

void
TmpDir::deltree(std::string const& d)
{
    if (nftw(d.c_str(), callback, FOPEN_MAX, FTW_DEPTH) != 0)
    {
        throw std::runtime_error("nftw failed in deltree");
    }
}
#endif

TmpDir::~TmpDir()
{
    if (mImpl)
    {
        deltree(*mImpl);
        LOG(DEBUG) << "TmpDir deleted: " << *mImpl;
        mImpl.reset();
    }
}

TmpDirMaster::TmpDirMaster(Application& app)
    : mApp(app)
{
    clean();
    TmpDir::mkdir(mApp.getConfig().TMP_DIR_PATH);
}

TmpDirMaster::~TmpDirMaster()
{
    clean();
}

void
TmpDirMaster::clean()
{
    std::string const& tmpRoot = mApp.getConfig().TMP_DIR_PATH;
    if (TmpDir::exists(tmpRoot))
    {
        LOG(DEBUG) << "TmpDirMaster cleaning: " << tmpRoot;
        TmpDir::deltree(tmpRoot);
    }
}

TmpDir
TmpDirMaster::tmpDir(std::string const& prefix)
{
    return TmpDir(mApp, prefix);
}


}
