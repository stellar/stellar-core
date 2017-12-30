#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <string>

namespace stellar
{
namespace fs
{

////
// Utility for manipulating process ids
////

long getCurrentPid();

bool processExists(long pid);

////
// Utility functions for operating on the filesystem.
////

// raises an exception if a lock file cannot be created
void lockFile(std::string const& path);
// unlocks a file locked with `lockFile`
void unlockFile(std::string const& path);

// Whether a path exists
bool exists(std::string const& path);

// Delete a path and everything inside it (if a dir)
void deltree(std::string const& path);

// Make a single dir; not mkdir -p, i.e. non-recursive
bool mkdir(std::string const& path);

// Make a dir path like mkdir -p, i.e. recursive, uses '/' as dir separator
bool mkpath(std::string const& path);

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
}
}
