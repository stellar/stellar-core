// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/GeneralizedLedgerEntry.h"
#include "util/XDRCereal.h"
#include "util/types.h"

#include <fmt/format.h>

namespace stellar
{

bool
operator==(SponsorshipKey const& lhs, SponsorshipKey const& rhs)
{
    return lhs.sponsoredID == rhs.sponsoredID;
}

bool
operator!=(SponsorshipKey const& lhs, SponsorshipKey const& rhs)
{
    return !(lhs == rhs);
}

bool
operator==(SponsorshipEntry const& lhs, SponsorshipEntry const& rhs)
{
    return lhs.sponsoredID == rhs.sponsoredID &&
           lhs.sponsoringID == rhs.sponsoringID;
}

bool
operator!=(SponsorshipEntry const& lhs, SponsorshipEntry const& rhs)
{
    return !(lhs == rhs);
}

bool
operator==(SponsorshipCounterKey const& lhs, SponsorshipCounterKey const& rhs)
{
    return lhs.sponsoringID == rhs.sponsoringID;
}

bool
operator!=(SponsorshipCounterKey const& lhs, SponsorshipCounterKey const& rhs)
{
    return !(lhs == rhs);
}

bool
operator==(SponsorshipCounterEntry const& lhs,
           SponsorshipCounterEntry const& rhs)
{
    return lhs.sponsoringID == rhs.sponsoringID &&
           lhs.numSponsoring == rhs.numSponsoring;
}

bool
operator!=(SponsorshipCounterEntry const& lhs,
           SponsorshipCounterEntry const& rhs)
{
    return !(lhs == rhs);
}

// GeneralizedLedgerKey -------------------------------------------------------
GeneralizedLedgerKey::GeneralizedLedgerKey()
    : GeneralizedLedgerKey(GeneralizedLedgerEntryType::LEDGER_ENTRY)
{
}

GeneralizedLedgerKey::GeneralizedLedgerKey(GeneralizedLedgerEntryType t)
    : mType(t)
{
    construct();
}

GeneralizedLedgerKey::GeneralizedLedgerKey(LedgerKey const& lk)
    : GeneralizedLedgerKey(GeneralizedLedgerEntryType::LEDGER_ENTRY)
{
    ledgerKey() = lk;
}

GeneralizedLedgerKey::GeneralizedLedgerKey(SponsorshipKey const& sk)
    : GeneralizedLedgerKey(GeneralizedLedgerEntryType::SPONSORSHIP)
{
    sponsorshipKey() = sk;
}

GeneralizedLedgerKey::GeneralizedLedgerKey(SponsorshipCounterKey const& sck)
    : GeneralizedLedgerKey(GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER)
{
    sponsorshipCounterKey() = sck;
}

GeneralizedLedgerKey::GeneralizedLedgerKey(GeneralizedLedgerKey const& glk)
    : GeneralizedLedgerKey(glk.type())
{
    assign(glk);
}

GeneralizedLedgerKey::GeneralizedLedgerKey(GeneralizedLedgerKey&& glk)
    : GeneralizedLedgerKey(glk.type())
{
    assign(std::move(glk));
}

GeneralizedLedgerKey&
GeneralizedLedgerKey::operator=(GeneralizedLedgerKey const& glk)
{
    type(glk.type());
    assign(glk);
    return *this;
}

GeneralizedLedgerKey&
GeneralizedLedgerKey::operator=(GeneralizedLedgerKey&& glk)
{
    if (this == &glk)
    {
        return *this;
    }

    type(glk.type());
    assign(std::move(glk));
    return *this;
}

GeneralizedLedgerKey::~GeneralizedLedgerKey()
{
    destruct();
}

void
GeneralizedLedgerKey::assign(GeneralizedLedgerKey const& glk)
{
    assert(glk.type() == mType);
    switch (mType)
    {
    case GeneralizedLedgerEntryType::LEDGER_ENTRY:
        ledgerKey() = glk.ledgerKey();
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP:
        sponsorshipKey() = glk.sponsorshipKey();
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER:
        sponsorshipCounterKey() = glk.sponsorshipCounterKey();
        break;
    default:
        abort();
    }
}

void
GeneralizedLedgerKey::assign(GeneralizedLedgerKey&& glk)
{
    assert(glk.type() == mType);
    switch (mType)
    {
    case GeneralizedLedgerEntryType::LEDGER_ENTRY:
        ledgerKey() = std::move(glk.ledgerKey());
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP:
        sponsorshipKey() = std::move(glk.sponsorshipKey());
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER:
        sponsorshipCounterKey() = std::move(glk.sponsorshipCounterKey());
        break;
    default:
        abort();
    }
}

void
GeneralizedLedgerKey::construct()
{
    switch (mType)
    {
    case GeneralizedLedgerEntryType::LEDGER_ENTRY:
        new (&mLedgerKey) LedgerKey();
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP:
        new (&mSponsorshipKey) SponsorshipKey();
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER:
        new (&mSponsorshipCounterKey) SponsorshipCounterKey();
        break;
    default:
        abort();
    }
}

void
GeneralizedLedgerKey::destruct()
{
    switch (mType)
    {
    case GeneralizedLedgerEntryType::LEDGER_ENTRY:
        mLedgerKey.~LedgerKey();
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP:
        mSponsorshipKey.~SponsorshipKey();
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER:
        mSponsorshipCounterKey.~SponsorshipCounterKey();
        break;
    default:
        abort();
    }
}

void
GeneralizedLedgerKey::type(GeneralizedLedgerEntryType t)
{
    if (t != mType)
    {
        destruct();
        mType = t;
        construct();
    }
}

GeneralizedLedgerEntryType
GeneralizedLedgerKey::type() const
{
    return mType;
}

void
GeneralizedLedgerKey::checkDiscriminant(
    GeneralizedLedgerEntryType expected) const
{
    if (mType != expected)
    {
        throw std::runtime_error("invalid union access");
    }
}

LedgerKey&
GeneralizedLedgerKey::ledgerKey()
{
    checkDiscriminant(GeneralizedLedgerEntryType::LEDGER_ENTRY);
    return mLedgerKey;
}

LedgerKey const&
GeneralizedLedgerKey::ledgerKey() const
{
    checkDiscriminant(GeneralizedLedgerEntryType::LEDGER_ENTRY);
    return mLedgerKey;
}

SponsorshipKey&
GeneralizedLedgerKey::sponsorshipKey()
{
    checkDiscriminant(GeneralizedLedgerEntryType::SPONSORSHIP);
    return mSponsorshipKey;
}

SponsorshipKey const&
GeneralizedLedgerKey::sponsorshipKey() const
{
    checkDiscriminant(GeneralizedLedgerEntryType::SPONSORSHIP);
    return mSponsorshipKey;
}

SponsorshipCounterKey&
GeneralizedLedgerKey::sponsorshipCounterKey()
{
    checkDiscriminant(GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER);
    return mSponsorshipCounterKey;
}

SponsorshipCounterKey const&
GeneralizedLedgerKey::sponsorshipCounterKey() const
{
    checkDiscriminant(GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER);
    return mSponsorshipCounterKey;
}

std::string
GeneralizedLedgerKey::toString() const
{
    switch (mType)
    {
    case GeneralizedLedgerEntryType::LEDGER_ENTRY:
        return xdr_to_string(ledgerKey());
    case GeneralizedLedgerEntryType::SPONSORSHIP:
        return fmt::format("{{\n  sponsoredID = {}\n}}\n",
                           xdr_to_string(sponsorshipKey().sponsoredID));
    case GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER:
        return fmt::format("{{\n  sponsoringID = {}\n}}\n",
                           xdr_to_string(sponsorshipCounterKey().sponsoringID));
    default:
        abort();
    }
}

bool
operator==(GeneralizedLedgerKey const& lhs, GeneralizedLedgerKey const& rhs)
{
    if (lhs.type() != rhs.type())
    {
        return false;
    }

    switch (lhs.type())
    {
    case GeneralizedLedgerEntryType::LEDGER_ENTRY:
        return lhs.ledgerKey() == rhs.ledgerKey();
    case GeneralizedLedgerEntryType::SPONSORSHIP:
        return lhs.sponsorshipKey() == rhs.sponsorshipKey();
    case GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER:
        return lhs.sponsorshipCounterKey() == rhs.sponsorshipCounterKey();
    default:
        abort();
    }
}

bool
operator!=(GeneralizedLedgerKey const& lhs, GeneralizedLedgerKey const& rhs)
{
    return !(lhs == rhs);
}

// GeneralizedLedgerEntry -----------------------------------------------------
GeneralizedLedgerEntry::GeneralizedLedgerEntry()
    : GeneralizedLedgerEntry(GeneralizedLedgerEntryType::LEDGER_ENTRY)
{
}

GeneralizedLedgerEntry::GeneralizedLedgerEntry(GeneralizedLedgerEntryType t)
    : mType(t)
{
    construct();
}

GeneralizedLedgerEntry::GeneralizedLedgerEntry(LedgerEntry const& le)
    : GeneralizedLedgerEntry(GeneralizedLedgerEntryType::LEDGER_ENTRY)
{
    ledgerEntry() = le;
}

GeneralizedLedgerEntry::GeneralizedLedgerEntry(SponsorshipEntry const& se)
    : GeneralizedLedgerEntry(GeneralizedLedgerEntryType::SPONSORSHIP)
{
    sponsorshipEntry() = se;
}

GeneralizedLedgerEntry::GeneralizedLedgerEntry(
    SponsorshipCounterEntry const& sce)
    : GeneralizedLedgerEntry(GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER)
{
    sponsorshipCounterEntry() = sce;
}

GeneralizedLedgerEntry::GeneralizedLedgerEntry(
    GeneralizedLedgerEntry const& gle)
    : GeneralizedLedgerEntry(gle.type())
{
    assign(gle);
}

GeneralizedLedgerEntry::GeneralizedLedgerEntry(GeneralizedLedgerEntry&& gle)
    : GeneralizedLedgerEntry(gle.type())
{
    assign(std::move(gle));
}

GeneralizedLedgerEntry&
GeneralizedLedgerEntry::operator=(GeneralizedLedgerEntry const& gle)
{
    type(gle.type());
    assign(gle);
    return *this;
}

GeneralizedLedgerEntry&
GeneralizedLedgerEntry::operator=(GeneralizedLedgerEntry&& gle)
{
    if (this == &gle)
    {
        return *this;
    }

    type(gle.type());
    assign(std::move(gle));
    return *this;
}

GeneralizedLedgerEntry::~GeneralizedLedgerEntry()
{
    destruct();
}

void
GeneralizedLedgerEntry::assign(GeneralizedLedgerEntry const& gle)
{
    assert(gle.type() == mType);
    switch (mType)
    {
    case GeneralizedLedgerEntryType::LEDGER_ENTRY:
        ledgerEntry() = gle.ledgerEntry();
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP:
        sponsorshipEntry() = gle.sponsorshipEntry();
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER:
        sponsorshipCounterEntry() = gle.sponsorshipCounterEntry();
        break;
    default:
        abort();
    }
}

void
GeneralizedLedgerEntry::assign(GeneralizedLedgerEntry&& gle)
{
    assert(gle.type() == mType);
    switch (mType)
    {
    case GeneralizedLedgerEntryType::LEDGER_ENTRY:
        ledgerEntry() = std::move(gle.ledgerEntry());
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP:
        sponsorshipEntry() = std::move(gle.sponsorshipEntry());
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER:
        sponsorshipCounterEntry() = std::move(gle.sponsorshipCounterEntry());
        break;
    default:
        abort();
    }
}

void
GeneralizedLedgerEntry::construct()
{
    switch (mType)
    {
    case GeneralizedLedgerEntryType::LEDGER_ENTRY:
        new (&mLedgerEntry) LedgerEntry();
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP:
        new (&mSponsorshipEntry) SponsorshipEntry();
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER:
        new (&mSponsorshipCounterEntry) SponsorshipCounterEntry();
        break;
    default:
        abort();
    }
}

void
GeneralizedLedgerEntry::destruct()
{
    switch (mType)
    {
    case GeneralizedLedgerEntryType::LEDGER_ENTRY:
        mLedgerEntry.~LedgerEntry();
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP:
        mSponsorshipEntry.~SponsorshipEntry();
        break;
    case GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER:
        mSponsorshipCounterEntry.~SponsorshipCounterEntry();
        break;
    default:
        abort();
    }
}

void
GeneralizedLedgerEntry::type(GeneralizedLedgerEntryType t)
{
    if (t != mType)
    {
        destruct();
        mType = t;
        construct();
    }
}

GeneralizedLedgerEntryType
GeneralizedLedgerEntry::type() const
{
    return mType;
}

void
GeneralizedLedgerEntry::checkDiscriminant(
    GeneralizedLedgerEntryType expected) const
{
    if (mType != expected)
    {
        throw std::runtime_error("invalid union access");
    }
}

LedgerEntry&
GeneralizedLedgerEntry::ledgerEntry()
{
    checkDiscriminant(GeneralizedLedgerEntryType::LEDGER_ENTRY);
    return mLedgerEntry;
}

LedgerEntry const&
GeneralizedLedgerEntry::ledgerEntry() const
{
    checkDiscriminant(GeneralizedLedgerEntryType::LEDGER_ENTRY);
    return mLedgerEntry;
}

SponsorshipEntry&
GeneralizedLedgerEntry::sponsorshipEntry()
{
    checkDiscriminant(GeneralizedLedgerEntryType::SPONSORSHIP);
    return mSponsorshipEntry;
}

SponsorshipEntry const&
GeneralizedLedgerEntry::sponsorshipEntry() const
{
    checkDiscriminant(GeneralizedLedgerEntryType::SPONSORSHIP);
    return mSponsorshipEntry;
}

SponsorshipCounterEntry&
GeneralizedLedgerEntry::sponsorshipCounterEntry()
{
    checkDiscriminant(GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER);
    return mSponsorshipCounterEntry;
}

SponsorshipCounterEntry const&
GeneralizedLedgerEntry::sponsorshipCounterEntry() const
{
    checkDiscriminant(GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER);
    return mSponsorshipCounterEntry;
}

GeneralizedLedgerKey
GeneralizedLedgerEntry::toKey() const
{
    switch (mType)
    {
    case GeneralizedLedgerEntryType::LEDGER_ENTRY:
        return LedgerEntryKey(ledgerEntry());
    case GeneralizedLedgerEntryType::SPONSORSHIP:
        return GeneralizedLedgerKey(
            SponsorshipKey{sponsorshipEntry().sponsoredID});
    case GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER:
        return GeneralizedLedgerKey(
            SponsorshipCounterKey{sponsorshipCounterEntry().sponsoringID});
    default:
        abort();
    }
}

std::string
GeneralizedLedgerEntry::toString() const
{
    switch (mType)
    {
    case GeneralizedLedgerEntryType::LEDGER_ENTRY:
        return xdr_to_string(ledgerEntry());
    case GeneralizedLedgerEntryType::SPONSORSHIP:
        return fmt::format("{{\n  sponsoredID = {},\n  sponsoringID = {}\n}}\n",
                           xdr_to_string(sponsorshipEntry().sponsoredID),
                           xdr_to_string(sponsorshipEntry().sponsoringID));
    case GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER:
        return fmt::format(
            "{{\n  sponsoringID = {},\n  numSponsoring = {}\n}}\n",
            xdr_to_string(sponsorshipCounterEntry().sponsoringID),
            sponsorshipCounterEntry().numSponsoring);
    default:
        abort();
    }
}

bool
operator==(GeneralizedLedgerEntry const& lhs, GeneralizedLedgerEntry const& rhs)
{
    if (lhs.type() != rhs.type())
    {
        return false;
    }

    switch (lhs.type())
    {
    case GeneralizedLedgerEntryType::LEDGER_ENTRY:
        return lhs.ledgerEntry() == rhs.ledgerEntry();
    case GeneralizedLedgerEntryType::SPONSORSHIP:
        return lhs.sponsorshipEntry() == rhs.sponsorshipEntry();
    case GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER:
        return lhs.sponsorshipCounterEntry() == rhs.sponsorshipCounterEntry();
    default:
        abort();
    }
}

bool
operator!=(GeneralizedLedgerEntry const& lhs, GeneralizedLedgerEntry const& rhs)
{
    return !(lhs == rhs);
}
}
