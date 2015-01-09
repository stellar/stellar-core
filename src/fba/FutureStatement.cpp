// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "fba/FutureStatement.h"
#include "fba/FBAGateway.h"
#include "main/Application.h"
#include "fba/FBA.h"
#include <memory>

/*
A ballot that has its close time too far in the future.
You are unwilling to consider it until enough time passes.
*/

namespace stellar
{
FutureStatement::FutureStatement(Statement::pointer statement, Application& app)
    : mTimer(app.getClock())
{
    uint64_t timeNow = time(nullptr);
    uint64_t numSeconds = statement->getBallot().closeTime - timeNow -
                          MAX_SECONDS_LEDGER_CLOSE_IN_FUTURE;
    if (numSeconds <= 0)
        numSeconds = 1;
    mTimer.expires_from_now(std::chrono::seconds(numSeconds));
    mTimer.async_wait([this, &app](asio::error_code ec)
                      {
                          this->tryNow(app);
                      });
}

FutureStatement::~FutureStatement()
{
    mTimer.cancel();
}

void
FutureStatement::tryNow(Application& app)
{
    app.getFBAGateway().statementReady(shared_from_this());
}
}
