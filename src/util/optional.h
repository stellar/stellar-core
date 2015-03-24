#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

namespace stellar
{
using namespace std;

template <typename T> using optional = shared_ptr<T>;

template <typename T, class... Args>
optional<T>
make_optional(Args&&... args)
{
    return make_shared<T>(forward<Args>(args)...);
}

template <typename T>
optional<T>
nullopt()
{
    return shared_ptr<T>();
};
}
