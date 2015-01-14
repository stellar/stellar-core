// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "util/TempDir.h"
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

class TempDir::Impl : public std::string
{
public:
    Impl(std::string const& s) : std::string(s)
    {
    }
};

TempDir::TempDir(std::string prefix)
{
    size_t attempts = 0;
    while (true)
    {
        std::string hex = binToHex(randomBytes(8));
        std::string name = prefix + "-" + hex;
#ifdef _WIN32
        int ret = _mkdir(name.c_str());
#else
        int ret = mkdir(name.c_str(), 0700);
#endif
        if (ret == 0)
        {
            mImpl = make_unique<Impl>(name);
            LOG(DEBUG) << "TempDir created: " << name;
            break;
        }
        if (++attempts > 100)
        {
            throw std::runtime_error("failed to create temp dir");
        }
    }
}

std::string const&
TempDir::getName() const
{
    return *mImpl;
}


#ifdef _WIN32
#include <Shellapi.h>
void
deltree(std::string const& d)
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
int
callback(char const* name,
         struct stat const* st,
         int flag,
         struct FTW *ftw)
{
    LOG(DEBUG) << "TempDir deleting: " << name;
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
deltree(std::string const& d)
{
    if (nftw(d.c_str(), callback, FOPEN_MAX, FTW_DEPTH) != 0)
    {
        throw std::runtime_error("nftw failed in deltree");
    }
}
#endif

TempDir::~TempDir()
{
    if (mImpl)
    {
        deltree(*mImpl);
        LOG(DEBUG) << "TempDir deleted: " << *mImpl;
        mImpl.reset();
    }
}


}
