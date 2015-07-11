#ifndef AUTOCHECK_ARBITRARY_HPP
#define AUTOCHECK_ARBITRARY_HPP

#include <cassert>
#include <cstdio>

#include "function.hpp"
#include "generator.hpp"
#include "value.hpp"
#include "apply.hpp"

namespace autocheck {

  /* Arbitrary produces a finite sequence, always in a tuple, ready for
   * application, and counts discards. */

  template <typename... Gens>
  class arbitrary {
    static_assert(sizeof...(Gens) > 0, "cannot create unit arbitrary");

    /* MSVC has difficulty with a direct `Gens...` parameter pack expansion
     * in the 3 public typedefs below, but can be convinced to work with a
     * trivial helper struct. */
    template <typename T> struct ExpandArgs : T {};

    public:
      typedef std::tuple<typename ExpandArgs<Gens>::result_type...> result_type;

      typedef typename predicate<typename ExpandArgs<Gens>::result_type...>::type
        discard_t;
      typedef std::function<void (typename ExpandArgs<Gens>::result_type&...)>
        prep_t;

    private:
      std::tuple<Gens...> gens;
      size_t              count = 0;
      size_t              num_discards = 0;
      size_t              max_discards = 500;
      discard_t           discard_f;
      resize_t            resize_f = id();
      prep_t              prep_f;

    public:
      arbitrary() {}
      arbitrary(const Gens&... gens) : gens(gens...) {}

      bool operator() (value<result_type>& candidate) {
        while (num_discards < max_discards) {
          /* Size starts at 0 and grows moderately. */
          candidate = generate<result_type>(gens, resize_f((num_discards + count) >> 1));
          if (prep_f) {
            apply(prep_f, candidate.ref());
          }
          if (discard_f && apply(discard_f, candidate.cref())) {
            ++num_discards;
          } else {
            ++count;
            return true;
          }
        }
        return false;
      }

      arbitrary& reset() {
        num_discards = 0;
        count        = 0;
      }

      arbitrary& resize(const resize_t& r) {
        resize_f = r;
        return *this;
      }

      arbitrary& prep(const prep_t& p) {
        prep_f = p;
        return *this;
      }

      arbitrary& discard_if(const discard_t& d) {
        discard_f = d;
        return *this;
      }

      arbitrary& discard_at_most(size_t discards) {
        assert(discards > 0);
        max_discards = discards;
        return *this;
      }
  };

  /* Combinators. */

  template <typename Arbitrary>
  Arbitrary&& resize(const resize_t& r, Arbitrary&& arb) {
    arb.resize(r);
    return std::forward<Arbitrary>(arb);
  }

  template <typename Arbitrary>
  Arbitrary&& prep(const typename Arbitrary::prep_t& p, Arbitrary&& arb) {
    arb.prep(p);
    return std::forward<Arbitrary>(arb);
  }

  template <typename Arbitrary>
  Arbitrary&& discard_if(const typename Arbitrary::discard_t& d,
      Arbitrary&& arb)
  {
    arb.discard_if(d);
    return std::forward<Arbitrary>(arb);
  }

  template <typename Arbitrary>
  Arbitrary&& discard_at_most(size_t discards, Arbitrary&& arb) {
    arb.discard_at_most(discards);
    return std::forward<Arbitrary>(arb);
  }

  /* Factories. */

  template <typename... Gens>
  arbitrary<Gens...> make_arbitrary(const Gens&... gens) {
    return arbitrary<Gens...>(gens...);
  }

  template <typename... Args>
  arbitrary<generator<Args>...> make_arbitrary() {
    return arbitrary<generator<Args>...>(generator<Args>()...);
  }

}

#endif

