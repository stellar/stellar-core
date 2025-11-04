#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"

namespace stellar
{
class Application;

class InMemorySorobanStateMatchesBucketList : public Invariant
{
  public:
    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    InMemorySorobanStateMatchesBucketList();

    virtual std::string getName() const override;

    virtual std::string checkOnLedgerCommit(
        SearchableSnapshotConstPtr lclLiveState,
        SearchableHotArchiveSnapshotConstPtr lclHotArchiveState,
        LedgerCommitState const& commitState,
        InMemorySorobanState const& inMemorySorobanState) override;
};
}
