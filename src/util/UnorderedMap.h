// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once
#include "util/RandHasher.h"
#include <unordered_map>

namespace stellar
{
template <class KeyT, class ValT, class Hasher = std::hash<KeyT>>
using UnorderedMap =
    std::unordered_map<KeyT, ValT, std::RandHasher<KeyT, Hasher>>;
}
