#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger.h"

namespace stellar
{

class LedgerState;

class LedgerHeaderReference
{
    friend class LedgerState;

    class IgnoreInvalid
    {
        LedgerHeader& mHeader;
        LedgerHeader const& mPreviousHeader;

      public:
        explicit IgnoreInvalid(LedgerHeaderReference& lhr);

        LedgerHeader& header();
        LedgerHeader const& previousHeader();
    };

    bool mValid;
    LedgerHeader mHeader;
    LedgerHeader const mPreviousHeader;

  public:
    LedgerHeaderReference(LedgerHeaderReference const&) = delete;
    LedgerHeaderReference& operator=(LedgerHeaderReference const&) = delete;

    LedgerHeaderReference(LedgerHeaderReference&&) = delete;
    LedgerHeaderReference& operator=(LedgerHeaderReference&&) = delete;

    LedgerHeader& header();
    LedgerHeader const& previousHeader();

    bool valid();

    void invalidate();

  private:
    explicit LedgerHeaderReference(LedgerHeader const& header,
                                   LedgerHeader const& previous);

    IgnoreInvalid ignoreInvalid();
};

std::string ledgerAbbrev(LedgerHeader const& header);
}
