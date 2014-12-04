#ifndef __FUTURE_STATEMENT__
#define __FUTURE_STATEMENT__

#include <memory>
#include "fba/Statement.h"
#include "util/timer.h"

namespace stellar
{
    class FutureStatement : public std::enable_shared_from_this<FutureStatement>
    {
        Timer mTimer;
        
    public:
        Statement::pointer mStatement;
        typedef std::shared_ptr<FutureStatement> pointer;

        FutureStatement(Statement::pointer statement, Application &app);
        ~FutureStatement();

        void tryNow(Application &app);

    };
}

#endif
