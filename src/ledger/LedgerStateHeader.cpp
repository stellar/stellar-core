// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerStateHeader.h"
#include "ledger/LedgerState.h"

namespace stellar
{

class LedgerStateHeader::Impl
{
    AbstractLedgerState& mLedgerState;
    LedgerHeader& mCurrent;

  public:
    explicit Impl(AbstractLedgerState& ls, LedgerHeader& current);

    // Copy construction and copy assignment are forbidden.
    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;

    // Move construction and move assignment are forbidden.
    Impl(Impl&& other) = delete;
    Impl& operator=(Impl&& other) = delete;

    LedgerHeader& current();
    LedgerHeader const& current() const;

    void deactivate();
};

std::shared_ptr<LedgerStateHeader::Impl>
LedgerStateHeader::makeSharedImpl(AbstractLedgerState& ls,
                                  LedgerHeader& current)
{
    return std::make_shared<Impl>(ls, current);
}

LedgerStateHeader::LedgerStateHeader(std::shared_ptr<Impl> const& impl)
    : mImpl(impl) // Constructing weak_ptr from shared_ptr is noexcept
{
}

LedgerStateHeader::Impl::Impl(AbstractLedgerState& ls, LedgerHeader& current)
    : mLedgerState(ls), mCurrent(current)
{
}

LedgerStateHeader::~LedgerStateHeader()
{
    auto impl = mImpl.lock();
    if (impl)
    {
        impl->deactivate();
    }
}

LedgerStateHeader::LedgerStateHeader(LedgerStateHeader&& other)
    : mImpl(std::move(other.mImpl))
{
    // According to https://en.cppreference.com/w/cpp/memory/weak_ptr/weak_ptr
    // other.mImpl is empty after move-construction so reset is not required.
}

// Copy-and-swap implementation ensures that *this is properly destructed (and
// deactivated) if this->mImpl != nullptr, but note that self-assignment must
// still be handled explicitly since the copy would still deactivate the entry.
LedgerStateHeader&
LedgerStateHeader::operator=(LedgerStateHeader&& other)
{
    if (this != &other)
    {
        LedgerStateHeader otherCopy(other.mImpl.lock());
        swap(otherCopy);
        other.mImpl.reset();
    }
    return *this;
}

LedgerStateHeader::operator bool() const
{
    return !mImpl.expired();
}

LedgerHeader&
LedgerStateHeader::current()
{
    return getImpl()->current();
}

LedgerHeader const&
LedgerStateHeader::current() const
{
    return getImpl()->current();
}

LedgerHeader&
LedgerStateHeader::Impl::current()
{
    return mCurrent;
}

LedgerHeader const&
LedgerStateHeader::Impl::current() const
{
    return mCurrent;
}

void
LedgerStateHeader::deactivate()
{
    getImpl()->deactivate();
}

void
LedgerStateHeader::Impl::deactivate()
{
    mLedgerState.deactivateHeader();
}

std::shared_ptr<LedgerStateHeader::Impl>
LedgerStateHeader::getImpl()
{
    auto impl = mImpl.lock();
    if (!impl)
    {
        throw std::runtime_error("LedgerStateHeader not active");
    }
    return impl;
}

std::shared_ptr<LedgerStateHeader::Impl const>
LedgerStateHeader::getImpl() const
{
    auto impl = mImpl.lock();
    if (!impl)
    {
        throw std::runtime_error("LedgerStateHeader not active");
    }
    return impl;
}

void
LedgerStateHeader::swap(LedgerStateHeader& other)
{
    mImpl.swap(other.mImpl);
}
}
