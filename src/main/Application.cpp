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
