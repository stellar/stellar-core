#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger-entries.h"
#include <memory>

namespace stellar
{

class AbstractLedgerState;

class EntryImplBase
{
  public:
    virtual ~EntryImplBase()
    {
    }
};

class LedgerStateEntry
{
  public:
    class Impl;

  private:
    std::weak_ptr<Impl> mImpl;

    std::shared_ptr<Impl> getImpl();
    std::shared_ptr<Impl const> getImpl() const;

  public:
    // LedgerStateEntry constructors do not throw
    LedgerStateEntry();
    explicit LedgerStateEntry(std::shared_ptr<Impl> const& impl);

    ~LedgerStateEntry();

    // Copy construction and copy assignment are forbidden.
    LedgerStateEntry(LedgerStateEntry const&) = delete;
    LedgerStateEntry& operator=(LedgerStateEntry const&) = delete;

    // Move construction and move assignment are permitted.
    LedgerStateEntry(LedgerStateEntry&& other);
    LedgerStateEntry& operator=(LedgerStateEntry&& other);

    explicit operator bool() const;

    LedgerEntry& current();
    LedgerEntry const& current() const;

    void deactivate();

    void erase();

    void swap(LedgerStateEntry& other);

    static std::shared_ptr<Impl> makeSharedImpl(AbstractLedgerState& ls,
                                                LedgerEntry& current);
};

class ConstLedgerStateEntry
{
  public:
    class Impl;

  private:
    std::weak_ptr<Impl> mImpl;

    std::shared_ptr<Impl> getImpl();
    std::shared_ptr<Impl const> getImpl() const;

  public:
    // ConstLedgerStateEntry constructors do not throw
    ConstLedgerStateEntry();
    explicit ConstLedgerStateEntry(std::shared_ptr<Impl> const& impl);

    ~ConstLedgerStateEntry();

    // Copy construction and copy assignment are forbidden.
    ConstLedgerStateEntry(ConstLedgerStateEntry const&) = delete;
    ConstLedgerStateEntry& operator=(ConstLedgerStateEntry const&) = delete;

    // Move construction and move assignment are permitted.
    ConstLedgerStateEntry(ConstLedgerStateEntry&& other);
    ConstLedgerStateEntry& operator=(ConstLedgerStateEntry&& other);

    explicit operator bool() const;

    LedgerEntry const& current() const;

    void deactivate();

    void swap(ConstLedgerStateEntry& other);

    static std::shared_ptr<Impl> makeSharedImpl(AbstractLedgerState& ls,
                                                LedgerEntry const& current);
};

std::shared_ptr<EntryImplBase>
toEntryImplBase(std::shared_ptr<LedgerStateEntry::Impl> const& impl);

std::shared_ptr<EntryImplBase>
toEntryImplBase(std::shared_ptr<ConstLedgerStateEntry::Impl> const& impl);
}
