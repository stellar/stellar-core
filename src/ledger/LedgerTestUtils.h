#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"

namespace stellar
{
namespace LedgerTestUtils
{

// note: entries generated are valid in the sense that they are sane by
// themselves
// it does NOT mean that it makes sense relative to other entries:
// for example the numsubentries of a related account is not updated when
// generating a 'valid' trust line

template <typename T> void replaceControlCharacters(T& s, int minSize);

void makeValid(AccountEntry& a);
void makeValid(TrustLineEntry& tl);
void makeValid(OfferEntry& o);
void makeValid(DataEntry& d);

LedgerEntry generateValidLedgerEntry(size_t b = 3);
std::vector<LedgerEntry> generateValidLedgerEntries(size_t n);

AccountEntry generateValidAccountEntry(size_t b = 3);
std::vector<AccountEntry> generateValidAccountEntries(size_t n);

TrustLineEntry generateValidTrustLineEntry(size_t b = 3);
std::vector<TrustLineEntry> generateValidTrustLineEntries(size_t n);

OfferEntry generateValidOfferEntry(size_t b = 3);
std::vector<OfferEntry> generateValidOfferEntries(size_t n);

DataEntry generateValidDataEntry(size_t b = 3);
std::vector<DataEntry> generateValidDataEntries(size_t n);
}
}
