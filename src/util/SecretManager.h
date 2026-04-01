// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <string>

namespace stellar
{
namespace secretmanager
{

// Resolve a config value that may reference an external secret source.
//
// Supported prefixes:
//   "$FILE:/path/file" - read from file (must have permissions 0600 or
//                         stricter)
//   no prefix          - return the value unchanged (backward compatible)
//
// Throws std::runtime_error on failure (unreadable file, bad permissions,
// empty resolved value).
std::string resolve(std::string const& configValue);

// Returns true if the value uses an external secret reference ($FILE: prefix).
bool isExternalSecret(std::string const& configValue);

} // namespace secretmanager
} // namespace stellar
