// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "util/Timer.h"
#include "xdr/Stellar-ledger.h"
#include <compare>

namespace stellar
{
class Database;
class SessionWrapper;

// Wrapper around StellarValue closeTime, which has a whole second field and a
// separate ms field, conditional on protocol version.
struct CloseTime
{
    TimePoint closeTime{0};
    uint32_t closeTimeMs{0};

    CloseTime() = default;
    // A whole-second close time is equivalent to a 0 ms close time
    CloseTime(TimePoint ct) : closeTime(ct), closeTimeMs(0)
    {
    }
    CloseTime(TimePoint ct, uint32_t ms) : closeTime(ct), closeTimeMs(ms)
    {
    }

    static constexpr uint32_t MAX_CLOSE_TIME_MS = 999;

    friend auto operator<=>(CloseTime const&, CloseTime const&) = default;

    // Returns the smallest close time that may validly follow this one for the
    // next ledger. On whole second protocols this is +1s, otherwise +1ms.
    CloseTime next(uint32_t protocolVersion) const;

    VirtualClock::system_time_point toSystemTime() const;
    // Converts system time at the resolution supported by `protocolVersion`.
    // Protocols before ms close times are rounded down to a whole second.
    static CloseTime fromSystemTime(VirtualClock::system_time_point time,
                                    uint32_t protocolVersion);
};

// Returns 0 for value types that predate millisecond resolution.
uint32_t getCloseTimeMs(StellarValue const& sv);
CloseTime getCloseTime(StellarValue const& sv);

bool isSignedStellarValue(StellarValue const& sv);
bool isEmptyTxSetStellarValue(StellarValue const& sv);
bool isMsCloseTimeStellarValue(StellarValue const& sv);

// Validates the close time format of a StellarValue.
// If `allowMsTime` is true, ms close times are allowed.
// If `allowWholeSecondTime` is true, whole second close times are allowed.
bool validateMsCloseTimeFormat(StellarValue const& sv, bool allowMsTime,
                               bool allowWholeSecondTime);

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
