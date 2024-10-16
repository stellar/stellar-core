#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <cstdint>
namespace stellar
{
// This is a set of utilities for checking the ledger protocol versions in
// expressive and searchable fashion.

enum class ProtocolVersion : uint32_t
{
    V_0 = 0,
    V_1,
    V_2,
    V_3,
    V_4,
    V_5,
    V_6,
    V_7,
    V_8,
    V_9,
    V_10,
    V_11,
    V_12,
    V_13,
    V_14,
    V_15,
    V_16,
    V_17,
    V_18,
    V_19,
    V_20,
    V_21,
    V_22,
    V_23
};

// Checks whether provided protocolVersion is before (i.e. strictly lower than)
// beforeVersion.
bool protocolVersionIsBefore(uint32_t protocolVersion,
                             ProtocolVersion beforeVersion);
// Checks whether provided protocolVersion starts from (i.e. is greater than or
// equal to) fromVersion.
bool protocolVersionStartsFrom(uint32_t protocolVersion,
                               ProtocolVersion fromVersion);
// Checks whether provided protocolVersion is exactly equalsVersion.
bool protocolVersionEquals(uint32_t protocolVersion,
                           ProtocolVersion equalsVersion);

constexpr ProtocolVersion SOROBAN_PROTOCOL_VERSION = ProtocolVersion::V_20;
constexpr ProtocolVersion PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION =
    ProtocolVersion::V_23;
}
