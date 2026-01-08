#ifndef AUTOCHECK_DISTRIBUTION_HPP
#define AUTOCHECK_DISTRIBUTION_HPP

#include <string>
#include <tuple>
#include <vector>

namespace autocheck {

  typedef std::tuple<std::string, size_t>       dist_tag;
  typedef std::vector<dist_tag>                 distribution;

}

#endif

