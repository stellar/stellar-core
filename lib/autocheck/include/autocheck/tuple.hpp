#ifndef AUTOCHECK_TUPLE_HPP
#define AUTOCHECK_TUPLE_HPP

#include "ostream.hpp"

namespace autocheck {
  namespace detail {

    template <typename... Ts>
    void print(std::ostream& out, const std::tuple<Ts...>& tup,
        const std::integral_constant<size_t, 0>&)
    {
      out << std::get<0>(tup);
    }

    template <size_t I, typename... Ts>
    void print(std::ostream& out, const std::tuple<Ts...>& tup,
        const std::integral_constant<size_t, I>&
          = std::integral_constant<size_t, I>())
    {
      print(out, tup, std::integral_constant<size_t, I - 1>());
      out << ", ";
      out << std::get<I>(tup);
    }

  }

  template <typename... Ts>
  std::ostream& operator<< (std::ostream& out, const std::tuple<Ts...>& tup) {
    out << "(";
    autocheck::detail::print(out, tup,
        std::integral_constant<size_t, sizeof...(Ts) - 1>());
    out << ")";
    return out;
  }

}

#endif

