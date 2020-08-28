#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"

#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace stellar
{
namespace fs
{

// An AWS EBS IOP is 256kb, so we try to write those.
inline constexpr size_t
bufsz()
{
    return 0x40000;
}

// Platform-specific synchronous stream type.
#ifdef _WIN32
using stream_t = asio::windows::stream_handle;
using random_access_t = asio::windows::random_access_handle;
using native_handle_t = HANDLE;
#else
using stream_t = asio::posix::stream_descriptor;
using random_access_t = asio::posix::stream_descriptor;
using native_handle_t = int;
#endif

////
// Utility functions for operating on the filesystem.
////

// raises an exception if a lock file cannot be created
void lockFile(std::string const& path);
// unlocks a file locked with `lockFile`
void unlockFile(std::string const& path);

// Call fsync() on POSIX or FlushFileBuffers() on Win32.
void flushFileChanges(native_handle_t h);

// For completely preposterous reasons, on windows an asio "stream"
// type wrapping a win32 HANDLE is always written-to using an OVERLAPPED
// structure with offset zero, meaning that when you write to a
// file-on-the-disk HANDLE as though it is a stream, you wind up writing all
// data at offset 0, over and over. Instead -- at least on windows and when
// dealing with a file-on-disk -- we need to use a "random access" type
// and track the offset to write next explicitly.
bool shouldUseRandomAccessHandle(std::string const& path);

// Open a native handle (fd or HANDLE) for writing.
native_handle_t openFileToWrite(std::string const& path);

// On POSIX, do rename(src, dst) then open dir and fsync() it
// too: a necessary second step for ensuring durability.
// On Win32, do MoveFileExA with MOVEFILE_WRITE_THROUGH.
bool durableRename(std::string const& src, std::string const& dst,
                   std::string const& dir);

// Whether a path exists
bool exists(std::string const& path);

// Delete a path and everything inside it (if a dir)
void deltree(std::string const& path);

// Make a single dir; not mkdir -p, i.e. non-recursive
bool mkdir(std::string const& path);

// Make a dir path like mkdir -p, i.e. recursive, uses '/' as dir separator
bool mkpath(std::string const& path);

// Get list of all files with names matching predicate
// Returned names are relative to path
std::vector<std::string>
findfiles(std::string const& path,
          std::function<bool(std::string const& name)> predicate);

size_t size(std::ifstream& ifs);

size_t size(std::string const& path);

class PathSplitter
{
  public:
    explicit PathSplitter(std::string path);

    std::string next();
    bool hasNext() const;

  private:
    std::string mPath;
    std::string::size_type mPos;
};

////
// Utility functions for constructing path names
////

// Format a 32bit number as an 8-char hex string
std::string hexStr(uint32_t checkpointNum);

// Map any >6 hex char string "ABCDEF..." to the path "AB/CD/EF"
std::string hexDir(std::string const& hexStr);

// Construct the string <type>-<hexstr>.<suffix>
std::string baseName(std::string const& type, std::string const& hexStr,
                     std::string const& suffix);

// Construct the string <type>/hexdir(hexStr)
std::string remoteDir(std::string const& type, std::string const& hexStr);

// Construct the string <type>/hexdir(hexStr)/<type>-<hexstr>.<suffix>
std::string remoteName(std::string const& type, std::string const& hexStr,
                       std::string const& suffix);

void checkGzipSuffix(std::string const& filename);

void checkNoGzipSuffix(std::string const& filename);

// returns the maximum number of connections that can be done at the same time
int getMaxConnections();
}
}
