#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <lib/soci/src/backends/sqlite3/soci-sqlite3.h>
#ifdef USE_POSTGRES
#include <lib/soci/src/backends/postgresql/soci-postgresql.h>
#endif

// Just a visitor type to help write code that's database-specific.
// See Database::doDatabaseTypeSpecificOperation.
namespace stellar
{
template <typename T = void> class DatabaseTypeSpecificOperation
{
  public:
    virtual T doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) = 0;
#ifdef USE_POSTGRES
    virtual T
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) = 0;
#endif
};
}
