// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/SecretManager.h"
#include "util/Logging.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace stellar
{
namespace secretmanager
{

namespace stdfs = std::filesystem;

static std::string const FILE_PREFIX = "$FILE:";

static std::string
rtrim(std::string s)
{
    auto end = s.find_last_not_of(" \t\n\r");
    if (end == std::string::npos)
    {
        return "";
    }
    return s.substr(0, end + 1);
}

static void
checkFilePermissions(std::string const& filePath)
{
    stdfs::path p(filePath);
    auto status = stdfs::status(p);
    if (!stdfs::is_regular_file(status))
    {
        throw std::runtime_error("Secret path is not a regular file: " +
                                 filePath);
    }
    auto perms = status.permissions();
    // Reject if group or others have any access
    auto forbidden = stdfs::perms::group_all | stdfs::perms::others_all;
    if ((perms & forbidden) != stdfs::perms::none)
    {
        throw std::runtime_error(
            "Secret file has overly permissive permissions "
            "(must not be accessible by group or others): " +
            filePath);
    }
}

static std::string
resolveFromFile(std::string const& filePath)
{
    LOG_INFO(DEFAULT_LOG, "Resolving secret from file");
    checkFilePermissions(filePath);
    std::ifstream ifs(filePath);
    if (!ifs.is_open())
    {
        throw std::runtime_error("Cannot open secret file: " + filePath);
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string result = rtrim(ss.str());
    if (result.empty())
    {
        throw std::runtime_error("Secret file is empty: " + filePath);
    }
    return result;
}

std::string
resolve(std::string const& configValue)
{
    if (configValue.substr(0, FILE_PREFIX.size()) == FILE_PREFIX)
    {
        return resolveFromFile(configValue.substr(FILE_PREFIX.size()));
    }
    return configValue;
}

} // namespace secretmanager
} // namespace stellar
