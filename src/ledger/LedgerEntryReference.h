#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>

namespace stellar
{

struct LedgerEntry;
struct LedgerKey;

class LedgerState;

class LedgerEntryReference
{
    friend class LedgerState;

    class IgnoreInvalid
    {
        std::shared_ptr<LedgerEntry> const& mEntry;
        std::shared_ptr<LedgerEntry const> const& mPreviousEntry;

      public:
        explicit IgnoreInvalid(LedgerEntryReference& ler);

        std::shared_ptr<LedgerEntry> const& entry();
        std::shared_ptr<LedgerEntry const> const& previousEntry();
    };

    bool mValid;
    std::shared_ptr<LedgerEntry> mEntry;
    std::shared_ptr<LedgerEntry const> const mPreviousEntry;

  public:
    LedgerEntryReference(LedgerEntryReference const&) = delete;
    LedgerEntryReference& operator=(LedgerEntryReference const&) = delete;

    LedgerEntryReference(LedgerEntryReference&&) = delete;
    LedgerEntryReference& operator=(LedgerEntryReference&&) = delete;

    std::shared_ptr<LedgerEntry> const& entry();
    std::shared_ptr<LedgerEntry const> const& previousEntry();

    void erase();

    bool valid();

    void invalidate();

  private:
    explicit LedgerEntryReference(
        std::shared_ptr<LedgerEntry const> const& entry,
        std::shared_ptr<LedgerEntry const> const& previous);

    IgnoreInvalid ignoreInvalid();
};
}
