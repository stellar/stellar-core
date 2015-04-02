#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>

namespace stellar
{

#if __cplusplus >= 201402L || (defined(_MSC_VER) && _MSC_VER >= 1900)

using std::make_unique;

#else
template <typename T, typename... Args>
std::unique_ptr<T>
make_unique(Args&&... args)
{
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}
#endif
}
