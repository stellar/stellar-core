// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerHeaderUtils.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "database/DatabaseUtils.h"
#include "util/Decoder.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include "util/types.h"
#include "xdrpp/marshal.h"

#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

namespace
{
template <typename T>
auto&
getLcValueSignatureImpl(T& sv)
{
    switch (sv.ext.v())
    {
    case STELLAR_VALUE_SIGNED:
        return sv.ext.lcValueSignature();
    case STELLAR_VALUE_EMPTY_TX_SET:
        return sv.ext.proposedValue().lcValueSignature;
#ifdef MS_CLOSE_TIME
    case STELLAR_VALUE_SIGNED_MS:
        return sv.ext.signedMsValue().lcValueSignature;
    case STELLAR_VALUE_EMPTY_TX_SET_MS:
        return sv.ext.proposedMsValue().lcValueSignature;
#endif
    default:
        releaseAssert(false);
    }
}
}

CloseTime
CloseTime::next(uint32_t protocolVersion) const
{
    releaseAssert(closeTimeMs <= MAX_CLOSE_TIME_MS);
    if (protocolVersionStartsFrom(protocolVersion,
                                  MS_CLOSE_TIME_PROTOCOL_VERSION) &&
        closeTimeMs < MAX_CLOSE_TIME_MS)
    {
        return {closeTime, closeTimeMs + 1};
    }
    return {closeTime + 1, 0};
}

VirtualClock::system_time_point
CloseTime::toSystemTime() const
{
    releaseAssert(closeTimeMs <= MAX_CLOSE_TIME_MS);
    return VirtualClock::from_time_t(static_cast<std::time_t>(closeTime)) +
           std::chrono::milliseconds(closeTimeMs);
}

CloseTime
CloseTime::fromSystemTime(VirtualClock::system_time_point time,
                          uint32_t protocolVersion)
{
    auto const sinceEpoch =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            time.time_since_epoch());
    releaseAssert(sinceEpoch.count() >= 0);
    auto const seconds =
        std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch);
    auto const milliseconds =
        protocolVersionStartsFrom(protocolVersion,
                                  MS_CLOSE_TIME_PROTOCOL_VERSION)
            ? sinceEpoch - seconds
            : std::chrono::milliseconds::zero();
    return {static_cast<TimePoint>(seconds.count()),
            static_cast<uint32_t>(milliseconds.count())};
}

uint32_t
getCloseTimeMs(StellarValue const& sv)
{
    switch (sv.ext.v())
    {
#ifdef MS_CLOSE_TIME
    case STELLAR_VALUE_SIGNED_MS:
        return sv.ext.signedMsValue().closeTimeMs;
    case STELLAR_VALUE_EMPTY_TX_SET_MS:
        return sv.ext.proposedMsValue().closeTimeMs;
#endif
    default:
        return 0;
    }
}

CloseTime
getCloseTime(StellarValue const& sv)
{
    return {sv.closeTime, getCloseTimeMs(sv)};
}

bool
isSignedStellarValue(StellarValue const& sv)
{
    switch (sv.ext.v())
    {
    case STELLAR_VALUE_SIGNED:
#ifdef MS_CLOSE_TIME
    case STELLAR_VALUE_SIGNED_MS:
#endif
        return true;
    default:
        return false;
    }
}

bool
isEmptyTxSetStellarValue(StellarValue const& sv)
{
    switch (sv.ext.v())
    {
    case STELLAR_VALUE_EMPTY_TX_SET:
#ifdef MS_CLOSE_TIME
    case STELLAR_VALUE_EMPTY_TX_SET_MS:
#endif
        return true;
    default:
        return false;
    }
}

bool
isMsCloseTimeStellarValue(StellarValue const& sv)
{
    switch (sv.ext.v())
    {
#ifdef MS_CLOSE_TIME
    case STELLAR_VALUE_SIGNED_MS:
    case STELLAR_VALUE_EMPTY_TX_SET_MS:
        return true;
#endif
    default:
        return false;
    }
}

bool
validateMsCloseTimeFormat(StellarValue const& sv, bool allowMsTime,
                          bool allowWholeSecondTime)
{
    if (getCloseTimeMs(sv) > CloseTime::MAX_CLOSE_TIME_MS)
    {
        return false;
    }
    return isMsCloseTimeStellarValue(sv) ? allowMsTime : allowWholeSecondTime;
}

LedgerCloseValueSignature&
getLcValueSignature(StellarValue& sv)
{
    return getLcValueSignatureImpl(sv);
}

LedgerCloseValueSignature const&
getLcValueSignature(StellarValue const& sv)
{
    return getLcValueSignatureImpl(sv);
}

Hash const&
getProposedTxSetHash(StellarValue const& sv)
{
#ifdef MS_CLOSE_TIME
    if (sv.ext.v() == STELLAR_VALUE_EMPTY_TX_SET_MS)
    {
        return sv.ext.proposedMsValue().txSetHash;
    }
#endif
    releaseAssert(sv.ext.v() == STELLAR_VALUE_EMPTY_TX_SET);
    return sv.ext.proposedValue().txSetHash;
}

Hash const&
getProposedPreviousLedgerHash(StellarValue const& sv)
{
#ifdef MS_CLOSE_TIME
    if (sv.ext.v() == STELLAR_VALUE_EMPTY_TX_SET_MS)
    {
        return sv.ext.proposedMsValue().previousLedgerHash;
    }
#endif
    releaseAssert(sv.ext.v() == STELLAR_VALUE_EMPTY_TX_SET);
    return sv.ext.proposedValue().previousLedgerHash;
}

uint32_t
getProposedPreviousLedgerVersion(StellarValue const& sv)
{
#ifdef MS_CLOSE_TIME
    if (sv.ext.v() == STELLAR_VALUE_EMPTY_TX_SET_MS)
    {
        return sv.ext.proposedMsValue().previousLedgerVersion;
    }
#endif
    releaseAssert(sv.ext.v() == STELLAR_VALUE_EMPTY_TX_SET);
    return sv.ext.proposedValue().previousLedgerVersion;
}

static bool
isValid(LedgerHeader const& lh)
{
    bool res = (lh.ledgerSeq <= INT32_MAX);

    res = res && (lh.scpValue.closeTime <= INT64_MAX);
    res = res && (getCloseTimeMs(lh.scpValue) <= CloseTime::MAX_CLOSE_TIME_MS);
    res = res && (lh.feePool >= 0);
    res = res && (lh.idPool <= INT64_MAX);
    return res;
}

namespace LedgerHeaderUtils
{

uint32_t
getFlags(LedgerHeader const& lh)
{
    return lh.ext.v() == 1 ? lh.ext.v1().flags : 0;
}

static std::string
encodeHeader(LedgerHeader const& header, std::string* hash)
{
    if (!isValid(header))
    {
        throw std::runtime_error("invalid ledger header (insert)");
    }
    auto headerBytes(xdr::xdr_to_opaque(header));
    if (hash)
    {
        *hash = binToHex(sha256(headerBytes));
    }
    return decoder::encode_b64(headerBytes);
}

std::string
encodeHeader(LedgerHeader const& header)
{
    return encodeHeader(header, nullptr);
}

#ifdef BUILD_TESTS
std::string
encodeHeader(LedgerHeader const& header, std::string& hash)
{
    return encodeHeader(header, &hash);
}

void
storeInDatabase(Database& db, LedgerHeader const& header, SessionWrapper& sess)
{
    ZoneScoped;

    std::string hash, prevHash(binToHex(header.previousLedgerHash)),
        bucketListHash(binToHex(header.bucketListHash));
    std::string headerEncoded = encodeHeader(header, hash);

    // note: columns other than "data" are there to facilitate lookup/processing
    auto prep = db.getPreparedStatement(
        "INSERT INTO ledgerheaders "
        "(ledgerhash, prevhash, bucketlisthash, ledgerseq, closetime, data) "
        "VALUES "
        "(:h,        :ph,      :blh,            :seq,     :ct,       :data)",
        sess);
    auto& st = prep.statement();
    st.exchange(soci::use(hash));
    st.exchange(soci::use(prevHash));
    st.exchange(soci::use(bucketListHash));
    st.exchange(soci::use(header.ledgerSeq));
    st.exchange(soci::use(header.scpValue.closeTime));
    st.exchange(soci::use(headerEncoded));
    st.define_and_bind();
    {
        ZoneNamedN(insertLedgerHeadersZone, "insert ledgerheaders", true);
        st.execute(true);
    }
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}
#endif

LedgerHeader
decodeFromData(std::string const& data)
{
    ZoneScoped;
    LedgerHeader lh;
    std::vector<uint8_t> decoded;
    decoder::decode_b64(data, decoded);

    if (decoded.empty())
    {
        throw std::runtime_error("invalid base64 ledger header data");
    }

    xdr::xdr_get g(&decoded.front(), &decoded.back() + 1);
    xdr::xdr_argpack_archive(g, lh);
    g.done();

    if (!isValid(lh))
    {
        throw std::runtime_error("invalid ledger header (load)");
    }
    return lh;
}

std::string
getHeaderDataForHash(Database& db, Hash const& hash)
{
    ZoneScoped;
    std::string hash_s(binToHex(hash));
    std::string headerEncoded;

    auto prep = db.getPreparedStatement("SELECT data FROM ledgerheaders "
                                        "WHERE ledgerhash = :h",
                                        db.getSession());
    auto& st = prep.statement();
    st.exchange(soci::into(headerEncoded));
    st.exchange(soci::use(hash_s));
    st.define_and_bind();
    {
        ZoneNamedN(selectLedgerHeadersZone, "select ledgerheaders", true);
        st.execute(true);
    }
    if (st.got_data())
    {
        auto lh = decodeFromData(headerEncoded);
        auto ledgerHash = xdrSha256(lh);
        if (ledgerHash != hash)
        {
            throw std::runtime_error(
                fmt::format(FMT_STRING("Wrong hash in ledger header database: "
                                       "loaded ledger {} contains {}"),
                            binToHex(ledgerHash), binToHex(hash)));
        }
    }

    return headerEncoded;
}

void
maybeDropAndCreateNew(Database& db)
{
    std::string coll = db.getSimpleCollationClause();

    db.getRawSession() << "DROP TABLE IF EXISTS ledgerheaders;";
    db.getRawSession()
        << "CREATE TABLE ledgerheaders ("
        << "ledgerhash      CHARACTER(64) " << coll << " PRIMARY KEY,"
        << "prevhash        CHARACTER(64) NOT NULL,"
           "bucketlisthash  CHARACTER(64) NOT NULL,"
           "ledgerseq       INT UNIQUE CHECK (ledgerseq >= 0),"
           "closetime       BIGINT NOT NULL CHECK (closetime >= 0),"
           "data            TEXT NOT NULL"
           ");";
    db.getRawSession()
        << "CREATE INDEX ledgersbyseq ON ledgerheaders ( ledgerseq );";
}
}
}
