#pragma once

namespace stellar
{

template<typename T>
using optional = std::shared_ptr<T>;


template<typename T, class... Args>
optional<T> make_optional(Args&&... args) 
{
   return make_shared<T>(std::forward<Args>(args)...);
}

template<typename T>
optional<T> nullopt() { return std::shared_ptr<T>(); };


}
