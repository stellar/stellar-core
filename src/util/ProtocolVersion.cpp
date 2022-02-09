// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ProtocolVersion.h"

namespace stellar
{
bool
protocolVersionIsBefore(uint32_t protocolVersion, ProtocolVersion beforeVersion)
{
    return protocolVersion < static_cast<uint32_t>(beforeVersion);
}

bool
protocolVersionStartsFrom(uint32_t protocolVersion, ProtocolVersion fromVersion)
{
    return protocolVersion >= static_cast<uint32_t>(fromVersion);
}

bool
protocolVersionEquals(uint32_t protocolVersion, ProtocolVersion equalsVersion)
{
    return protocolVersion == static_cast<uint32_t>(equalsVersion);
}
}
