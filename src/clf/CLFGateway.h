#ifndef __CLF_GATEWAY__
#define __CLF_GATEWAY__
#include "clf/CLF.h"

namespace stellar
{
    class CLFGateway
    {
    public:
        virtual LedgerHeaderPtr getCurrentHeader()=0;
    };
}

#endif