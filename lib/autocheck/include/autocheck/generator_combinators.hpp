#ifndef AUTOCHECK_GENERATOR_COMBINATORS_HPP
#define AUTOCHECK_GENERATOR_COMBINATORS_HPP

#include <type_traits>

namespace autocheck {

  namespace detail {

    /* Wrappers used by combinators. */

    template <typename Func, typename Gen>
    class mapped_generator {
      public:
        typedef
          typename std::result_of<
            Func(typename Gen::result_type&&, size_t)
          >::type
          result_type;

      private:
        Func func;
        Gen  gen;

      public:
        mapped_generator(const Func& func, const Gen& gen) :
          func(func), gen(gen) {}

        result_type operator() (size_t size = 0) {
          return func(gen(size), size);
        }
    };

    template <typename Pred, typename Gen>
    class filtered_generator {
      public:
        typedef typename Gen::result_type result_type;

      private:
        Pred pred;
        Gen  gen;

      public:
        filtered_generator(const Pred& pred, const Gen& gen) :
          pred(pred), gen(gen) {}

        result_type operator() (size_t size = 0) {
          while (true) {
            result_type rv(gen(size));
            if (pred(rv)) return rv;
          }
        }
    };

    template <typename Resize, typename Gen>
    class resized_generator {
      public:
        typedef typename Gen::result_type result_type;

      private:
        Resize resizer;
        Gen    gen;

      public:
        resized_generator(const Resize& resizer, const Gen& gen) :
          resizer(resizer), gen(gen) {}

        result_type operator() (size_t size = 0) {
          return gen(resizer(size));
        }
    };

    template <typename Gen>
    class fixed_size_generator {
      private:
        size_t size;
        Gen    gen;

      public:
        typedef typename Gen::result_type result_type;

        fixed_size_generator(size_t size, const Gen& gen) :
          size(size), gen(gen) {}

        result_type operator() (size_t = 0) {
          return gen(size);
        }
    };

  }

  /* Generator combinators. */

  template <typename Func, typename Gen>
  detail::mapped_generator<Func, Gen> map(const Func& func, const Gen& gen) {
    return detail::mapped_generator<Func, Gen>(func, gen);
  }

  template <typename Pred, typename Gen>
  detail::filtered_generator<Pred, Gen> such_that(const Pred& pred,
      const Gen& gen)
  {
    return detail::filtered_generator<Pred, Gen>(pred, gen);
  }

  template <typename Resize, typename Gen>
  detail::resized_generator<Resize, Gen> resize(const Resize& resizer,
      const Gen& gen)
  {
    return detail::resized_generator<Resize, Gen>(resizer, gen);
  }

  template <typename Gen>
  detail::fixed_size_generator<Gen> fix(size_t size, const Gen& gen) {
    return detail::fixed_size_generator<Gen>(size, gen);
  }

}

#endif

