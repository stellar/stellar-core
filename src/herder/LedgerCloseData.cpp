#include "util/asio.h"
#include "LedgerCloseData.h"
#include "crypto/Hex.h"
#include "herder/Herder.h"
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
    Hash const& valueTxHash = mValue.txSetHash;
    releaseAssert(valueTxHash == Herder::SKIP_LEDGER_HASH ||
                  txSet->getContentsHash() == valueTxHash);
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
    Hash const& valueTxHash = mValue.txSetHash;
    releaseAssert(valueTxHash == Herder::SKIP_LEDGER_HASH ||
                  txSet->getContentsHash() == valueTxHash);
}
#endif // BUILD_TESTS

std::string
stellarValueToString(Config const& c, StellarValue const& sv)
{
    std::stringstream res;

    res << "[";
    switch (sv.ext.v())
    {
    case STELLAR_VALUE_BASIC:
        break;
    case STELLAR_VALUE_SIGNED:
        res << " SIGNED@" << c.toShortString(sv.ext.lcValueSignature().nodeID);
        break;
    case STELLAR_VALUE_SKIP:
        res << " SKIP@"
            << c.toShortString(sv.ext.originalValue().lcValueSignature.nodeID);
        break;
    default:
        res << " UNKNOWN";
        break;
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
