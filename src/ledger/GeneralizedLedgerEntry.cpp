// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/GeneralizedLedgerEntry.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include "xdrpp/printer.h"

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

std::string
GeneralizedLedgerKey::toString() const
{
    switch (mType)
    {
    case GeneralizedLedgerEntryType::LEDGER_ENTRY:
        return xdr::xdr_to_string(ledgerKey());
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

GeneralizedLedgerKey
GeneralizedLedgerEntry::toKey() const
{
    switch (mType)
    {
    case GeneralizedLedgerEntryType::LEDGER_ENTRY:
        return LedgerEntryKey(ledgerEntry());
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
        return xdr::xdr_to_string(ledgerEntry());
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
