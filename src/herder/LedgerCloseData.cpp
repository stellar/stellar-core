#include "LedgerCloseData.h"
#include "main/Application.h"
#include "crypto/Hex.h"
#include <overlay/OverlayManager.h>
#include <xdrpp/marshal.h>
#include "util/Logging.h"

using namespace std;

namespace stellar
{
std::string
stellarValueToString(StellarValue const& sv)
{
    std::stringstream res;

    res << "[ "
        << " h: " << hexAbbrev(sv.txSetHash) << ", ct: " << sv.closeTime
        << ", fee: " << sv.baseFee << ", v: " << sv.ledgerVersion << " ]";

    return res.str();
}
}
