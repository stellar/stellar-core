#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger-entries.h"
#include <memory>

namespace stellar
{

class AbstractLedgerTxn;

class EntryImplBase
{
  public:
    virtual ~EntryImplBase()
    {
    }
};

class LedgerTxnEntry
{
  public:
    class Impl;

  private:
    std::weak_ptr<Impl> mImpl;

    std::shared_ptr<Impl> getImpl();
    std::shared_ptr<Impl const> getImpl() const;

  public:
    // LedgerTxnEntry constructors do not throw
    LedgerTxnEntry();
    explicit LedgerTxnEntry(std::shared_ptr<Impl> const& impl);

    ~LedgerTxnEntry();

    // Copy construction and copy assignment are forbidden.
    LedgerTxnEntry(LedgerTxnEntry const&) = delete;
    LedgerTxnEntry& operator=(LedgerTxnEntry const&) = delete;

    // Move construction and move assignment are permitted.
    LedgerTxnEntry(LedgerTxnEntry&& other);
    LedgerTxnEntry& operator=(LedgerTxnEntry&& other);

    explicit operator bool() const;

    LedgerEntry& current();
    LedgerEntry const& current() const;

    void deactivate();

    void erase();

    void swap(LedgerTxnEntry& other);

    static std::shared_ptr<Impl> makeSharedImpl(AbstractLedgerTxn& ltx,
                                                LedgerEntry& current);
};

class ConstLedgerTxnEntry
{
  public:
    class Impl;

  private:
    std::weak_ptr<Impl> mImpl;

    std::shared_ptr<Impl> getImpl();
    std::shared_ptr<Impl const> getImpl() const;

  public:
    // ConstLedgerTxnEntry constructors do not throw
    ConstLedgerTxnEntry();
    explicit ConstLedgerTxnEntry(std::shared_ptr<Impl> const& impl);

    ~ConstLedgerTxnEntry();

    // Copy construction and copy assignment are forbidden.
    ConstLedgerTxnEntry(ConstLedgerTxnEntry const&) = delete;
    ConstLedgerTxnEntry& operator=(ConstLedgerTxnEntry const&) = delete;

    // Move construction and move assignment are permitted.
    ConstLedgerTxnEntry(ConstLedgerTxnEntry&& other);
    ConstLedgerTxnEntry& operator=(ConstLedgerTxnEntry&& other);

    explicit operator bool() const;

    LedgerEntry const& current() const;

    void deactivate();

    void swap(ConstLedgerTxnEntry& other);

    static std::shared_ptr<Impl> makeSharedImpl(AbstractLedgerTxn& ltx,
                                                LedgerEntry const& current);
};

std::shared_ptr<EntryImplBase>
toEntryImplBase(std::shared_ptr<LedgerTxnEntry::Impl> const& impl);

std::shared_ptr<EntryImplBase>
toEntryImplBase(std::shared_ptr<ConstLedgerTxnEntry::Impl> const& impl);
}
