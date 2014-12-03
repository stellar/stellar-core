#include "lib/util/types.h"

namespace stellar
{
    bool isZero(stellarxdr::uint256 const& b)
    {
        for(int i = 0; i < 32; i++)
            if(b[i] != 0)
                return false;

        return true;
    }

    void hashXDR(xdr::msg_ptr msg, stellarxdr::uint256& retHash)
    {
        // SANITY
    }

    std::string& toStr(stellarxdr::uint256 const& b, std::string& retstr)
    {
        // SANITY base58 encode
        return(retstr);
    }
}
