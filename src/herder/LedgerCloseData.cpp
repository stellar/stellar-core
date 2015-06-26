#include "LedgerCloseData.h"
#include "main/Application.h"
#include "crypto/Hex.h"
#include <overlay/OverlayManager.h>
#include <xdrpp/marshal.h>
#include "util/Logging.h"

using namespace std;

namespace stellar
{

LedgerCloseData::LedgerCloseData(uint32_t ledgerSeq, TxSetFramePtr txSet,
                                 StellarValue const& v)
    : mLedgerSeq(ledgerSeq), mTxSet(txSet), mValue(v)
{
    using xdr::operator==;
    assert(txSet->getContentsHash() == mValue.txSetHash);
}

std::string
stellarValueToString(StellarValue const& sv)
{
    std::stringstream res;

    res << "[ "
        << " h: " << hexAbbrev(sv.txSetHash) << ", ct: " << sv.closeTime
        << " upgrades: [";
    for (auto const& upgrade : sv.upgrades)
    {
        if (upgrade.empty())
        {
            // should not happen as this is not valid
            res << "<empty>";
        }
        else
        {
            try
            {
                LedgerUpgrade lupgrade;
                xdr::xdr_from_opaque(upgrade, lupgrade);
                switch (lupgrade.type())
                {
                case LEDGER_UPGRADE_BASE_FEE:
                    res << "BASE_FEE=" << lupgrade.newBaseFee();
                    break;
                case LEDGER_UPGRADE_VERSION:
                    res << "VERSION=" << lupgrade.newLedgerVersion();
                    break;
                default:
                    res << "<unsupported>";
                }
            }
            catch (std::exception&)
            {
                res << "<unknown>";
            }
        }
        res << ", ";
    }
    res << " ] ]";

    return res.str();
}
}
