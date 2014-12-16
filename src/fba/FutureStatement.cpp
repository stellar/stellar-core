#include "fba/FutureStatement.h"
#include "main/Application.h"
#include "fba/FBA.h"
/*
A ballot that has its close time too far in the future.
You are unwilling to consider it until enough time passes.
*/

namespace stellar
{
FutureStatement::FutureStatement(Statement::pointer statement, Application& app)
    : mTimer(app.getMainIOService())
{
    uint64_t timeNow = time(nullptr);
    uint64_t numSeconds =
        statement->getBallot().closeTime - timeNow - MAX_SECONDS_LEDGER_CLOSE_IN_FUTURE;
    if (numSeconds <= 0)
        numSeconds = 1;
    mTimer.expires_from_now(std::chrono::seconds(numSeconds));
    mTimer.async_wait([this, &app](asio::error_code const& ec)
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
