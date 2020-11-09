// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

enum class GeneralizedLedgerEntryType
{
    LEDGER_ENTRY,
    SPONSORSHIP,
    SPONSORSHIP_COUNTER
};

struct SponsorshipKey
{
    AccountID sponsoredID;
};

struct SponsorshipCounterKey
{
    AccountID sponsoringID;
};

class GeneralizedLedgerKey
{
  private:
    GeneralizedLedgerEntryType mType;
    union {
        LedgerKey mLedgerKey;
        SponsorshipKey mSponsorshipKey;
        SponsorshipCounterKey mSponsorshipCounterKey;
    };

    void assign(GeneralizedLedgerKey const& glk);
    void assign(GeneralizedLedgerKey&& glk);
    void construct();
    void destruct();

    void checkDiscriminant(GeneralizedLedgerEntryType expected) const;

  public:
    GeneralizedLedgerKey();
    explicit GeneralizedLedgerKey(GeneralizedLedgerEntryType t);

    GeneralizedLedgerKey(LedgerKey const& lk);
    explicit GeneralizedLedgerKey(SponsorshipKey const& sk);
    explicit GeneralizedLedgerKey(SponsorshipCounterKey const& sck);

    GeneralizedLedgerKey(GeneralizedLedgerKey const& glk);
    GeneralizedLedgerKey(GeneralizedLedgerKey&& glk);

    GeneralizedLedgerKey& operator=(GeneralizedLedgerKey const& glk);
    GeneralizedLedgerKey& operator=(GeneralizedLedgerKey&& glk);

    ~GeneralizedLedgerKey();

    void type(GeneralizedLedgerEntryType t);
    GeneralizedLedgerEntryType type() const;

    LedgerKey& ledgerKey();
    LedgerKey const& ledgerKey() const;

    SponsorshipKey& sponsorshipKey();
    SponsorshipKey const& sponsorshipKey() const;

    SponsorshipCounterKey& sponsorshipCounterKey();
    SponsorshipCounterKey const& sponsorshipCounterKey() const;

    std::string toString() const;
};

struct SponsorshipEntry
{
    AccountID sponsoredID;
    AccountID sponsoringID;
};

struct SponsorshipCounterEntry
{
    AccountID sponsoringID;
    int64_t numSponsoring;
};

class GeneralizedLedgerEntry
{
  private:
    GeneralizedLedgerEntryType mType;
    union {
        LedgerEntry mLedgerEntry;
        SponsorshipEntry mSponsorshipEntry;
        SponsorshipCounterEntry mSponsorshipCounterEntry;
    };

    void assign(GeneralizedLedgerEntry const& gle);
    void assign(GeneralizedLedgerEntry&& gle);
    void construct();
    void destruct();

    void checkDiscriminant(GeneralizedLedgerEntryType expected) const;

  public:
    GeneralizedLedgerEntry();
    explicit GeneralizedLedgerEntry(GeneralizedLedgerEntryType t);

    GeneralizedLedgerEntry(LedgerEntry const& le);
    explicit GeneralizedLedgerEntry(SponsorshipEntry const& se);
    explicit GeneralizedLedgerEntry(SponsorshipCounterEntry const& sce);

    GeneralizedLedgerEntry(GeneralizedLedgerEntry const& gle);
    GeneralizedLedgerEntry(GeneralizedLedgerEntry&& gle);

    GeneralizedLedgerEntry& operator=(GeneralizedLedgerEntry const& gle);
    GeneralizedLedgerEntry& operator=(GeneralizedLedgerEntry&& gle);

    ~GeneralizedLedgerEntry();

    void type(GeneralizedLedgerEntryType t);
    GeneralizedLedgerEntryType type() const;

    LedgerEntry& ledgerEntry();
    LedgerEntry const& ledgerEntry() const;

    SponsorshipEntry& sponsorshipEntry();
    SponsorshipEntry const& sponsorshipEntry() const;

    SponsorshipCounterEntry& sponsorshipCounterEntry();
    SponsorshipCounterEntry const& sponsorshipCounterEntry() const;

    GeneralizedLedgerKey toKey() const;

    std::string toString() const;
};

bool operator==(SponsorshipKey const& lhs, SponsorshipKey const& rhs);
bool operator!=(SponsorshipKey const& lhs, SponsorshipKey const& rhs);
bool operator==(SponsorshipEntry const& lhs, SponsorshipEntry const& rhs);
bool operator!=(SponsorshipEntry const& lhs, SponsorshipEntry const& rhs);

bool operator==(SponsorshipCounterKey const& lhs,
                SponsorshipCounterKey const& rhs);
bool operator!=(SponsorshipCounterKey const& lhs,
                SponsorshipCounterKey const& rhs);
bool operator==(SponsorshipCounterEntry const& lhs,
                SponsorshipCounterEntry const& rhs);
bool operator!=(SponsorshipCounterEntry const& lhs,
                SponsorshipCounterEntry const& rhs);

bool operator==(GeneralizedLedgerKey const& lhs,
                GeneralizedLedgerKey const& rhs);
bool operator!=(GeneralizedLedgerKey const& lhs,
                GeneralizedLedgerKey const& rhs);
bool operator==(GeneralizedLedgerEntry const& lhs,
                GeneralizedLedgerEntry const& rhs);
bool operator!=(GeneralizedLedgerEntry const& lhs,
                GeneralizedLedgerEntry const& rhs);
}
