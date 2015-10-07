#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <string>
#include <vector>
#include <cstdint>

namespace stellar
{

struct SrvRecord
{
    std::uint16_t mTtl;
    std::uint16_t mPriority;
    std::uint16_t mWeight;
    std::uint16_t mPort;
    std::string mHostname;
    std::string hostAndPort() const;
};

std::vector<SrvRecord>
getSrvRecords(std::string const& qname);

SrvRecord
pickSrvRecordByPriorityAndWeight(std::vector<SrvRecord> const& records);

}
