// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/InternalLedgerEntry.h"
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

// InternalLedgerKey -------------------------------------------------------
InternalLedgerKey::InternalLedgerKey()
    : InternalLedgerKey(InternalLedgerEntryType::LEDGER_ENTRY)
{
}

InternalLedgerKey::InternalLedgerKey(InternalLedgerEntryType t) : mType(t)
{
    construct();
}

InternalLedgerKey::InternalLedgerKey(LedgerKey const& lk)
    : InternalLedgerKey(InternalLedgerEntryType::LEDGER_ENTRY)
{
    ledgerKey() = lk;
}

InternalLedgerKey::InternalLedgerKey(SponsorshipKey const& sk)
    : InternalLedgerKey(InternalLedgerEntryType::SPONSORSHIP)
{
    sponsorshipKey() = sk;
}

InternalLedgerKey::InternalLedgerKey(SponsorshipCounterKey const& sck)
    : InternalLedgerKey(InternalLedgerEntryType::SPONSORSHIP_COUNTER)
{
    sponsorshipCounterKey() = sck;
}

InternalLedgerKey::InternalLedgerKey(InternalLedgerKey const& glk)
    : InternalLedgerKey(glk.type())
{
    assign(glk);
}

InternalLedgerKey::InternalLedgerKey(InternalLedgerKey&& glk)
    : InternalLedgerKey(glk.type())
{
    assign(std::move(glk));
}

InternalLedgerKey&
InternalLedgerKey::operator=(InternalLedgerKey const& glk)
{
    type(glk.type());
    assign(glk);
    return *this;
}

InternalLedgerKey&
InternalLedgerKey::operator=(InternalLedgerKey&& glk)
{
    if (this == &glk)
    {
        return *this;
    }

    type(glk.type());
    assign(std::move(glk));
    return *this;
}

InternalLedgerKey::~InternalLedgerKey()
{
    destruct();
}

void
InternalLedgerKey::assign(InternalLedgerKey const& glk)
{
    assert(glk.type() == mType);
    switch (mType)
    {
    case InternalLedgerEntryType::LEDGER_ENTRY:
        ledgerKey() = glk.ledgerKey();
        break;
    case InternalLedgerEntryType::SPONSORSHIP:
        sponsorshipKey() = glk.sponsorshipKey();
        break;
    case InternalLedgerEntryType::SPONSORSHIP_COUNTER:
        sponsorshipCounterKey() = glk.sponsorshipCounterKey();
        break;
    default:
        abort();
    }
}

void
InternalLedgerKey::assign(InternalLedgerKey&& glk)
{
    assert(glk.type() == mType);
    switch (mType)
    {
    case InternalLedgerEntryType::LEDGER_ENTRY:
        ledgerKey() = std::move(glk.ledgerKey());
        break;
    case InternalLedgerEntryType::SPONSORSHIP:
        sponsorshipKey() = std::move(glk.sponsorshipKey());
        break;
    case InternalLedgerEntryType::SPONSORSHIP_COUNTER:
        sponsorshipCounterKey() = std::move(glk.sponsorshipCounterKey());
        break;
    default:
        abort();
    }
}

void
InternalLedgerKey::construct()
{
    switch (mType)
    {
    case InternalLedgerEntryType::LEDGER_ENTRY:
        new (&mLedgerKey) LedgerKey();
        break;
    case InternalLedgerEntryType::SPONSORSHIP:
        new (&mSponsorshipKey) SponsorshipKey();
        break;
    case InternalLedgerEntryType::SPONSORSHIP_COUNTER:
        new (&mSponsorshipCounterKey) SponsorshipCounterKey();
        break;
    default:
        abort();
    }
}

void
InternalLedgerKey::destruct()
{
    switch (mType)
    {
    case InternalLedgerEntryType::LEDGER_ENTRY:
        mLedgerKey.~LedgerKey();
        break;
    case InternalLedgerEntryType::SPONSORSHIP:
        mSponsorshipKey.~SponsorshipKey();
        break;
    case InternalLedgerEntryType::SPONSORSHIP_COUNTER:
        mSponsorshipCounterKey.~SponsorshipCounterKey();
        break;
    default:
        abort();
    }
}

void
InternalLedgerKey::type(InternalLedgerEntryType t)
{
    if (t != mType)
    {
        destruct();
        mType = t;
        construct();
    }
}

InternalLedgerEntryType
InternalLedgerKey::type() const
{
    return mType;
}

void
InternalLedgerKey::checkDiscriminant(InternalLedgerEntryType expected) const
{
    if (mType != expected)
    {
        throw std::runtime_error("invalid union access");
    }
}

LedgerKey&
InternalLedgerKey::ledgerKey()
{
    checkDiscriminant(InternalLedgerEntryType::LEDGER_ENTRY);
    return mLedgerKey;
}

LedgerKey const&
InternalLedgerKey::ledgerKey() const
{
    checkDiscriminant(InternalLedgerEntryType::LEDGER_ENTRY);
    return mLedgerKey;
}

SponsorshipKey&
InternalLedgerKey::sponsorshipKey()
{
    checkDiscriminant(InternalLedgerEntryType::SPONSORSHIP);
    return mSponsorshipKey;
}

SponsorshipKey const&
InternalLedgerKey::sponsorshipKey() const
{
    checkDiscriminant(InternalLedgerEntryType::SPONSORSHIP);
    return mSponsorshipKey;
}

SponsorshipCounterKey&
InternalLedgerKey::sponsorshipCounterKey()
{
    checkDiscriminant(InternalLedgerEntryType::SPONSORSHIP_COUNTER);
    return mSponsorshipCounterKey;
}

SponsorshipCounterKey const&
InternalLedgerKey::sponsorshipCounterKey() const
{
    checkDiscriminant(InternalLedgerEntryType::SPONSORSHIP_COUNTER);
    return mSponsorshipCounterKey;
}

std::string
InternalLedgerKey::toString() const
{
    switch (mType)
    {
    case InternalLedgerEntryType::LEDGER_ENTRY:
        return xdr_to_string(ledgerKey(), "LedgerKey");

    case InternalLedgerEntryType::SPONSORSHIP:
        return fmt::format(
            "{{\n  {}\n}}\n",
            xdr_to_string(sponsorshipKey().sponsoredID, "sponsoredID"));
    case InternalLedgerEntryType::SPONSORSHIP_COUNTER:
        return fmt::format("{{\n  {}\n}}\n",
                           xdr_to_string(sponsorshipCounterKey().sponsoringID,
                                         "sponsoringID"));
    default:
        abort();
    }
}

bool
operator==(InternalLedgerKey const& lhs, InternalLedgerKey const& rhs)
{
    if (lhs.type() != rhs.type())
    {
        return false;
    }

    switch (lhs.type())
    {
    case InternalLedgerEntryType::LEDGER_ENTRY:
        return lhs.ledgerKey() == rhs.ledgerKey();
    case InternalLedgerEntryType::SPONSORSHIP:
        return lhs.sponsorshipKey() == rhs.sponsorshipKey();
    case InternalLedgerEntryType::SPONSORSHIP_COUNTER:
        return lhs.sponsorshipCounterKey() == rhs.sponsorshipCounterKey();
    default:
        abort();
    }
}

bool
operator!=(InternalLedgerKey const& lhs, InternalLedgerKey const& rhs)
{
    return !(lhs == rhs);
}

// InternalLedgerEntry -----------------------------------------------------
InternalLedgerEntry::InternalLedgerEntry()
    : InternalLedgerEntry(InternalLedgerEntryType::LEDGER_ENTRY)
{
}

InternalLedgerEntry::InternalLedgerEntry(InternalLedgerEntryType t) : mType(t)
{
    construct();
}

InternalLedgerEntry::InternalLedgerEntry(LedgerEntry const& le)
    : InternalLedgerEntry(InternalLedgerEntryType::LEDGER_ENTRY)
{
    ledgerEntry() = le;
}

InternalLedgerEntry::InternalLedgerEntry(SponsorshipEntry const& se)
    : InternalLedgerEntry(InternalLedgerEntryType::SPONSORSHIP)
{
    sponsorshipEntry() = se;
}

InternalLedgerEntry::InternalLedgerEntry(SponsorshipCounterEntry const& sce)
    : InternalLedgerEntry(InternalLedgerEntryType::SPONSORSHIP_COUNTER)
{
    sponsorshipCounterEntry() = sce;
}

InternalLedgerEntry::InternalLedgerEntry(InternalLedgerEntry const& gle)
    : InternalLedgerEntry(gle.type())
{
    assign(gle);
}

InternalLedgerEntry::InternalLedgerEntry(InternalLedgerEntry&& gle)
    : InternalLedgerEntry(gle.type())
{
    assign(std::move(gle));
}

InternalLedgerEntry&
InternalLedgerEntry::operator=(InternalLedgerEntry const& gle)
{
    type(gle.type());
    assign(gle);
    return *this;
}

InternalLedgerEntry&
InternalLedgerEntry::operator=(InternalLedgerEntry&& gle)
{
    if (this == &gle)
    {
        return *this;
    }

    type(gle.type());
    assign(std::move(gle));
    return *this;
}

InternalLedgerEntry::~InternalLedgerEntry()
{
    destruct();
}

void
InternalLedgerEntry::assign(InternalLedgerEntry const& gle)
{
    assert(gle.type() == mType);
    switch (mType)
    {
    case InternalLedgerEntryType::LEDGER_ENTRY:
        ledgerEntry() = gle.ledgerEntry();
        break;
    case InternalLedgerEntryType::SPONSORSHIP:
        sponsorshipEntry() = gle.sponsorshipEntry();
        break;
    case InternalLedgerEntryType::SPONSORSHIP_COUNTER:
        sponsorshipCounterEntry() = gle.sponsorshipCounterEntry();
        break;
    default:
        abort();
    }
}

void
InternalLedgerEntry::assign(InternalLedgerEntry&& gle)
{
    assert(gle.type() == mType);
    switch (mType)
    {
    case InternalLedgerEntryType::LEDGER_ENTRY:
        ledgerEntry() = std::move(gle.ledgerEntry());
        break;
    case InternalLedgerEntryType::SPONSORSHIP:
        sponsorshipEntry() = std::move(gle.sponsorshipEntry());
        break;
    case InternalLedgerEntryType::SPONSORSHIP_COUNTER:
        sponsorshipCounterEntry() = std::move(gle.sponsorshipCounterEntry());
        break;
    default:
        abort();
    }
}

void
InternalLedgerEntry::construct()
{
    switch (mType)
    {
    case InternalLedgerEntryType::LEDGER_ENTRY:
        new (&mLedgerEntry) LedgerEntry();
        break;
    case InternalLedgerEntryType::SPONSORSHIP:
        new (&mSponsorshipEntry) SponsorshipEntry();
        break;
    case InternalLedgerEntryType::SPONSORSHIP_COUNTER:
        new (&mSponsorshipCounterEntry) SponsorshipCounterEntry();
        break;
    default:
        abort();
    }
}

void
InternalLedgerEntry::destruct()
{
    switch (mType)
    {
    case InternalLedgerEntryType::LEDGER_ENTRY:
        mLedgerEntry.~LedgerEntry();
        break;
    case InternalLedgerEntryType::SPONSORSHIP:
        mSponsorshipEntry.~SponsorshipEntry();
        break;
    case InternalLedgerEntryType::SPONSORSHIP_COUNTER:
        mSponsorshipCounterEntry.~SponsorshipCounterEntry();
        break;
    default:
        abort();
    }
}

void
InternalLedgerEntry::type(InternalLedgerEntryType t)
{
    if (t != mType)
    {
        destruct();
        mType = t;
        construct();
    }
}

InternalLedgerEntryType
InternalLedgerEntry::type() const
{
    return mType;
}

void
InternalLedgerEntry::checkDiscriminant(InternalLedgerEntryType expected) const
{
    if (mType != expected)
    {
        throw std::runtime_error("invalid union access");
    }
}

LedgerEntry&
InternalLedgerEntry::ledgerEntry()
{
    checkDiscriminant(InternalLedgerEntryType::LEDGER_ENTRY);
    return mLedgerEntry;
}

LedgerEntry const&
InternalLedgerEntry::ledgerEntry() const
{
    checkDiscriminant(InternalLedgerEntryType::LEDGER_ENTRY);
    return mLedgerEntry;
}

SponsorshipEntry&
InternalLedgerEntry::sponsorshipEntry()
{
    checkDiscriminant(InternalLedgerEntryType::SPONSORSHIP);
    return mSponsorshipEntry;
}

SponsorshipEntry const&
InternalLedgerEntry::sponsorshipEntry() const
{
    checkDiscriminant(InternalLedgerEntryType::SPONSORSHIP);
    return mSponsorshipEntry;
}

SponsorshipCounterEntry&
InternalLedgerEntry::sponsorshipCounterEntry()
{
    checkDiscriminant(InternalLedgerEntryType::SPONSORSHIP_COUNTER);
    return mSponsorshipCounterEntry;
}

SponsorshipCounterEntry const&
InternalLedgerEntry::sponsorshipCounterEntry() const
{
    checkDiscriminant(InternalLedgerEntryType::SPONSORSHIP_COUNTER);
    return mSponsorshipCounterEntry;
}

InternalLedgerKey
InternalLedgerEntry::toKey() const
{
    switch (mType)
    {
    case InternalLedgerEntryType::LEDGER_ENTRY:
        return LedgerEntryKey(ledgerEntry());
    case InternalLedgerEntryType::SPONSORSHIP:
        return InternalLedgerKey(
            SponsorshipKey{sponsorshipEntry().sponsoredID});
    case InternalLedgerEntryType::SPONSORSHIP_COUNTER:
        return InternalLedgerKey(
            SponsorshipCounterKey{sponsorshipCounterEntry().sponsoringID});
    default:
        abort();
    }
}

std::string
InternalLedgerEntry::toString() const
{
    switch (mType)
    {
    case InternalLedgerEntryType::LEDGER_ENTRY:
        return xdr_to_string(ledgerEntry(), "LedgerEntry");
    case InternalLedgerEntryType::SPONSORSHIP:
        return fmt::format(
            "{{\n  {},\n  {}\n}}\n",
            xdr_to_string(sponsorshipEntry().sponsoredID, "sponsoredID"),
            xdr_to_string(sponsorshipEntry().sponsoringID, "sponsoringID"));
    case InternalLedgerEntryType::SPONSORSHIP_COUNTER:
        return fmt::format("{{\n  {},\n  numSponsoring = {}\n}}\n",
                           xdr_to_string(sponsorshipCounterEntry().sponsoringID,
                                         "sponsoringID"),
                           sponsorshipCounterEntry().numSponsoring);
    default:
        abort();
    }
}

bool
operator==(InternalLedgerEntry const& lhs, InternalLedgerEntry const& rhs)
{
    if (lhs.type() != rhs.type())
    {
        return false;
    }

    switch (lhs.type())
    {
    case InternalLedgerEntryType::LEDGER_ENTRY:
        return lhs.ledgerEntry() == rhs.ledgerEntry();
    case InternalLedgerEntryType::SPONSORSHIP:
        return lhs.sponsorshipEntry() == rhs.sponsorshipEntry();
    case InternalLedgerEntryType::SPONSORSHIP_COUNTER:
        return lhs.sponsorshipCounterEntry() == rhs.sponsorshipCounterEntry();
    default:
        abort();
    }
}

bool
operator!=(InternalLedgerEntry const& lhs, InternalLedgerEntry const& rhs)
{
    return !(lhs == rhs);
}
}
