#include "ledger/TransactionResultSetFrame.h"
#include "crypto/SHA.h"
#include "util/ProtocolVersion.h"

namespace stellar
{

TransactionResultSetFrame::TransactionResultSetFrame(uint32_t protocolVersion)
{
    mVersion = 1;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    // See bug https://github.com/stellar/stellar-core/issues/3555 -- for the
    // time being, we are _not_ hashing or emitting TransactionResutlSetFrameV2
    // structures because we have not worked out how to transition horizon, the
    // history archives / catchup code, archivist, and other such tools.
    bool V2_CONSUMERS_READY = false;
    if (V2_CONSUMERS_READY &&
        protocolVersionStartsFrom(protocolVersion, SOROBAN_PROTOCOL_VERSION))
    {
        mVersion = 2;
    }
#endif
}

void
TransactionResultSetFrame::reserveResults(size_t n)
{
    switch (mVersion)
    {
    case 1:
        mTransactionResultSet.results.reserve(n);
        break;
    default:
        releaseAssert(false);
    }
}
void
TransactionResultSetFrame::pushMetaAndResultPair(
    TransactionMeta const& tm, TransactionResultPair const& rp)
{
    switch (mVersion)
    {
    case 1:
        mTransactionResultSet.results.emplace_back();
        mTransactionResultSet.results.back() = rp;
        break;
    default:
        releaseAssert(false);
    }
}
TransactionResultSet const&
TransactionResultSetFrame::getV1XDR()
{
    return mTransactionResultSet;
}
Hash
TransactionResultSetFrame::getXDRHash()
{
    switch (mVersion)
    {
    case 1:
        return xdrSha256(mTransactionResultSet);
    default:
        releaseAssert(false);
    }
}

}
