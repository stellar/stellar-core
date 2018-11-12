// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerStateEntry.h"
#include "ledger/LedgerState.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{

// Implementation of LedgerStateEntry -----------------------------------------
class LedgerStateEntry::Impl : public EntryImplBase
{
    AbstractLedgerState& mLedgerState;
    LedgerEntry& mCurrent;

  public:
    explicit Impl(AbstractLedgerState& ls, LedgerEntry& current);

    ~Impl() override;

    // Copy construction and copy assignment are forbidden.
    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;

    // Move construction and move assignment are forbidden.
    Impl(Impl&& other) = delete;
    Impl& operator=(Impl&& other) = delete;

    LedgerEntry& current();
    LedgerEntry const& current() const;

    void deactivate();

    void erase();
};

std::shared_ptr<LedgerStateEntry::Impl>
LedgerStateEntry::makeSharedImpl(AbstractLedgerState& ls, LedgerEntry& current)
{
    return std::make_shared<Impl>(ls, current);
}

std::shared_ptr<EntryImplBase>
toEntryImplBase(std::shared_ptr<LedgerStateEntry::Impl> const& impl)
{
    return impl;
}

LedgerStateEntry::LedgerStateEntry()
{
}

LedgerStateEntry::LedgerStateEntry(std::shared_ptr<Impl> const& impl)
    : mImpl(impl) // Constructing weak_ptr from shared_ptr is noexcept
{
}

LedgerStateEntry::Impl::Impl(AbstractLedgerState& ls, LedgerEntry& current)
    : mLedgerState(ls), mCurrent(current)
{
}

LedgerStateEntry::~LedgerStateEntry()
{
    auto impl = mImpl.lock();
    if (impl)
    {
        impl->deactivate();
    }
}

LedgerStateEntry::Impl::~Impl()
{
}

LedgerStateEntry::LedgerStateEntry(LedgerStateEntry&& other)
    : mImpl(std::move(other.mImpl))
{
    // According to https://en.cppreference.com/w/cpp/memory/weak_ptr/weak_ptr
    // other.mImpl is empty after move-construction so reset is not required.
}

// Copy-and-swap implementation ensures that *this is properly destructed (and
// deactivated) if this->mImpl != nullptr, but note that self-assignment must
// still be handled explicitly since the copy would still deactivate the entry.
LedgerStateEntry&
LedgerStateEntry::operator=(LedgerStateEntry&& other)
{
    if (this != &other)
    {
        LedgerStateEntry otherCopy(other.mImpl.lock());
        swap(otherCopy);
        other.mImpl.reset();
    }
    return *this;
}

LedgerStateEntry::operator bool() const
{
    return !mImpl.expired();
}

LedgerEntry&
LedgerStateEntry::current()
{
    return getImpl()->current();
}

LedgerEntry const&
LedgerStateEntry::current() const
{
    return getImpl()->current();
}

LedgerEntry&
LedgerStateEntry::Impl::current()
{
    return mCurrent;
}

LedgerEntry const&
LedgerStateEntry::Impl::current() const
{
    return mCurrent;
}

void
LedgerStateEntry::deactivate()
{
    getImpl()->deactivate();
}

void
LedgerStateEntry::Impl::deactivate()
{
    auto key = LedgerEntryKey(mCurrent);
    mLedgerState.deactivate(key);
}

void
LedgerStateEntry::erase()
{
    getImpl()->erase();
}

void
LedgerStateEntry::Impl::erase()
{
    auto key = LedgerEntryKey(mCurrent);
    mLedgerState.erase(key);
}

std::shared_ptr<LedgerStateEntry::Impl>
LedgerStateEntry::getImpl()
{
    auto impl = mImpl.lock();
    if (!impl)
    {
        throw std::runtime_error("LedgerStateEntry not active");
    }
    return impl;
}

std::shared_ptr<LedgerStateEntry::Impl const>
LedgerStateEntry::getImpl() const
{
    auto impl = mImpl.lock();
    if (!impl)
    {
        throw std::runtime_error("LedgerStateEntry not active");
    }
    return impl;
}

void
LedgerStateEntry::swap(LedgerStateEntry& other)
{
    mImpl.swap(other.mImpl);
}

// Implementation of ConstLedgerStateEntry ------------------------------------
class ConstLedgerStateEntry::Impl : public EntryImplBase
{
    AbstractLedgerState& mLedgerState;
    LedgerEntry const mCurrent;

  public:
    explicit Impl(AbstractLedgerState& ls, LedgerEntry const& current);

    ~Impl() override;

    // Copy construction and copy assignment are forbidden.
    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;

    // Move construction and move assignment are forbidden.
    Impl(Impl&& other) = delete;
    Impl& operator=(Impl&& other) = delete;

    LedgerEntry const& current() const;

    void deactivate();
};

std::shared_ptr<ConstLedgerStateEntry::Impl>
ConstLedgerStateEntry::makeSharedImpl(AbstractLedgerState& ls,
                                      LedgerEntry const& current)
{
    return std::make_shared<Impl>(ls, current);
}

std::shared_ptr<EntryImplBase>
toEntryImplBase(std::shared_ptr<ConstLedgerStateEntry::Impl> const& impl)
{
    return impl;
}

ConstLedgerStateEntry::ConstLedgerStateEntry()
{
}

ConstLedgerStateEntry::ConstLedgerStateEntry(std::shared_ptr<Impl> const& impl)
    : mImpl(impl) // Constructing weak_ptr from shared_ptr is noexcept
{
}

ConstLedgerStateEntry::Impl::Impl(AbstractLedgerState& ls,
                                  LedgerEntry const& current)
    : mLedgerState(ls), mCurrent(current)
{
}

ConstLedgerStateEntry::~ConstLedgerStateEntry()
{
    auto impl = mImpl.lock();
    if (impl)
    {
        impl->deactivate();
    }
}

ConstLedgerStateEntry::Impl::~Impl()
{
}

ConstLedgerStateEntry::ConstLedgerStateEntry(ConstLedgerStateEntry&& other)
    : mImpl(std::move(other.mImpl))
{
    // According to https://en.cppreference.com/w/cpp/memory/weak_ptr/weak_ptr
    // other.mImpl is empty after move-construction so reset is not required.
}

// Copy-and-swap implementation ensures that *this is properly destructed (and
// deactivated) if this->mImpl != nullptr, but note that self-assignment must
// still be handled explicitly since the copy would still deactivate the entry.
ConstLedgerStateEntry&
ConstLedgerStateEntry::operator=(ConstLedgerStateEntry&& other)
{
    if (this != &other)
    {
        ConstLedgerStateEntry otherCopy(other.mImpl.lock());
        swap(otherCopy);
        other.mImpl.reset();
    }
    return *this;
}

ConstLedgerStateEntry::operator bool() const
{
    return !mImpl.expired();
}

LedgerEntry const&
ConstLedgerStateEntry::current() const
{
    return getImpl()->current();
}

LedgerEntry const&
ConstLedgerStateEntry::Impl::current() const
{
    return mCurrent;
}

std::shared_ptr<ConstLedgerStateEntry::Impl>
ConstLedgerStateEntry::getImpl()
{
    auto impl = mImpl.lock();
    if (!impl)
    {
        throw std::runtime_error("ConstLedgerStateEntry not active");
    }
    return impl;
}

std::shared_ptr<ConstLedgerStateEntry::Impl const>
ConstLedgerStateEntry::getImpl() const
{
    auto impl = mImpl.lock();
    if (!impl)
    {
        throw std::runtime_error("ConstLedgerStateEntry not active");
    }
    return impl;
}

void
ConstLedgerStateEntry::deactivate()
{
    getImpl()->deactivate();
}

void
ConstLedgerStateEntry::Impl::deactivate()
{
    auto key = LedgerEntryKey(mCurrent);
    mLedgerState.deactivate(key);
}

void
ConstLedgerStateEntry::swap(ConstLedgerStateEntry& other)
{
    mImpl.swap(other.mImpl);
}
}
