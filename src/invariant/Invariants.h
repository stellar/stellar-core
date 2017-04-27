#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxSetFrame.h"

#include <memory>
#include <vector>

namespace stellar
{

class Invariant;
class LedgerDelta;

class Invariants
{
  public:
    explicit Invariants(std::vector<std::unique_ptr<Invariant>> invariants);
    ~Invariants();

    void check(TxSetFramePtr const& txSet, LedgerDelta const& delta) const;

  private:
    std::vector<std::unique_ptr<Invariant>> mInvariants;
};
}
