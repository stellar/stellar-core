#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <string>

namespace stellar
{
namespace fs
{

////
// Utility functions for operating on the filesystem.
////

// Whether a path exists
bool exists(std::string const& path);

// Delete a path and everything inside it (if a dir)
void deltree(std::string const& path);

// Make a single dir; not mkdir-p, i.e. non-recursive
bool mkdir(std::string const& path);


////
// Utility functions for constructing path names
////

// Format a 32bit number as an 8-char hex string
std::string hexStr(uint32_t checkpointNum);

// Map any >6 hex char string "ABCDEF..." to the path "AB/CD/EF"
std::string hexDir(std::string const& hexStr);

// Construct the string <type>-<hexstr>.<suffix>
std::string baseName(std::string const& type,
                     std::string const& hexStr,
                     std::string const& suffix);

// Construct the string <type>/hexdir(hexStr)
std::string remoteDir(std::string const& type,
                      std::string const& hexStr);

// Construct the string <type>/hexdir(hexStr)/<type>-<hexstr>.<suffix>
std::string remoteName(std::string const& type,
                       std::string const& hexStr,
                       std::string const& suffix);

}
}
