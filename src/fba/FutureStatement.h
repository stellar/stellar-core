#ifndef __FUTURE_STATEMENT__
#define __FUTURE_STATEMENT__

#include <memory>
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>
#include "fba/Statement.h"
#include <chrono>

namespace stellar
{
    class FutureStatement : public std::enable_shared_from_this<FutureStatement>
    {
        asio::basic_waitable_timer<std::chrono::steady_clock> mTimer;
        
    public:
        Statement::pointer mStatement;
        typedef std::shared_ptr<FutureStatement> pointer;

        FutureStatement(Statement::pointer statement, ApplicationPtr app);
        ~FutureStatement();

        void tryNow(ApplicationPtr app);

    };
}

#endif
