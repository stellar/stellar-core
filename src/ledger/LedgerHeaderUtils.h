// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "xdr/Stellar-ledger.h"
#include <chrono>
#include <compare>
#include <string>
#include <type_traits>

namespace stellar
{
class Database;
class SessionWrapper;

// A whole-second distance between two ApplyTimes, e.g. how far
// ahead of the last closed ledger the ledger a tx set is built for applies.
class ApplyTimeOffset
{
  public:
    constexpr ApplyTimeOffset() = default;

    static constexpr ApplyTimeOffset
    fromSeconds(Duration seconds)
    {
        return ApplyTimeOffset(seconds);
    }

    constexpr Duration
    seconds() const
    {
        return mSeconds;
    }

    friend auto operator<=>(ApplyTimeOffset const&,
                            ApplyTimeOffset const&) = default;

  private:
    explicit constexpr ApplyTimeOffset(Duration seconds) : mSeconds(seconds)
    {
    }

    Duration mSeconds{0};
};

// Whole-second close time used by ledger application and protocol features
// such as time bounds, sequence age, claim predicates, Soroban timestamps, and
// upgrade scheduling.
class ApplyTime
{
  public:
    constexpr ApplyTime() = default;

    static constexpr ApplyTime
    fromTimePoint(TimePoint timePoint)
    {
        return ApplyTime(timePoint);
    }

    constexpr TimePoint
    timePoint() const
    {
        return mTimePoint;
    }

    // `later` must not precede `earlier`
    friend ApplyTimeOffset operator-(ApplyTime const& later,
                                     ApplyTime const& earlier);

    friend auto operator<=>(ApplyTime const&, ApplyTime const&) = default;

  private:
    explicit constexpr ApplyTime(TimePoint timePoint) : mTimePoint(timePoint)
    {
    }

    TimePoint mTimePoint{0};
};

// Close time as seen by consensus (SCP), in milliseconds since the Unix epoch.
//
// The value is ALWAYS milliseconds, whatever the protocol. Before
// MS_CLOSE_TIME_PROTOCOL_VERSION consensus only resolves whole seconds, so
// every ConsensusTime produced under such a protocol is a multiple of 1000
// (isWholeSecond()); from that protocol on, values may carry a sub-second
// component. Holding both in the same unit keeps comparisons correct across
// the upgrade boundary, where a whole-second LCL meets the first sub-second
// value.
//
// Sub-second values can only be created in MS_CLOSE_TIME builds: without the
// flag the ms constructor and accessor do not exist, and fromSystemTime() and
// next() always round to whole seconds, so a non-ms build can neither produce
// nor observe a sub-second close time.
class ConsensusTime
{
  public:
    ConsensusTime() = default;

    static ConsensusTime fromApplyTime(ApplyTime applyTime);
    // The system time rounded down to the whole second, unless
    // protocolVersion has ms close times (MS_CLOSE_TIME builds only).
    static ConsensusTime
    fromSystemTime(std::chrono::system_clock::time_point time,
                   uint32_t protocolVersion);
#ifdef MS_CLOSE_TIME
    static ConsensusTime fromMilliseconds(TimePointMilliseconds milliseconds);
    TimePointMilliseconds milliseconds() const;
#endif // MS_CLOSE_TIME

    bool isWholeSecond() const;
    // Rounded down to the whole second: the value StellarValue::closeTime
    // carries in both value formats.
    ApplyTime toApplyTime() const;
    std::chrono::system_clock::time_point toSystemTime() const;
    // The smallest close time strictly after this one: 1ms later once
    // protocolVersion has ms close times (MS_CLOSE_TIME builds only), the next
    // whole second before that.
    ConsensusTime next(uint32_t protocolVersion) const;
    std::string toString() const;

    friend auto operator<=>(ConsensusTime const&,
                            ConsensusTime const&) = default;

  private:
    explicit ConsensusTime(uint64_t milliseconds);

    uint64_t mMilliseconds{0};
};

static_assert(!std::is_convertible_v<TimePoint, ApplyTime>);
static_assert(!std::is_convertible_v<ApplyTime, TimePoint>);
static_assert(!std::is_convertible_v<uint64_t, ConsensusTime>);
static_assert(!std::is_convertible_v<ConsensusTime, uint64_t>);
static_assert(!std::is_convertible_v<ConsensusTime, ApplyTime>);
static_assert(!std::is_convertible_v<ApplyTime, ConsensusTime>);

bool protocolHasMsCloseTime(uint32_t protocolVersion);
bool isMsCloseTimeStellarValue(StellarValue const& sv);
bool hasValidCloseTime(StellarValue const& sv);

// Only meaningful for a value that passes hasValidCloseTime().
ConsensusTime getConsensusTime(StellarValue const& sv);

// Deliberately reads StellarValue::closeTime, whose whole-second application
// semantics are unchanged by CAP-0088.
ApplyTime getApplyTime(StellarValue const& sv);

bool isSignedStellarValue(StellarValue const& sv);
bool isEmptyTxSetStellarValue(StellarValue const& sv);

// Only valid for signed or empty-tx-set values.
LedgerCloseValueSignature& getLcValueSignature(StellarValue& sv);
LedgerCloseValueSignature const& getLcValueSignature(StellarValue const& sv);

// Only valid for empty-tx-set values.
Hash const& getProposedTxSetHash(StellarValue const& sv);
Hash const& getProposedPreviousLedgerHash(StellarValue const& sv);
uint32_t getProposedPreviousLedgerVersion(StellarValue const& sv);

namespace LedgerHeaderUtils
{

uint32_t getFlags(LedgerHeader const& lh);

// Return base64-encoded header data. Throws if the header fails basic sanity
// checks (e.g., fee pool >= 0).
std::string encodeHeader(LedgerHeader const& header);

#ifdef BUILD_TESTS
// Like the non-test encodeHeader, except also include the hex-encoded hash of
// the header in the `hash` out parameter
std::string encodeHeader(LedgerHeader const& header, std::string& hash);
void storeInDatabase(Database& db, LedgerHeader const& header,
                     SessionWrapper& sess);
#endif

LedgerHeader decodeFromData(std::string const& data);

// Returns the base64-encoded header data for the given hash. Returns an empty
// string if no header is found for the hash.
std::string getHeaderDataForHash(Database& db, Hash const& hash);

void maybeDropAndCreateNew(Database& db);
}
}
