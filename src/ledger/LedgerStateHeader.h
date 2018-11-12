#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>

namespace stellar
{

class AbstractLedgerState;
struct LedgerHeader;

class LedgerStateHeader
{
  public:
    class Impl;

  private:
    std::weak_ptr<Impl> mImpl;

    std::shared_ptr<Impl> getImpl();
    std::shared_ptr<Impl const> getImpl() const;

  public:
    // LedgerStateEntry constructors do not throw
    explicit LedgerStateHeader(std::shared_ptr<Impl> const& impl);

    ~LedgerStateHeader();

    // Copy construction and copy assignment are forbidden.
    LedgerStateHeader(LedgerStateHeader const&) = delete;
    LedgerStateHeader& operator=(LedgerStateHeader const&) = delete;

    // Move construction and move assignment are permitted.
    LedgerStateHeader(LedgerStateHeader&& other);
    LedgerStateHeader& operator=(LedgerStateHeader&& other);

    explicit operator bool() const;

    LedgerHeader& current();
    LedgerHeader const& current() const;

    void deactivate();

    void swap(LedgerStateHeader& other);

    static std::shared_ptr<Impl> makeSharedImpl(AbstractLedgerState& ls,
                                                LedgerHeader& current);
};
}
