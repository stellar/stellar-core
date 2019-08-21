#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <string>

namespace stellar
{
class Application;

class CommandHandler
{
  public:
    static std::unique_ptr<CommandHandler> create(Application& app);
    virtual void addRoutes() = 0;
    virtual void manualCmd(std::string const& cmd) = 0;
    virtual ~CommandHandler()
    {
    }
};
}
