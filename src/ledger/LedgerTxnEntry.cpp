// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxnEntry.h"
#include "ledger/InternalLedgerEntry.h"
#include "ledger/LedgerTxn.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{

// Implementation of LedgerTxnEntry -----------------------------------------
class LedgerTxnEntry::Impl : public EntryImplBase
{
    AbstractLedgerTxn& mLedgerTxn;
    InternalLedgerEntry& mCurrent;

  public:
    explicit Impl(AbstractLedgerTxn& ltx, InternalLedgerEntry& current);

    ~Impl() override;

    // Copy construction and copy assignment are forbidden.
    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;

    // Move construction and move assignment are forbidden.
    Impl(Impl&& other) = delete;
    Impl& operator=(Impl&& other) = delete;

    LedgerEntry& current();
    LedgerEntry const& current() const;

    InternalLedgerEntry& currentGeneralized();
    InternalLedgerEntry const& currentGeneralized() const;

    void deactivate();

    void erase();
};

std::shared_ptr<LedgerTxnEntry::Impl>
LedgerTxnEntry::makeSharedImpl(AbstractLedgerTxn& ltx,
                               InternalLedgerEntry& current)
{
    return std::make_shared<Impl>(ltx, current);
}

std::shared_ptr<EntryImplBase>
toEntryImplBase(std::shared_ptr<LedgerTxnEntry::Impl> const& impl)
{
    return impl;
}

LedgerTxnEntry::LedgerTxnEntry() : mIsInitialized(false)
{
}

LedgerTxnEntry::LedgerTxnEntry(std::shared_ptr<Impl> const& impl)
    : mImpl(impl) // Constructing weak_ptr from shared_ptr is noexcept
    , mIsInitialized(static_cast<bool>(impl))
{
}

LedgerTxnEntry::Impl::Impl(AbstractLedgerTxn& ltx, InternalLedgerEntry& current)
    : mLedgerTxn(ltx), mCurrent(current)
{
}

LedgerTxnEntry::~LedgerTxnEntry()
{
    auto impl = mImpl.lock();
    if (impl)
    {
        impl->deactivate();
    }
}

LedgerTxnEntry::Impl::~Impl()
{
}

LedgerTxnEntry::LedgerTxnEntry(LedgerTxnEntry&& other)
    : mImpl(std::move(other.mImpl)), mIsInitialized(other.mIsInitialized)
{
    // According to https://en.cppreference.com/w/cpp/memory/weak_ptr/weak_ptr
    // other.mImpl is empty after move-construction so reset is not required.
}

// Copy-and-swap implementation ensures that *this is properly destructed (and
// deactivated) if this->mImpl != nullptr, but note that self-assignment must
// still be handled explicitly since the copy would still deactivate the entry.
LedgerTxnEntry&
LedgerTxnEntry::operator=(LedgerTxnEntry&& other)
{
    if (this != &other)
    {
        LedgerTxnEntry otherCopy(other.mImpl.lock());
        swap(otherCopy);
        other.mImpl.reset();
        mIsInitialized = other.mIsInitialized;
    }
    return *this;
}

LedgerTxnEntry::operator bool() const
{
    if (!mImpl.expired())
    {
        return true;
    }

    if (!mIsInitialized)
    {
        return false;
    }

    // If we get here, we know we had an initialized entry, but the entry was
    // either deleted or deactivated. In either scenario, it's now invalid to
    // use this LedgerTxnEntry.
    throw std::runtime_error("Accessed deactivated entry");
}

LedgerEntry&
LedgerTxnEntry::current()
{
    return getImpl()->current();
}

LedgerEntry const&
LedgerTxnEntry::current() const
{
    return getImpl()->current();
}

LedgerEntry&
LedgerTxnEntry::Impl::current()
{
    return mCurrent.ledgerEntry();
}

LedgerEntry const&
LedgerTxnEntry::Impl::current() const
{
    return mCurrent.ledgerEntry();
}

InternalLedgerEntry&
LedgerTxnEntry::currentGeneralized()
{
    return getImpl()->currentGeneralized();
}

InternalLedgerEntry const&
LedgerTxnEntry::currentGeneralized() const
{
    return getImpl()->currentGeneralized();
}

InternalLedgerEntry&
LedgerTxnEntry::Impl::currentGeneralized()
{
    return mCurrent;
}

InternalLedgerEntry const&
LedgerTxnEntry::Impl::currentGeneralized() const
{
    return mCurrent;
}

void
LedgerTxnEntry::deactivate()
{
    getImpl()->deactivate();
}

void
LedgerTxnEntry::Impl::deactivate()
{
    auto key = mCurrent.toKey();
    mLedgerTxn.deactivate(key);
}

void
LedgerTxnEntry::erase()
{
    getImpl()->erase();
}

void
LedgerTxnEntry::Impl::erase()
{
    auto key = mCurrent.toKey();
    mLedgerTxn.erase(key);
}

std::shared_ptr<LedgerTxnEntry::Impl>
LedgerTxnEntry::getImpl()
{
    auto impl = mImpl.lock();
    if (!impl)
    {
        throw std::runtime_error("LedgerTxnEntry not active");
    }
    return impl;
}

std::shared_ptr<LedgerTxnEntry::Impl const>
LedgerTxnEntry::getImpl() const
{
    auto impl = mImpl.lock();
    if (!impl)
    {
        throw std::runtime_error("LedgerTxnEntry not active");
    }
    return impl;
}

void
LedgerTxnEntry::swap(LedgerTxnEntry& other)
{
    mImpl.swap(other.mImpl);
}

// Implementation of ConstLedgerTxnEntry ------------------------------------
class ConstLedgerTxnEntry::Impl : public EntryImplBase
{
    AbstractLedgerTxn& mLedgerTxn;
    InternalLedgerEntry const mCurrent;

  public:
    explicit Impl(AbstractLedgerTxn& ltx, InternalLedgerEntry const& current);

    ~Impl() override;

    // Copy construction and copy assignment are forbidden.
    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;

    // Move construction and move assignment are forbidden.
    Impl(Impl&& other) = delete;
    Impl& operator=(Impl&& other) = delete;

    LedgerEntry const& current() const;

    InternalLedgerEntry const& currentGeneralized() const;

    void deactivate();
};

std::shared_ptr<ConstLedgerTxnEntry::Impl>
ConstLedgerTxnEntry::makeSharedImpl(AbstractLedgerTxn& ltx,
                                    InternalLedgerEntry const& current)
{
    return std::make_shared<Impl>(ltx, current);
}

std::shared_ptr<EntryImplBase>
toEntryImplBase(std::shared_ptr<ConstLedgerTxnEntry::Impl> const& impl)
{
    return impl;
}

ConstLedgerTxnEntry::ConstLedgerTxnEntry() : mIsInitialized(false)
{
}

ConstLedgerTxnEntry::ConstLedgerTxnEntry(std::shared_ptr<Impl> const& impl)
    : mImpl(impl) // Constructing weak_ptr from shared_ptr is noexcept
    , mIsInitialized(static_cast<bool>(impl))
{
}

ConstLedgerTxnEntry::Impl::Impl(AbstractLedgerTxn& ltx,
                                InternalLedgerEntry const& current)
    : mLedgerTxn(ltx), mCurrent(current)
{
}

ConstLedgerTxnEntry::~ConstLedgerTxnEntry()
{
    auto impl = mImpl.lock();
    if (impl)
    {
        impl->deactivate();
    }
}

ConstLedgerTxnEntry::Impl::~Impl()
{
}

ConstLedgerTxnEntry::ConstLedgerTxnEntry(ConstLedgerTxnEntry&& other)
    : mImpl(std::move(other.mImpl)), mIsInitialized(other.mIsInitialized)
{
    // According to https://en.cppreference.com/w/cpp/memory/weak_ptr/weak_ptr
    // other.mImpl is empty after move-construction so reset is not required.
}

// Copy-and-swap implementation ensures that *this is properly destructed (and
// deactivated) if this->mImpl != nullptr, but note that self-assignment must
// still be handled explicitly since the copy would still deactivate the entry.
ConstLedgerTxnEntry&
ConstLedgerTxnEntry::operator=(ConstLedgerTxnEntry&& other)
{
    if (this != &other)
    {
        ConstLedgerTxnEntry otherCopy(other.mImpl.lock());
        swap(otherCopy);
        other.mImpl.reset();
        mIsInitialized = other.mIsInitialized;
    }
    return *this;
}

ConstLedgerTxnEntry::operator bool() const
{
    if (!mImpl.expired())
    {
        return true;
    }

    if (!mIsInitialized)
    {
        return false;
    }

    // If we get here, we know we had an initialized entry, but the entry was
    // either deleted or deactivated. In either scenario, it's now invalid to
    // use this LedgerTxnEntry.
    throw std::runtime_error("Accessed deactivated entry");
}

LedgerEntry const&
ConstLedgerTxnEntry::current() const
{
    return getImpl()->current();
}

LedgerEntry const&
ConstLedgerTxnEntry::Impl::current() const
{
    return mCurrent.ledgerEntry();
}

InternalLedgerEntry const&
ConstLedgerTxnEntry::currentGeneralized() const
{
    return getImpl()->currentGeneralized();
}

InternalLedgerEntry const&
ConstLedgerTxnEntry::Impl::currentGeneralized() const
{
    return mCurrent;
}

std::shared_ptr<ConstLedgerTxnEntry::Impl>
ConstLedgerTxnEntry::getImpl()
{
    auto impl = mImpl.lock();
    if (!impl)
    {
        throw std::runtime_error("ConstLedgerTxnEntry not active");
    }
    return impl;
}

std::shared_ptr<ConstLedgerTxnEntry::Impl const>
ConstLedgerTxnEntry::getImpl() const
{
    auto impl = mImpl.lock();
    if (!impl)
    {
        throw std::runtime_error("ConstLedgerTxnEntry not active");
    }
    return impl;
}

void
ConstLedgerTxnEntry::deactivate()
{
    getImpl()->deactivate();
}

void
ConstLedgerTxnEntry::Impl::deactivate()
{
    auto key = mCurrent.toKey();
    mLedgerTxn.deactivate(key);
}

void
ConstLedgerTxnEntry::swap(ConstLedgerTxnEntry& other)
{
    mImpl.swap(other.mImpl);
}
}
