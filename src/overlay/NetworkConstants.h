#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <cstdint>

namespace stellar
{
// Maximum size of a single classic transaction in bytes (100KB)
static constexpr uint32_t MAX_CLASSIC_TX_SIZE_BYTES = 100 * 1024;

// Maximum total size of a transaction set (10MB)
static constexpr uint32_t MAX_TX_SET_ALLOWANCE = 10 * 1024 * 1024;

// Maximum Soroban byte allowance (5MB)
static constexpr uint32_t MAX_SOROBAN_BYTE_ALLOWANCE = 5 * 1024 * 1024;

// Maximum classic byte allowance (5MB)
static constexpr uint32_t MAX_CLASSIC_BYTE_ALLOWANCE = 5 * 1024 * 1024;

} // namespace stellar
