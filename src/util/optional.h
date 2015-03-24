#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

namespace stellar
{
template <typename T> using optional = std::shared_ptr<T>;

template <typename T, class... Args>
optional<T>
make_optional(Args&&... args)
{
    return std::make_shared<T>(std::forward<Args>(args)...);
}

template <typename T>
optional<T>
nullopt()
{
    return std::shared_ptr<T>();
};
}
