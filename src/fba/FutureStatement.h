#ifndef __FUTURE_STATEMENT__
#define __FUTURE_STATEMENT__

#include <memory>
#include <boost/asio.hpp>
#include "fba/Statement.h"

namespace stellar
{
    class FutureStatement : public std::enable_shared_from_this<FutureStatement>
    {
        boost::asio::deadline_timer mTimer;
        
    public:
        Statement::pointer mStatement;
        typedef std::shared_ptr<FutureStatement> pointer;

        FutureStatement(Statement::pointer statement, ApplicationPtr app);
        ~FutureStatement();

        void tryNow(ApplicationPtr app);

    };
}

#endif