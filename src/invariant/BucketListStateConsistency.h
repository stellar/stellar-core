#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"

namespace stellar
{
class Application;

class BucketListStateConsistency : public Invariant
{
  public:
    BucketListStateConsistency();
    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    virtual std::string getName() const override;

    virtual std::string
    checkSnapshot(SearchableSnapshotConstPtr liveSnapshot,
                  SearchableHotArchiveSnapshotConstPtr hotArchiveSnapshot,
                  InMemorySorobanState const& inMemorySnapshot,
                  std::function<bool()> isStopping) override;
};
}
