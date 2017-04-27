#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <functional>

namespace stellar
{

class LedgerDelta;

class Invariant
{
  public:
    virtual ~Invariant();

    virtual std::string getName() const = 0;
    virtual std::string check(LedgerDelta const& delta) const = 0;
};
}
