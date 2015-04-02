#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

/*
parent of QuorumSet and TransactionSet
*/

namespace stellar
{
class FetchableItem
{
    uint256 mItemID;

  public:
    uint256 getItemID();

    virtual Message::pointer createMessage() = 0;
};
}
