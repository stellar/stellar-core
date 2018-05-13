// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Fd.h"

#ifndef _WIN32

#include <fcntl.h>

#endif

namespace stellar
{
namespace fd
{

#ifdef _WIN32

// No-op on Windows
bool
disableProcessInheritance(asio::ip::tcp::acceptor& /*acceptor*/)
{
    return true;
}

bool
disableProcessInheritance(asio::ip::tcp::socket& /*socket*/)
{
    return true;
}

bool
disableProcessInheritance(int /*fileDescriptor*/)
{
    return true;
}

#else

bool
disableProcessInheritance(asio::ip::tcp::acceptor& acceptor)
{
    return disableProcessInheritance(acceptor.native_handle());
}

bool
disableProcessInheritance(asio::ip::tcp::socket& socket)
{
    return disableProcessInheritance(socket.native_handle());
}

bool
disableProcessInheritance(int fileDescriptor)
{
    int flags = fcntl(fileDescriptor, F_GETFD);
    if (flags != -1)
    {
        return fcntl(fileDescriptor, F_SETFD, flags | FD_CLOEXEC) != -1;
    }
    return false;
}

#endif
}
}
