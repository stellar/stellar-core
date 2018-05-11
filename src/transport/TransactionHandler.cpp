// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transport/TransactionHandler.h"

namespace stellar
{

const char* TransactionHandler::TX_STATUS_STRING[TX_STATUS_COUNT] = {
    "PENDING", "DUPLICATE", "ERROR"};

TransactionHandler::~TransactionHandler()
{
}
}
