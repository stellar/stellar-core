#ifndef AUTOCHECK_GENERATOR_HPP
#define AUTOCHECK_GENERATOR_HPP

#include <random>
#include <vector>
#include <iterator>
#include <limits>
#include <algorithm>

#include "is_one_of.hpp"
#include "function.hpp"
#include "generator_combinators.hpp"
#include "apply.hpp"

namespace autocheck {

  /* Reusable static standard random number generator. */
  inline std::mt19937& rng() {
    static std::random_device rd;
    static std::mt19937 rng(rd());
    return rng;
  }

  template <typename T, typename... Gens, int... Is>
  T generate(std::tuple<Gens...>& gens, size_t size,
      const range<0, Is...>&)
  {
    return T(std::get<Is>(gens)(size)...);
  }

  template <typename T, typename... Gens>
  T generate(std::tuple<Gens...>& gens, size_t size) {
    return generate<T>(gens, size, range<sizeof...(Gens)>());
  }

  /* Generators produce an infinite sequence. */

  template <typename T, typename Enable = void>
  class generator;

  template <>
  class generator<bool> {
    public:
      typedef bool result_type;

      result_type operator() (size_t = 0) {
        static std::bernoulli_distribution dist(0.5);
        return dist(rng());
      }
  };

  namespace detail {

    static const char alnums[]  = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz"
      "0123456789";
    /* Subtract 1 for NUL terminator. */
    static const size_t nalnums = sizeof(alnums) - 1;
    static const size_t nprint  = '~' - ' ' + 1;

  }

  enum CharCategory {
    ccAlphaNumeric,
    ccPrintable,
    ccAny
  };

  template <typename CharType, CharCategory Category = ccPrintable>
  class char_generator {
    public:
      typedef CharType result_type;

      result_type operator() (size_t size = 0) {
        if (Category == ccAlphaNumeric || size < detail::nalnums) {
          size = detail::nalnums - 1;
        } else if (Category == ccPrintable || size < detail::nprint) {
          size = detail::nprint - 1;
        } else {
          size = std::numeric_limits<CharType>::max();
        }
        /* Distribution is non-static. */
        std::uniform_int_distribution<size_t> dist(0, size);
        auto i = dist(rng());
        auto rv =
          (size < detail::nalnums) ? detail::alnums[i] :
          ((size < detail::nprint) ? ' ' + i :
           i);
        return static_cast<result_type>(rv);
      }
  };

  /* WARNING: wchar_t, char16_t, char32_t, and family are typedefs. They
   * cannot be distinguished from regular (un)signed integrals, meaning we
   * cannot provide a general `generator` for them with the special char
   * consideration of size. If you want that consideration, use
   * char_generator specifically. */

  template <typename CharType>
  class generator<
    CharType,
    typename std::enable_if<
      is_one_of<CharType, unsigned char, char, signed char>::value
    >::type
  > : public char_generator<CharType> {};

  template <typename UnsignedIntegral>
  class generator<
    UnsignedIntegral,
    typename std::enable_if<
      is_one_of<UnsignedIntegral, unsigned short, unsigned int,
        unsigned long, unsigned long long>::value
    >::type
  >
  {
    public:
      typedef UnsignedIntegral result_type;

      result_type operator() (size_t size = 0) {
        /* Distribution is non-static. */
        std::uniform_int_distribution<UnsignedIntegral> dist(0, static_cast<UnsignedIntegral>(size));
        auto rv = dist(rng());
        return rv;
      }
  };

  template <typename SignedIntegral>
  class generator<
    SignedIntegral,
    typename std::enable_if<
      is_one_of<SignedIntegral, short, int, long, long long>::value
    >::type
  >
  {
    public:
      typedef SignedIntegral result_type;

      result_type operator() (size_t size = 0) {
        auto s = static_cast<SignedIntegral>(size >> 1);
        /* Distribution is non-static. */
        std::uniform_int_distribution<SignedIntegral> dist(-s, s);
        auto rv = dist(rng());
        return rv;
      }
  };

  template <typename Floating>
  class generator<
    Floating,
    typename std::enable_if<
      is_one_of<Floating, float, double>::value
    >::type
  >
  {
    public:
      typedef Floating result_type;

      result_type operator() (size_t size = 0) {
        /* Distribution is non-static. */
        Floating f_size = static_cast<Floating>(size);
        std::uniform_real_distribution<Floating> dist(-f_size, f_size);
        auto rv = dist(rng());
        return rv;
      }
  };

  template <typename CharGen = generator<char>>
  class string_generator {
    private:
      CharGen chargen;

    public:
      string_generator(const CharGen& chargen = CharGen()) :
        chargen(chargen) {}

      typedef std::basic_string<typename CharGen::result_type> result_type;

      result_type operator() (size_t size = 0) {
        result_type rv;
        rv.reserve(size);
        std::generate_n(std::back_insert_iterator<result_type>(rv), size,
            /* Scale characters faster than string size. */
            fix(size << 2, chargen));
        return rv;
      }
  };

  template <
    CharCategory Category = ccPrintable,
    typename CharType     = char
  >
  string_generator<char_generator<CharType, Category>> string() {
    return string_generator<char_generator<CharType, Category>>();
  }

  template <typename CharGen>
  string_generator<CharGen> string(const CharGen& chargen) {
    return string_generator<CharGen>();
  }

  template <typename CharType>
  class generator<std::basic_string<CharType>> :
    public string_generator<generator<CharType>> {};

  /* TODO: Generic sequence generator. */

  inline size_t elt_size(size_t nelts, size_t size)
  {
      switch (nelts)
      {
      case 0:
          return 0;
      case 1:
          return size >> 1;
      default:
          return size / (nelts >> 1);
      }
  }

  template <typename Gen>
  class list_generator {
    private:
      Gen eltgen;

    public:

      typedef std::vector<typename Gen::result_type> result_type;

      list_generator(const Gen& eltgen = Gen()) :
        eltgen(eltgen) {}

      result_type operator() (size_t size = 0) {
        result_type rv;
        rv.reserve(size);
        size_t eltsize = elt_size(size, size);
        std::generate_n(std::back_insert_iterator<result_type>(rv), size,
            fix(eltsize, eltgen));
        return rv;
      }
  };

  template <typename Gen>
  list_generator<Gen> list_of(const Gen& gen) {
    return list_generator<Gen>(gen);
  }

  template <typename T, typename Gen = generator<T>>
  list_generator<Gen> list_of() {
    return list_of(Gen());
  }

  template <typename T>
  class generator<std::vector<T>> : public list_generator<generator<T>> {};

  /* Ordered list combinator. */

  namespace detail {

    struct sorter {
      template <typename T>
      std::vector<T> operator() (std::vector<T>&& a, size_t) {
        std::sort(a.begin(), a.end());
        return a;
      }
    };

  }

  template <typename Gen>
  detail::mapped_generator<detail::sorter, list_generator<Gen>>
  ordered_list(const Gen& gen) {
    return map(detail::sorter(), list_of(gen));
  }

  template <typename T, typename Gen = generator<T>>
  detail::mapped_generator<detail::sorter, list_generator<Gen>>
  ordered_list() {
    return ordered_list(Gen());
  }

  /* Generic type generator (by construction). */

  template <typename T, typename... Gens>
  class cons_generator {
    private:
      std::tuple<Gens...> gens;

    public:
      cons_generator() :
        gens(Gens()...) {}

      cons_generator(const Gens&... gens) :
        gens(gens...) {}

      typedef T result_type;

      result_type operator() (size_t size = 0) {
        return generate<result_type>(gens, (size > 0) ? (size - 1) : size);
      }
  };

  template <typename T, typename... Gens>
  cons_generator<T, Gens...> cons(const Gens&... gens) {
    return cons_generator<T, Gens...>(gens...);
  }

  template <typename T, typename... Args>
  cons_generator<T, generator<Args>...> cons() {
    return cons_generator<T, generator<Args>...>();
  }
}

#endif

