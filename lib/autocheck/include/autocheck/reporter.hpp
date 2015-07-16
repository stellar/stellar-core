#ifndef AUTOCHECK_REPORTER_HPP
#define AUTOCHECK_REPORTER_HPP

#include <iostream>
#include <sstream>
#include <cmath>

#include "distribution.hpp"

namespace autocheck {

  class reporter {
    public:
      virtual void success(size_t tests, size_t max_tests,
          size_t trivial = 0,
          distribution&& dist = distribution()) const = 0;
      virtual void failure(size_t tests, const char* reason) const = 0;
      virtual ~reporter() {}
  };

  inline int round_percentage(size_t a, size_t b) {
    return static_cast<int>(ceil(100.0 * a / b));
  }

  inline void report_success(std::ostream& out, size_t tests, size_t max_tests,
      size_t trivial, distribution&& dist)
  {
    if (tests < max_tests) {
      out << "Arguments exhausted after " << tests << " tests."
        << std::endl;
    } else {
      assert(tests == max_tests);
      out << "OK, passed " << tests << " tests";
      if (trivial) {
        out << " (" << round_percentage(trivial, tests) << "% trivial)";
      }
      out << "." << std::endl;
    }

    /* Sort tags in descending order by size. */
    std::sort(dist.begin(), dist.end(),
        [] (const dist_tag& a, const dist_tag& b) {
          return (std::get<1>(a) == std::get<1>(b))
          ? (std::get<0>(a) < std::get<0>(b))
          : (std::get<1>(a) > std::get<1>(b));
        });
    for (const dist_tag& tag : dist) {
      out << round_percentage(std::get<1>(tag), tests) << "% "
        << std::get<0>(tag) << "." << std::endl;
    }
  }

  inline void report_failure(std::ostream& out, size_t tests, const char* reason) {
    out << "Falsifiable, after " << tests << " tests:" << std::endl
      << reason << std::endl;
  }

  class ostream_reporter : public reporter {
    private:
      std::ostream& out;

    public:
      ostream_reporter(std::ostream& out = std::cout) : out(out) {}

      virtual void success(size_t tests, size_t max_tests,
          size_t trivial, distribution&& dist) const
      {
        report_success(out, tests, max_tests, trivial, std::move(dist));
      }

      virtual void failure(size_t tests, const char* reason) const {
        report_failure(out, tests, reason);
      }
  };

#ifdef ASSERT_TRUE

  class gtest_reporter : public reporter {
    public:
      virtual void success(size_t tests, size_t max_tests,
          size_t trivial, distribution&& dist) const
      {
        report_success(std::clog, tests, max_tests, trivial, std::move(dist));
        ASSERT_TRUE(true);
      }

      virtual void failure(size_t tests, const char* reason) const {
        std::ostringstream out;
        report_failure(out, tests, reason);
        static const bool AUTOCHECK_SUCCESS = false;
        ASSERT_TRUE(AUTOCHECK_SUCCESS) << out.str();
      }
  };

#endif // ASSERT_TRUE

}

#endif

