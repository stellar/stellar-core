#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>

namespace stellar
{

class Application;
class BucketList;
struct LedgerHeader;

class CLFMaster
{
    class Impl;
    std::unique_ptr<Impl> mImpl;

  public:
    CLFMaster(Application&);
    ~CLFMaster();
    LedgerHeader const& getHeader();
    BucketList const& getBucketList();
};
}


