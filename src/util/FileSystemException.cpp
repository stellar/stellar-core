// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "FileSystemException.h"
#include <fmt/format.h>

namespace stellar
{

#ifdef _WIN32

std::string
FileSystemException::getLastErrorString()
{
    std::string res;

    DWORD constexpr bufSize = 512;
    char buf[bufSize + 1];
    DWORD dw = ::GetLastError();

    DWORD sz = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                                    FORMAT_MESSAGE_IGNORE_INSERTS,
                                NULL, dw, 0, (LPTSTR)buf, bufSize, NULL);
    buf[sz] = 0;
    res = fmt::format("Error {:#X} - {}", dw, buf);
    return res;
}

void
FileSystemException::failWithGetLastError(std::string msg)
{
    failWith(msg + ", " + getLastErrorString());
}
#endif
}
