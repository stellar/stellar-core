// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxnHeader.h"
#include "ledger/LedgerTxn.h"

namespace stellar
{

class LedgerTxnHeader::Impl
{
    AbstractLedgerTxn& mLedgerTxn;
    LedgerHeader& mCurrent;

  public:
    explicit Impl(AbstractLedgerTxn& ltx, LedgerHeader& current);

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

std::shared_ptr<LedgerTxnHeader::Impl>
LedgerTxnHeader::makeSharedImpl(AbstractLedgerTxn& ltx, LedgerHeader& current)
{
    return std::make_shared<Impl>(ltx, current);
}

LedgerTxnHeader::LedgerTxnHeader(std::shared_ptr<Impl> const& impl)
    : mImpl(impl) // Constructing weak_ptr from shared_ptr is noexcept
{
}

LedgerTxnHeader::Impl::Impl(AbstractLedgerTxn& ltx, LedgerHeader& current)
    : mLedgerTxn(ltx), mCurrent(current)
{
}

LedgerTxnHeader::~LedgerTxnHeader()
{
    auto impl = mImpl.lock();
    if (impl)
    {
        impl->deactivate();
    }
}

LedgerTxnHeader::LedgerTxnHeader(LedgerTxnHeader&& other)
    : mImpl(std::move(other.mImpl))
{
    // According to https://en.cppreference.com/w/cpp/memory/weak_ptr/weak_ptr
    // other.mImpl is empty after move-construction so reset is not required.
}

// Copy-and-swap implementation ensures that *this is properly destructed (and
// deactivated) if this->mImpl != nullptr, but note that self-assignment must
// still be handled explicitly since the copy would still deactivate the entry.
LedgerTxnHeader&
LedgerTxnHeader::operator=(LedgerTxnHeader&& other)
{
    if (this != &other)
    {
        LedgerTxnHeader otherCopy(other.mImpl.lock());
        swap(otherCopy);
        other.mImpl.reset();
    }
    return *this;
}

LedgerTxnHeader::operator bool() const
{
    return !mImpl.expired();
}

LedgerHeader&
LedgerTxnHeader::current()
{
    return getImpl()->current();
}

LedgerHeader const&
LedgerTxnHeader::current() const
{
    return getImpl()->current();
}

LedgerHeader&
LedgerTxnHeader::Impl::current()
{
    return mCurrent;
}

LedgerHeader const&
LedgerTxnHeader::Impl::current() const
{
    return mCurrent;
}

void
LedgerTxnHeader::deactivate()
{
    getImpl()->deactivate();
}

void
LedgerTxnHeader::Impl::deactivate()
{
    mLedgerTxn.deactivateHeader();
}

std::shared_ptr<LedgerTxnHeader::Impl>
LedgerTxnHeader::getImpl()
{
    auto impl = mImpl.lock();
    if (!impl)
    {
        throw std::runtime_error("LedgerTxnHeader not active");
    }
    return impl;
}

std::shared_ptr<LedgerTxnHeader::Impl const>
LedgerTxnHeader::getImpl() const
{
    auto impl = mImpl.lock();
    if (!impl)
    {
        throw std::runtime_error("LedgerTxnHeader not active");
    }
    return impl;
}

void
LedgerTxnHeader::swap(LedgerTxnHeader& other)
{
    mImpl.swap(other.mImpl);
}
}
