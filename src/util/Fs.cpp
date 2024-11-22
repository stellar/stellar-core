// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Fs.h"
#include "crypto/Hex.h"
#include "util/FileSystemException.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include <Tracy.hpp>
#include <cstdint>
#include <filesystem>
#include <fmt/format.h>

#include <map>
#include <regex>
#include <sstream>

#ifdef _WIN32
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys\stat.h>
#include <sys\types.h>

#include <direct.h>

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

native_handle_t
openFileToWrite(std::string const& path)
{
    ZoneScoped;
    HANDLE h = ::CreateFile(path.c_str(),
                            GENERIC_READ | GENERIC_WRITE,       // DesiredAccess
                            FILE_SHARE_READ | FILE_SHARE_WRITE, // ShareMode
                            NULL,                  // SecurityAttributes
                            CREATE_ALWAYS,         // CreationDisposition
                            FILE_ATTRIBUTE_NORMAL, // FlagsAndAttributes
                            NULL);                 // TemplateFile

    if (h == INVALID_HANDLE_VALUE)
    {
        FileSystemException::failWithGetLastError(
            std::string("fs::openFileToWrite() failed on CreateFile(\"") +
            path + std::string("\"): "));
    }
    return h;
}

FILE*
fdOpen(native_handle_t h)
{
    FILE* res;
    int const fd =
        ::_open_osfhandle(reinterpret_cast<::intptr_t>(h), _O_APPEND);
    if (-1 != fd)
    {
        res = ::_fdopen(fd, "wb");
        if (res == NULL)
        {
            ::_close(fd);
        }
    }
    else
    {
        ::CloseHandle(h);
        res = NULL;
    }
    return res;
}

bool
durableRename(std::string const& src, std::string const& dst,
              std::string const& dir)
{
    ZoneScoped;
    if (MoveFileExA(src.c_str(), dst.c_str(),
                    MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING) == 0)
    {
        FileSystemException::failWithGetLastError(
            "fs::durableRename() failed on MoveFileExA(): ");
    }
    return true;
}

#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
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
    std::error_code ec;
    std::filesystem::rename(src.c_str(), dst.c_str(), ec);
    if (ec)
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
#endif

namespace stdfs = std::filesystem;

bool
exists(std::string const& name)
{
    ZoneScoped;
    return stdfs::exists(stdfs::path(name));
}

bool
mkdir(std::string const& name)
{
    ZoneScoped;
    bool ok = stdfs::create_directory(stdfs::path(name));
    CLOG_DEBUG(Fs, "{}{}", (ok ? "created dir " : "failed to create dir "),
               name);
    return ok;
}

void
deltree(std::string const& d)
{
    ZoneScoped;
    stdfs::remove_all(stdfs::path(d));
}

std::vector<std::string>
findfiles(std::string const& p,
          std::function<bool(std::string const& name)> predicate)
{
    ZoneScoped;
    std::vector<std::string> res;
    for (auto& entry : stdfs::directory_iterator(stdfs::path(p)))
    {
        if (stdfs::is_regular_file(entry.status()))
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

bool
mkpath(const std::string& path)
{
    ZoneScoped;
    auto p = stdfs::path(path);
    stdfs::create_directories(p);
    return stdfs::exists(p) && stdfs::is_directory(p);
}

std::string
hexStr(uint32_t checkpointNum)
{
    return fmt::format(FMT_STRING("{:08x}"), checkpointNum);
}

std::string
hexDir(std::string const& hexStr)
{
    static const std::regex rx(
        "([[:xdigit:]]{2})([[:xdigit:]]{2})([[:xdigit:]]{2}).*");
    std::smatch sm;
    bool matched = std::regex_match(hexStr, sm, rx);
    releaseAssert(matched);
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
    if (std::filesystem::path(filename).extension().string() != suf)
    {
        throw std::runtime_error("filename does not end in .gz");
    }
}

void
checkNoGzipSuffix(std::string const& filename)
{
    static const std::string suf(".gz");
    if (std::filesystem::path(filename).extension().string() == suf)
    {
        throw std::runtime_error("filename ends in .gz");
    }
}

size_t
size(std::ifstream& ifs)
{
    ZoneScoped;
    releaseAssert(ifs.is_open());

    ifs.seekg(0, ifs.end);
    auto result = ifs.tellg();
    ifs.seekg(0, ifs.beg);

    return std::max(decltype(result){0}, result);
}

size_t
size(std::string const& filename)
{
    ZoneScoped;
    return stdfs::file_size(stdfs::path(filename));
}

#ifdef _WIN32

int64_t
getMaxHandles()
{
    // on Windows, there is no limit on handles
    // only limits based on ephemeral ports, etc
    return 32000;
}

#else
int64_t
getMaxHandles()
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

#if defined(_WIN32)
int64_t
getOpenHandleCount()
{
    HANDLE proc =
        OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    if (proc)
    {
        DWORD count{0};
        if (GetProcessHandleCount(proc, &count))
        {
            return static_cast<int64_t>(count);
        }
        CloseHandle(proc);
    }
    return 0;
}
#elif defined(__APPLE__)
int64_t
getOpenHandleCount()
{
    int64_t n{0};
    for (auto const& _fd : std::filesystem::directory_iterator("/dev/fd"))
    {
        std::ignore = _fd;
        ++n;
    }
    return n;
}
#elif defined(__linux__)
int64_t
getOpenHandleCount()
{
    int64_t n{0};
    for (auto const& _fd : std::filesystem::directory_iterator("/proc/self/fd"))
    {
        std::ignore = _fd;
        ++n;
    }
    return n;
}
#else
int64_t
getOpenHandleCount()
{
    return 0;
}
#endif

}
}
