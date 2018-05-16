#pragma once

#include "util/asio.h"

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{
namespace fd
{

bool disableProcessInheritance(asio::ip::tcp::acceptor& acceptor);
bool disableProcessInheritance(asio::ip::tcp::socket& socket);
bool disableProcessInheritance(int fileDescriptor);
}
}
