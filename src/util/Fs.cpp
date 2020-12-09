// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Fs.h"
#include "crypto/Hex.h"
#include "util/FileSystemException.h"
#include "util/Logging.h"
#include <Tracy.hpp>
#include <fmt/format.h>

#include <map>
#include <regex>
#include <sstream>

#ifdef _WIN32
#include <direct.h>

// Latest version of VC++ complains without this define (confused by C++ 17)
#define _SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING 1
#include <experimental/filesystem>

#include <io.h>
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
    ZoneScoped;
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
        throw FileSystemException(errmsg.str());
    }

    lockMap.insert(std::make_pair(path, h));
}

void
unlockFile(std::string const& path)
{
    ZoneScoped;
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

void
flushFileChanges(native_handle_t fh)
{
    ZoneScoped;
    if (FlushFileBuffers(fh) == FALSE)
    {
        FileSystemException::failWithGetLastError(
            "fs::flushFileChanges() failed on _get_osfhandle(): ");
    }
}

bool
shouldUseRandomAccessHandle(std::string const& path)
{
    // Named pipes use stream mode, everything else uses random access.
    return path.find("\\\\.\\pipe\\") != 0;
}

native_handle_t
openFileToWrite(std::string const& path)
{
    ZoneScoped;
    HANDLE h = ::CreateFile(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,                   // DesiredAccess
        FILE_SHARE_READ | FILE_SHARE_WRITE,             // ShareMode
        NULL,                                           // SecurityAttributes
        CREATE_ALWAYS,                                  // CreationDisposition
        (FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED), // FlagsAndAttributes
        NULL);                                          // TemplateFile

    if (h == INVALID_HANDLE_VALUE)
    {
        FileSystemException::failWithGetLastError(
            std::string("fs::openFileToWrite() failed on CreateFile(\"") +
            path + std::string("\"): "));
    }
    return h;
}

bool
durableRename(std::string const& src, std::string const& dst,
              std::string const& dir)
{
    ZoneScoped;
    if (MoveFileExA(src.c_str(), dst.c_str(), MOVEFILE_WRITE_THROUGH) == 0)
    {
        FileSystemException::failWithGetLastError(
            "fs::durableRename() failed on MoveFileExA(): ");
    }
    return true;
}

bool
exists(std::string const& name)
{
    ZoneScoped;
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
            throw FileSystemException(msg + name);
        }
    }
    return true;
}

bool
mkdir(std::string const& name)
{
    ZoneScoped;
    bool b = _mkdir(name.c_str()) == 0;
    CLOG_DEBUG(Fs, "{}{}", (b ? "created dir " : "failed to create dir "),
               name);
    return b;
}

void
deltree(std::string const& d)
{
    ZoneScoped;
    namespace fs = std::experimental::filesystem;
    fs::remove_all(fs::path(d));
}

std::vector<std::string>
findfiles(std::string const& p,
          std::function<bool(std::string const& name)> predicate)
{
    ZoneScoped;
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
    ZoneScoped;
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
        throw FileSystemException(errmsg.str());
    }

    int r = flock(fd, LOCK_EX | LOCK_NB);
    if (r != 0)
    {
        close(fd);
        errmsg << "unable to flock file: " << path << " (" << strerror(errno)
               << ")";
        throw FileSystemException(errmsg.str());
    }

    lockMap.insert(std::make_pair(path, fd));
}

void
unlockFile(std::string const& path)
{
    ZoneScoped;
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

void
flushFileChanges(native_handle_t fd)
{
    ZoneScoped;
    while (fsync(fd) == -1)
    {
        if (errno == EINTR)
        {
            continue;
        }
        FileSystemException::failWithErrno(
            "fs::flushFileChanges() failed on fsync(): ");
    }
}

bool
shouldUseRandomAccessHandle(std::string const& path)
{
    return false;
}

native_handle_t
openFileToWrite(std::string const& path)
{
    ZoneScoped;
    int fd;
    while ((fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644)) ==
           -1)
    {
        if (errno == EINTR)
        {
            continue;
        }
        FileSystemException::failWithErrno(std::string("fs::openFile(\"") +
                                           path + "\") failed: ");
    }
    return fd;
}

bool
durableRename(std::string const& src, std::string const& dst,
              std::string const& dir)
{
    ZoneScoped;
    if (rename(src.c_str(), dst.c_str()) != 0)
    {
        return false;
    }
    int dfd;
    while ((dfd = open(dir.c_str(), O_RDONLY)) == -1)
    {
        if (errno == EINTR)
        {
            continue;
        }
        FileSystemException::failWithErrno(
            std::string("Failed to open directory ") + dir + " :");
    }
    while (fsync(dfd) == -1)
    {
        if (errno == EINTR)
        {
            continue;
        }
        FileSystemException::failWithErrno(
            std::string("Failed to fsync directory ") + dir + " :");
    }
    while (close(dfd) == -1)
    {
        if (errno == EINTR)
        {
            continue;
        }
        FileSystemException::failWithErrno(
            std::string("Failed to close directory ") + dir + " :");
    }
    return true;
}

bool
exists(std::string const& name)
{
    ZoneScoped;
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
            throw FileSystemException(msg + name);
        }
    }
    return true;
}

bool
mkdir(std::string const& name)
{
    ZoneScoped;
    bool b = ::mkdir(name.c_str(), 0700) == 0;
    CLOG_DEBUG(Fs, "{}{}", (b ? "created dir " : "failed to create dir "),
               name);
    return b;
}

namespace
{

int
nftw_deltree_callback(char const* name, struct stat const* st, int flag,
                      struct FTW* ftw)
{
    ZoneScoped;
    CLOG_DEBUG(Fs, "deleting: {}", name);
    if (flag == FTW_DP)
    {
        if (rmdir(name) != 0)
        {
            throw FileSystemException(std::string{"rmdir of "} + name +
                                      " failed");
        }
    }
    else
    {
        if (std::remove(name) != 0)
        {
            throw FileSystemException(std::string{"std::remove of "} + name +
                                      " failed");
        }
    }
    return 0;
}
}

void
deltree(std::string const& d)
{
    ZoneScoped;
    if (nftw(d.c_str(), nftw_deltree_callback, FOPEN_MAX, FTW_DEPTH) != 0)
    {
        throw FileSystemException("nftw failed in deltree for " + d);
    }
}

std::vector<std::string>
findfiles(std::string const& path,
          std::function<bool(std::string const& name)> predicate)
{
    ZoneScoped;
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
    ZoneScoped;
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
    static const std::regex rx(
        "([[:xdigit:]]{2})([[:xdigit:]]{2})([[:xdigit:]]{2}).*");
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
    static const std::string suf(".gz");
    if (!(filename.size() >= suf.size() &&
          equal(suf.rbegin(), suf.rend(), filename.rbegin())))
    {
        throw std::runtime_error("filename does not end in .gz");
    }
}

void
checkNoGzipSuffix(std::string const& filename)
{
    static const std::string suf(".gz");
    if (filename.size() >= suf.size() &&
        equal(suf.rbegin(), suf.rend(), filename.rbegin()))
    {
        throw std::runtime_error("filename ends in .gz");
    }
}

size_t
size(std::ifstream& ifs)
{
    ZoneScoped;
    assert(ifs.is_open());

    ifs.seekg(0, ifs.end);
    auto result = ifs.tellg();
    ifs.seekg(0, ifs.beg);

    return std::max(decltype(result){0}, result);
}

size_t
size(std::string const& filename)
{
    ZoneScoped;
    std::ifstream ifs;
    ifs.open(filename, std::ifstream::binary);
    if (ifs)
    {
        ifs.exceptions(std::ios::badbit);
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
