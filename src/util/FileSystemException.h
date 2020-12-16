#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Logging.h"
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace stellar
{

class FileSystemException : public std::runtime_error
{
  public:
    static void
    failWith(std::string msg)
    {
        CLOG_FATAL(Fs, "{}", msg);
        throw FileSystemException(msg);
    }
    static void
    failWithErrno(std::string msg)
    {
        failWith(msg + std::strerror(errno));
    }
#ifdef _WIN32
    static std::string getLastErrorString();
    static void failWithGetLastError(std::string msg);
#endif // _WIN32
    explicit FileSystemException(std::string const& msg)
        : std::runtime_error{msg}
    {
    }
    virtual ~FileSystemException() = default;
};
}
