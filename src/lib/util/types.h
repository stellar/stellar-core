#include <vector>
#include "generated/stellar.hh"
#include "xdrpp/message.h"

namespace stellar
{
    typedef std::vector <unsigned char> Blob;

    bool isZero(stellarxdr::uint256 const& b);

    void hashXDR(xdr::msg_ptr msg, stellarxdr::uint256& retHash);
    void hashStr(std::string str, stellarxdr::uint256& retHash);

    std::string& toBase58(stellarxdr::uint256 const& b, std::string& retstr);
    stellarxdr::uint256 fromBase58(const std::string& str);

    stellarxdr::uint256 makePublicKey(stellarxdr::uint256 const& b);


}
