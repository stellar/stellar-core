#ifndef MAKE_UNIQUE_H
#define MAKE_UNIQUE_H
#include <memory>

namespace stellar {

#if __cplusplus >= 201402L
    using std::make_unique;

#else
    template <typename T, typename ... Args>
    std::unique_ptr<T>
    make_unique(Args && ... args)
    {
        return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
    }
}
#endif

#endif
