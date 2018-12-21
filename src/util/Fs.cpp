// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Fs.h"
#include "crypto/Hex.h"
#include "lib/util/format.h"
#include "util/Logging.h"
#include <map>
#include <regex>
#include <sstream>

#ifdef _WIN32
#include <direct.h>
#include <filesystem>
#else
#include <dirent.h>
#include <sys/resource.h>
#include <sys/stat.h>
#endif

#include <cstdio>

namespace stellar
{

namespace fs
{

#ifdef _WIN32
#include <Shellapi.h>
#include <Windows.h>
#include <psapi.h>

static std::map<std::string, HANDLE> lockMap;

void
lockFile(std::string const& path)
{
    std::ostringstream errmsg;

    if (lockMap.find(path) != lockMap.end())
    {
        errmsg << "file already locked by this process: " << path;
        throw std::runtime_error(errmsg.str());
    }
    HANDLE h = ::CreateFile(path.c_str(), GENERIC_WRITE,
                            0, // don't allow sharing
                            NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY |
                                FILE_FLAG_DELETE_ON_CLOSE,
                            NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        // not sure if there is more verbose info that can be obtained here
        errmsg << "unable to create lock file: " << path;
        throw std::runtime_error(errmsg.str());
    }

    lockMap.insert(std::make_pair(path, h));
}

void
unlockFile(std::string const& path)
{
    auto it = lockMap.find(path);
    if (it != lockMap.end())
    {
        ::CloseHandle(it->second);
        lockMap.erase(it);
    }
    else
    {
        throw std::runtime_error("file was not locked");
    }
}

bool
exists(std::string const& name)
{
    if (name.empty())
        return false;

    if (GetFileAttributes(name.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (GetLastError() == ERROR_FILE_NOT_FOUND ||
            GetLastError() == ERROR_PATH_NOT_FOUND)
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
mkdir(std::string const& name)
{
    bool b = _mkdir(name.c_str()) == 0;
    CLOG(DEBUG, "Fs") << (b ? "created dir " : "failed to create dir ") << name;
    return b;
}

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

std::vector<std::string>
findfiles(std::string const& p,
          std::function<bool(std::string const& name)> predicate)
{
    using namespace std;
    namespace fs = std::experimental::filesystem;

    std::vector<std::string> res;
    for (auto& entry : fs::directory_iterator(fs::path(p)))
    {
        if (fs::is_regular_file(entry.status()))
        {
            auto n = entry.path().filename().string();
            if (predicate(n))
            {
                res.emplace_back(n);
            }
        }
    }
    return res;
}

long
getCurrentPid()
{
    return static_cast<long>(GetCurrentProcessId());
}

bool
processExists(long pid)
{
    std::vector<DWORD> buffer(4096);
    DWORD bytesWritten;
    for (;;)
    {
        if (!EnumProcesses(buffer.data(),
                           static_cast<DWORD>(buffer.size() * sizeof(DWORD)),
                           &bytesWritten))
        {
            throw std::runtime_error("EnumProcess failed");
        }
        if (bytesWritten / sizeof(DWORD) < buffer.size())
        {
            auto found = std::find(buffer.begin(), buffer.end(),
                                   static_cast<DWORD>(pid));
            return !(found == buffer.end());
        }
        // Need a larger buffer to hold all the ids.
        buffer.resize(buffer.size() * 2);
    }
}

#else
#include <cerrno>
#include <fcntl.h>
#include <ftw.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static std::map<std::string, int> lockMap;

void
lockFile(std::string const& path)
{
    std::ostringstream errmsg;

    if (lockMap.find(path) != lockMap.end())
    {
        errmsg << "file is already locked by this process: " << path;
        throw std::runtime_error(errmsg.str());
    }
    int fd = open(path.c_str(), O_RDWR | O_CREAT, S_IRWXU);

    if (fd == -1)
    {
        errmsg << "unable to open lock file: " << path << " ("
               << strerror(errno) << ")";
        throw std::runtime_error(errmsg.str());
    }

    int r = flock(fd, LOCK_EX | LOCK_NB);
    if (r != 0)
    {
        close(fd);
        errmsg << "unable to flock file: " << path << " (" << strerror(errno)
               << ")";
        throw std::runtime_error(errmsg.str());
    }

    lockMap.insert(std::make_pair(path, fd));
}

void
unlockFile(std::string const& path)
{
    auto it = lockMap.find(path);
    if (it != lockMap.end())
    {
        // cannot unlink to avoid potential race
        close(it->second);
        lockMap.erase(it);
    }
    else
    {
        throw std::runtime_error("file was not locked");
    }
}

bool
exists(std::string const& name)
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
mkdir(std::string const& name)
{
    bool b = ::mkdir(name.c_str(), 0700) == 0;
    CLOG(DEBUG, "Fs") << (b ? "created dir " : "failed to create dir ") << name;
    return b;
}

namespace
{

int
nftw_deltree_callback(char const* name, struct stat const* st, int flag,
                      struct FTW* ftw)
{
    CLOG(DEBUG, "Fs") << "deleting: " << name;
    if (flag == FTW_DP)
    {
        if (rmdir(name) != 0)
        {
            throw std::runtime_error(std::string{"rmdir of "} + name +
                                     " failed");
        }
    }
    else
    {
        if (std::remove(name) != 0)
        {
            throw std::runtime_error(std::string{"std::remove of "} + name +
                                     " failed");
        }
    }
    return 0;
}
}

void
deltree(std::string const& d)
{
    if (nftw(d.c_str(), nftw_deltree_callback, FOPEN_MAX, FTW_DEPTH) != 0)
    {
        throw std::runtime_error("nftw failed in deltree for " + d);
    }
}

std::vector<std::string>
findfiles(std::string const& path,
          std::function<bool(std::string const& name)> predicate)
{
    auto dir = opendir(path.c_str());
    auto result = std::vector<std::string>{};
    if (!dir)
    {
        return result;
    }

    try
    {
        while (auto entry = readdir(dir))
        {
            auto name = std::string{entry->d_name};
            if (predicate(name))
            {
                result.push_back(name);
            }
        }

        closedir(dir);
        return result;
    }
    catch (...)
    {
        // small RAII class could do here
        closedir(dir);
        throw;
    }
}

long
getCurrentPid()
{
    return static_cast<long>(getpid());
}

bool
processExists(long pid)
{
    return (kill(pid, 0) == 0);
}

#endif

PathSplitter::PathSplitter(std::string path) : mPath{std::move(path)}, mPos{0}
{
}

std::string
PathSplitter::next()
{
    auto slash = mPath.find('/', mPos);
    mPos = slash == std::string::npos ? mPath.length() : slash;
    auto r = mPos == 0 ? "/" : mPath.substr(0, mPos);
    mPos++;
    auto mLastSlash = mPos;
    while (mLastSlash < mPath.length() && mPath[mLastSlash] == '/')
        mLastSlash++;
    if (mLastSlash > mPos)
        mPath.erase(mPos, mLastSlash - mPos);
    return r;
}

bool
PathSplitter::hasNext() const
{
    return mPos < mPath.length();
}

bool
mkpath(const std::string& path)
{
    auto splitter = PathSplitter{path};
    while (splitter.hasNext())
    {
        auto subpath = splitter.next();
        if (!exists(subpath) && !mkdir(subpath))
        {
            return false;
        }
    }

    return true;
}

std::string
hexStr(uint32_t checkpointNum)
{
    return fmt::format("{:08x}", checkpointNum);
}

std::string
hexDir(std::string const& hexStr)
{
    std::regex rx("([[:xdigit:]]{2})([[:xdigit:]]{2})([[:xdigit:]]{2}).*");
    std::smatch sm;
    bool matched = std::regex_match(hexStr, sm, rx);
    assert(matched);
    return (std::string(sm[1]) + "/" + std::string(sm[2]) + "/" +
            std::string(sm[3]));
}

std::string
baseName(std::string const& type, std::string const& hexStr,
         std::string const& suffix)
{
    return type + "-" + hexStr + "." + suffix;
}

std::string
remoteDir(std::string const& type, std::string const& hexStr)
{
    return type + "/" + hexDir(hexStr);
}

std::string
remoteName(std::string const& type, std::string const& hexStr,
           std::string const& suffix)
{
    return remoteDir(type, hexStr) + "/" + baseName(type, hexStr, suffix);
}

void
checkGzipSuffix(std::string const& filename)
{
    std::string suf(".gz");
    if (!(filename.size() >= suf.size() &&
          equal(suf.rbegin(), suf.rend(), filename.rbegin())))
    {
        throw std::runtime_error("filename does not end in .gz");
    }
}

void
checkNoGzipSuffix(std::string const& filename)
{
    std::string suf(".gz");
    if (filename.size() >= suf.size() &&
        equal(suf.rbegin(), suf.rend(), filename.rbegin()))
    {
        throw std::runtime_error("filename ends in .gz");
    }
}

size_t
size(std::ifstream& ifs)
{
    assert(ifs.is_open());

    ifs.seekg(0, ifs.end);
    auto result = ifs.tellg();
    ifs.seekg(0, ifs.beg);

    return std::max(decltype(result){0}, result);
}

size_t
size(std::string const& filename)
{
    std::ifstream ifs;
    ifs.open(filename, std::ifstream::binary);
    if (ifs)
    {
        return size(ifs);
    }
    else
    {
        return 0;
    }
}

#ifdef _WIN32

int
getMaxConnections()
{
    // on Windows, there is no limit on handles
    // only limits based on ephemeral ports, etc
    return 32000;
}

#else
int
getMaxConnections()
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
    {
        // leave some buffer
        return (rl.rlim_cur * 3) / 4;
    }
    // could not query the limit, default to a value that should work
    return 64;
}
#endif
}
}
