#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>

namespace stellar
{

class AbstractLedgerTxn;
struct LedgerHeader;

class LedgerTxnHeader
{
  public:
    class Impl;

  private:
    std::weak_ptr<Impl> mImpl;

    std::shared_ptr<Impl> getImpl();
    std::shared_ptr<Impl const> getImpl() const;

  public:
    // LedgerTxnEntry constructors do not throw
    explicit LedgerTxnHeader(std::shared_ptr<Impl> const& impl);

    ~LedgerTxnHeader();

    // Copy construction and copy assignment are forbidden.
    LedgerTxnHeader(LedgerTxnHeader const&) = delete;
    LedgerTxnHeader& operator=(LedgerTxnHeader const&) = delete;

    // Move construction and move assignment are permitted.
    LedgerTxnHeader(LedgerTxnHeader&& other);
    LedgerTxnHeader& operator=(LedgerTxnHeader&& other);

    explicit operator bool() const;

    LedgerHeader& current();
    LedgerHeader const& current() const;

    void deactivate();

    void swap(LedgerTxnHeader& other);

    static std::shared_ptr<Impl> makeSharedImpl(AbstractLedgerTxn& ltx,
                                                LedgerHeader& current);
};
}
