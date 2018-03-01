#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Database.h"

namespace stellar
{
namespace DatabaseUtils
{
void deleteOldEntriesHelper(soci::session& sess, uint32_t ledgerSeq,
                            uint32_t count, std::string const& tableName,
                            std::string const& ledgerSeqColumn);
}
}
