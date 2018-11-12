#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"

namespace stellar
{

class Application;

// This Invariant is used to validate that the BucketList and Database are
// in a consistent state after a bucket apply, such as during catchup-minimal.
// It overrides Invariant::checkOnBucketApply, which is called after each
// bucket apply, to iterate over the entries of the bucket and compare them
// to the database. The invariant does not hold if the bucket contains a
// LIVEENTRY which does not match the database (either because it is not
// present or because they are not identical), a DEADENTRY which is found in
// the database, or if the database does not contain the same number of
// entries in the appropriate ledger range as the bucket has LIVEENTRYs.
// The first two conditions show that every entry in the bucket matches the
// database, while the third condition shows that the database does not
// contain any entry in the appropriate ledger range other than those in
// the bucket.
class BucketListIsConsistentWithDatabase : public Invariant
{
  public:
    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    explicit BucketListIsConsistentWithDatabase(Application& app);

    virtual std::string getName() const override;

    virtual std::string checkOnBucketApply(std::shared_ptr<Bucket const> bucket,
                                           uint32_t oldestLedger,
                                           uint32_t newestLedger) override;

  private:
    Application& mApp;
};
}
