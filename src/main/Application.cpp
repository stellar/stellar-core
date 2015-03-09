// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Application.h"
#include "ApplicationImpl.h"

namespace stellar
{
using namespace std;

Application::pointer Application::create(VirtualClock& clock, Config const&cfg)
{
  return make_shared<ApplicationImpl>(clock, cfg);
}



}
