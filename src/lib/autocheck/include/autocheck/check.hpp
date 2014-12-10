#ifndef AUTOCHECK_CHECK_HPP
#define AUTOCHECK_CHECK_HPP

#include <sstream>

#include "function.hpp"
#include "arbitrary.hpp"
#include "tuple.hpp"
#include "reporter.hpp"
#include "classifier.hpp"

namespace autocheck {

  template <typename T>
  T copy(const T& t) { return t; }

  /* Have to split defaults between two functions because of Clang ICE. */

  template <
    typename... Args,
    typename Property,
    typename Arbitrary
    //, typename Classifier = classifier<Args...> // ICEs Clang
  >
  void check(Property prop,
      size_t max_tests,
      Arbitrary arb,
      const reporter& rep = ostream_reporter(),
      classifier<Args...> cls = classifier<Args...>(),
      //Classifier cls = Classifier(),
      bool verbose = false)
  {
    assert(max_tests > 0);

    size_t tests = 0;

    typedef std::tuple<Args...> args_t;
    value<args_t> args;
    while (arb(args) && (++tests != max_tests)) {
      /* Get what we need from `args` before we let the user modify them. */
      std::ostringstream reason;
      reason << std::boolalpha << args;
      if (verbose) {
        std::cout << reason.str() << std::endl;
      }

      cls.check(args.cref());
      if (!apply(prop, args.ref())) {
        rep.failure(tests, reason.str().c_str());
        return;
      }
    }

    rep.success(tests, max_tests, cls.trivial(), cls.distro());
  }

  template <typename... Args, typename Property>
  void check(Property prop, size_t max_tests = 100) {
    check<Args...>(std::move(prop),
        max_tests,
        arbitrary<generator<Args>...>(),
        ostream_reporter(),
        classifier<Args...>(),
        false);
  }

}

#endif

