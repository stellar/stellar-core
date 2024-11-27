#include "util/asio.h"
#include "LedgerCloseData.h"
#include "crypto/Hex.h"
#include "herder/Upgrades.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include <overlay/OverlayManager.h>
#include <xdrpp/marshal.h>

using namespace std;

namespace stellar
{

LedgerCloseData::LedgerCloseData(uint32_t ledgerSeq,
                                 TxSetXDRFrameConstPtr txSet,
                                 StellarValue const& v,
                                 std::optional<Hash> const& expectedLedgerHash)
    : mLedgerSeq(ledgerSeq)
    , mTxSet(txSet)
    , mValue(v)
    , mExpectedLedgerHash(expectedLedgerHash)
{
    releaseAssert(txSet->getContentsHash() == mValue.txSetHash);
}

#ifdef BUILD_TESTS
LedgerCloseData::LedgerCloseData(
    uint32_t ledgerSeq, TxSetXDRFrameConstPtr txSet, StellarValue const& v,
    std::optional<Hash> const& expectedLedgerHash,
    std::optional<TransactionResultSet> const& expectedResults)
    : mLedgerSeq(ledgerSeq)
    , mTxSet(txSet)
    , mValue(v)
    , mExpectedLedgerHash(expectedLedgerHash)
    , mExpectedResults(expectedResults)
{
    releaseAssert(txSet->getContentsHash() == mValue.txSetHash);
}
#endif // BUILD_TESTS

std::string
stellarValueToString(Config const& c, StellarValue const& sv)
{
    std::stringstream res;

    res << "[";
    if (sv.ext.v() == STELLAR_VALUE_SIGNED)
    {
        res << " SIGNED@" << c.toShortString(sv.ext.lcValueSignature().nodeID);
    }
    res << " txH: " << hexAbbrev(sv.txSetHash) << ", ct: " << sv.closeTime
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
                res << Upgrades::toString(lupgrade);
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
