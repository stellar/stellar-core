#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <string>

namespace stellar
{

/**
 * Some values should not be printed to log directly - for example database
 * connection strings with password or secret keys.
 *
 * Using this simple wrapper allows to prevent using these values as a normal
 * string as it requires extra code to read the value.
 */
struct SecretValue
{
    std::string value;
};

bool operator==(SecretValue const& x, SecretValue const& y);
bool operator!=(SecretValue const& x, SecretValue const& y);
}
