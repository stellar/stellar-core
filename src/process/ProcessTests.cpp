#include "main/Application.h"
#include "xdrpp/autocheck.h"
#include "history/HistoryGateway.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include <future>

using namespace stellar;

TEST_CASE("subprocess", "[process]")
{
    Config const& cfg = getTestConfig();
    Application app(cfg);
    auto evt = app.getProcessGateway().runProcess("hostname");
    bool exited = false;
    evt.async_wait([&](asio::error_code ec)
                   {
                       LOG(DEBUG) << "process exited: " << ec;
                       if (ec)
                           LOG(DEBUG) << "error code: " << ec.message();
                       exited = true;
                   });

    while (!exited && !app.getMainIOService().stopped())
    {
        app.getMainIOService().poll_one();
    }
}
