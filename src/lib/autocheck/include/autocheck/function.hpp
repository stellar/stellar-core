#ifndef AUTOCHECK_FUNCTION_HPP
#define AUTOCHECK_FUNCTION_HPP

#include <functional>

namespace autocheck {

  /* Generic identity function. */
  struct id {
    template <typename T>
    T&& operator() (T&& t) const { return std::forward<T>(t); }
  };

  struct always {
    template <typename... Args>
    bool operator() (const Args&...) const { return true; }
  };

  struct never {
    template <typename... Args>
    bool operator() (const Args&...) const { return false; }
  };

  template <typename... Args>
  struct predicate {
    typedef std::function<bool (const Args&...)> type;
  };

  /* Type of functions that adjust size of generated value. */
  typedef std::function<size_t (size_t)> resize_t;

}

#endif

