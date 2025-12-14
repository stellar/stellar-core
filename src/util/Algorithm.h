// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <map>
#include <type_traits>
#include <vector>

template <typename V, typename Extractor,
          typename K = typename std::invoke_result_t<Extractor, V const&>>
std::map<K, std::vector<V>>
split(std::vector<V> const& data, Extractor extractor)
{
    auto r = std::map<K, std::vector<V>>{};
    for (auto&& v : data)
        r[extractor(v)].push_back(v);
    return r;
}
