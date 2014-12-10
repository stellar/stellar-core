#ifndef AUTOCHECK_DISTRIBUTION_HPP
#define AUTOCHECK_DISTRIBUTION_HPP

#include <vector>
#include <tuple>
#include <string>

namespace autocheck {

  typedef std::tuple<std::string, size_t>       dist_tag;
  typedef std::vector<dist_tag>                 distribution;

}

#endif

