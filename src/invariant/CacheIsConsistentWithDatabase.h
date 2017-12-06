#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"
#include <memory>

namespace stellar
{

class Application;
class Database;
class LedgerDelta;

// This Invariant is used to validate that the changes in LedgerDelta match the
// changes in the Database. It is important to note that this invariant does
// not check the converse, meaning it does not check that all changes in the
// Database match the changes in the LedgerDelta. One specific consequence of
// this is that a change made to the Database by an external process will not
// be caught. This invariant makes it possible to write the Invariants at the
// Operation level without querying the Database, since they can instead rely
// on the correctness of the LedgerDelta.
class CacheIsConsistentWithDatabase : public Invariant
{
  public:
    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    explicit CacheIsConsistentWithDatabase(Database& db);

    virtual std::string getName() const override;

    virtual std::string
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& result,
                          LedgerDelta const& delta) override;

  private:
    Database& mDb;
};
}
