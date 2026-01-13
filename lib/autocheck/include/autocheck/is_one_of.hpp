#ifndef AUTOCHECK_IS_ONE_OF_HPP
#define AUTOCHECK_IS_ONE_OF_HPP

#include <type_traits>

/* Template metaprogram to find a type in a list of types. */

namespace autocheck {

  template <typename T, typename... Us>
  struct is_one_of : public std::disjunction<
    std::is_same<
      typename std::decay<T>::type,
      typename std::decay<Us>::type
    >...
  > {};

}

#endif

