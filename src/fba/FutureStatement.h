#ifndef __FUTURESTATEMENT__
#define __FUTURESTATEMENT__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>
#include "fba/Statement.h"
#include "util/Timer.h"

namespace stellar
{
class FutureStatement : public std::enable_shared_from_this<FutureStatement>
{
    VirtualTimer mTimer;

  public:
    Statement::pointer mStatement;
    typedef std::shared_ptr<FutureStatement> pointer;

    FutureStatement(Statement::pointer statement, Application& app);
    ~FutureStatement();

    void tryNow(Application& app);
};
}

#endif
