#ifndef __COMMANDHANDLER__
#define __COMMANDHANDLER__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <string>
#include "lib/http/server.hpp"

/*
handler functions for the http commands this server supports
*/

namespace stellar
{
class Application;

class CommandHandler
{

    Application& mApp;
    std::unique_ptr<http::server::server> mServer;

  public:
    CommandHandler(Application& app);

    void stop(const std::string& params, std::string& retStr);
    void peers(const std::string& params, std::string& retStr);
    void info(const std::string& params, std::string& retStr);
    void metrics(const std::string& params, std::string& retStr);
    void reloadCfg(const std::string& params, std::string& retStr);
    void logRotate(const std::string& params, std::string& retStr);
    void connect(const std::string& params, std::string& retStr);
    void tx(const std::string& params, std::string& retStr);
};
}

#endif
