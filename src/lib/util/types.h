#include <vector>
#include "generated/stellar.hh"
#include "xdrpp/message.h"

namespace stellar
{
    typedef std::vector <unsigned char> Blob;

    bool isZero(stellarxdr::uint256& b);

    void hashXDR(xdr::msg_ptr msg, stellarxdr::uint256& retHash);

    std::string& toStr(stellarxdr::uint256& b, std::string& retstr);

}
