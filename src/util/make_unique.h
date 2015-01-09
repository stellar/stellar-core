#ifndef __MAKE_UNIQUE__
#define __MAKE_UNIQUE__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>

namespace stellar
{

#if __cplusplus >= 201402L || ( defined(_MSC_VER) && _MSC_VER >= 1900 )

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
#endif

