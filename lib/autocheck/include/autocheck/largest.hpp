#ifndef AUTOCHECK_LARGEST_HPP
#define AUTOCHECK_LARGEST_HPP

#include <type_traits>

/* Template metaprogram to find the largest in a list of types. */

namespace autocheck {

  template <typename Head, typename... Tail>
  struct largest1 :
    std::conditional<
      (sizeof(Head) > sizeof(typename largest1<Tail...>::type)),
      Head,
      typename largest1<Tail...>::type
    >
  {};

  template <typename Only>
  struct largest1<Only> {
    typedef Only type;
  };

  template <typename... Ts>
  struct largest : largest1<Ts...> {
    enum { size = sizeof(typename largest::type) };
  };

}

#endif

