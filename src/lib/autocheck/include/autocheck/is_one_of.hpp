#ifndef AUTOCHECK_IS_ONE_OF_HPP
#define AUTOCHECK_IS_ONE_OF_HPP

#include <type_traits>

/* Template metaprogram to find a type in a list of types. */

namespace autocheck {

  template <typename T, typename... Tail>
  struct is_one_of : std::false_type {};

  template <typename T, typename Head, typename... Tail>
  struct is_one_of<T, Head, Tail...> :
    std::conditional<
      std::is_same<
        typename std::decay<T>::type,
        typename std::decay<Head>::type
      >::value,
      std::true_type,
      is_one_of<T, Tail...>
    >::type
  {};

}

#endif

