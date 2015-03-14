#pragma once

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
