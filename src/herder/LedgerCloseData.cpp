#include "util/asio.h"
#include "LedgerCloseData.h"
#include "crypto/Hex.h"
#include "main/Application.h"
#include "util/Logging.h"
#include <overlay/OverlayManager.h>
#include <xdrpp/marshal.h>

using namespace std;

namespace stellar
{

LedgerCloseData::LedgerCloseData(uint32_t ledgerSeq, TxSetFramePtr txSet,
                                 StellarValue const& v)
    : mLedgerSeq(ledgerSeq), mTxSet(txSet), mValue(v)
{
    Value x;
    Value y(x.begin(), x.end());

    using xdr::operator==;
    assert(txSet->getContentsHash() == mValue.txSetHash);
}

std::string
stellarValueToString(StellarValue const& sv)
{
    std::stringstream res;

    res << "[ "
        << " txH: " << hexAbbrev(sv.txSetHash) << ", ct: " << sv.closeTime
        << ", upgrades: [";
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
                case LEDGER_UPGRADE_VERSION:
                    res << "VERSION=" << lupgrade.newLedgerVersion();
                    break;
                case LEDGER_UPGRADE_BASE_FEE:
                    res << "BASE_FEE=" << lupgrade.newBaseFee();
                    break;
                case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
                    res << "MAX_TX_SET_SIZE=" << lupgrade.newMaxTxSetSize();
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
