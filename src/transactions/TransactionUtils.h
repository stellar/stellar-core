#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{

class ConstLedgerStateEntry;
class ConstTrustLineWrapper;
class AbstractLedgerState;
class LedgerStateEntry;
class LedgerStateHeader;
class TrustLineWrapper;

LedgerStateEntry loadAccount(AbstractLedgerState& ls,
                             AccountID const& accountID);

ConstLedgerStateEntry loadAccountWithoutRecord(AbstractLedgerState& ls,
                                               AccountID const& accountID);

LedgerStateEntry loadData(AbstractLedgerState& ls, AccountID const& accountID,
                          std::string const& dataName);

LedgerStateEntry loadOffer(AbstractLedgerState& ls, AccountID const& sellerID,
                           uint64_t offerID);

TrustLineWrapper loadTrustLine(AbstractLedgerState& ls,
                               AccountID const& accountID, Asset const& asset);

ConstTrustLineWrapper loadTrustLineWithoutRecord(AbstractLedgerState& ls,
                                                 AccountID const& accountID,
                                                 Asset const& asset);

TrustLineWrapper loadTrustLineIfNotNative(AbstractLedgerState& ls,
                                          AccountID const& accountID,
                                          Asset const& asset);

ConstTrustLineWrapper loadTrustLineWithoutRecordIfNotNative(
    AbstractLedgerState& ls, AccountID const& accountID, Asset const& asset);

void acquireLiabilities(AbstractLedgerState& ls,
                        LedgerStateHeader const& header,
                        LedgerStateEntry const& offer);

bool addBalance(LedgerStateHeader const& header, LedgerStateEntry& entry,
                int64_t delta);

bool addBuyingLiabilities(LedgerStateHeader const& header,
                          LedgerStateEntry& entry, int64_t delta);

bool addNumEntries(LedgerStateHeader const& header, LedgerStateEntry& entry,
                   int count);

bool addSellingLiabilities(LedgerStateHeader const& header,
                           LedgerStateEntry& entry, int64_t delta);

uint64_t generateID(LedgerStateHeader& header);

int64_t getAvailableBalance(LedgerStateHeader const& header,
                            LedgerEntry const& le);
int64_t getAvailableBalance(LedgerStateHeader const& header,
                            LedgerStateEntry const& entry);
int64_t getAvailableBalance(LedgerStateHeader const& header,
                            ConstLedgerStateEntry const& entry);

int64_t getBuyingLiabilities(LedgerStateHeader const& header,
                             LedgerEntry const& le);
int64_t getBuyingLiabilities(LedgerStateHeader const& header,
                             LedgerStateEntry const& offer);

int64_t getMaxAmountReceive(LedgerStateHeader const& header,
                            LedgerEntry const& le);
int64_t getMaxAmountReceive(LedgerStateHeader const& header,
                            LedgerStateEntry const& entry);
int64_t getMaxAmountReceive(LedgerStateHeader const& header,
                            ConstLedgerStateEntry const& entry);

int64_t getMinBalance(LedgerStateHeader const& header, uint32_t ownerCount);

int64_t getMinimumLimit(LedgerStateHeader const& header, LedgerEntry const& le);
int64_t getMinimumLimit(LedgerStateHeader const& header,
                        LedgerStateEntry const& entry);
int64_t getMinimumLimit(LedgerStateHeader const& header,
                        ConstLedgerStateEntry const& entry);

int64_t getOfferBuyingLiabilities(LedgerStateHeader const& header,
                                  LedgerEntry const& entry);
int64_t getOfferBuyingLiabilities(LedgerStateHeader const& header,
                                  LedgerStateEntry const& entry);

int64_t getOfferSellingLiabilities(LedgerStateHeader const& header,
                                   LedgerEntry const& entry);
int64_t getOfferSellingLiabilities(LedgerStateHeader const& header,
                                   LedgerStateEntry const& entry);

int64_t getSellingLiabilities(LedgerStateHeader const& header,
                              LedgerEntry const& le);
int64_t getSellingLiabilities(LedgerStateHeader const& header,
                              LedgerStateEntry const& offer);

uint64_t getStartingSequenceNumber(LedgerStateHeader const& header);

bool isAuthorized(LedgerEntry const& le);
bool isAuthorized(LedgerStateEntry const& entry);
bool isAuthorized(ConstLedgerStateEntry const& entry);

bool isAuthRequired(ConstLedgerStateEntry const& entry);

bool isImmutableAuth(LedgerStateEntry const& entry);

void normalizeSigners(LedgerStateEntry& entry);

void releaseLiabilities(AbstractLedgerState& ls,
                        LedgerStateHeader const& header,
                        LedgerStateEntry const& offer);

void setAuthorized(LedgerStateEntry& entry, bool authorized);
}
