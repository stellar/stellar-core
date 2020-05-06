#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{

class ConstLedgerTxnEntry;
class ConstTrustLineWrapper;
class AbstractLedgerTxn;
class LedgerTxnEntry;
class LedgerTxnHeader;
class TrustLineWrapper;
struct LedgerKey;
struct TransactionEnvelope;
struct MuxedAccount;

LedgerKey accountKey(AccountID const& accountID);
LedgerKey trustlineKey(AccountID const& accountID, Asset const& asset);
LedgerKey offerKey(AccountID const& sellerID, uint64_t offerID);
LedgerKey dataKey(AccountID const& accountID, std::string const& dataName);

uint32_t const FIRST_PROTOCOL_SUPPORTING_OPERATION_LIMITS = 11;
uint32_t const ACCOUNT_SUBENTRY_LIMIT = 1000;

LedgerTxnEntry loadAccount(AbstractLedgerTxn& ltx, AccountID const& accountID);

ConstLedgerTxnEntry loadAccountWithoutRecord(AbstractLedgerTxn& ltx,
                                             AccountID const& accountID);

LedgerTxnEntry loadData(AbstractLedgerTxn& ltx, AccountID const& accountID,
                        std::string const& dataName);

LedgerTxnEntry loadOffer(AbstractLedgerTxn& ltx, AccountID const& sellerID,
                         int64_t offerID);

TrustLineWrapper loadTrustLine(AbstractLedgerTxn& ltx,
                               AccountID const& accountID, Asset const& asset);

ConstTrustLineWrapper loadTrustLineWithoutRecord(AbstractLedgerTxn& ltx,
                                                 AccountID const& accountID,
                                                 Asset const& asset);

TrustLineWrapper loadTrustLineIfNotNative(AbstractLedgerTxn& ltx,
                                          AccountID const& accountID,
                                          Asset const& asset);

ConstTrustLineWrapper loadTrustLineWithoutRecordIfNotNative(
    AbstractLedgerTxn& ltx, AccountID const& accountID, Asset const& asset);

void acquireLiabilities(AbstractLedgerTxn& ltx, LedgerTxnHeader const& header,
                        LedgerTxnEntry const& offer);

bool addBalance(LedgerTxnHeader const& header, LedgerTxnEntry& entry,
                int64_t delta);

bool addBuyingLiabilities(LedgerTxnHeader const& header, LedgerTxnEntry& entry,
                          int64_t delta);

enum class AddSubentryResult
{
    SUCCESS,
    LOW_RESERVE,
    TOO_MANY_SUBENTRIES
};
AddSubentryResult addNumEntries(LedgerTxnHeader const& header,
                                LedgerTxnEntry& entry, int count);

bool addSellingLiabilities(LedgerTxnHeader const& header, LedgerTxnEntry& entry,
                           int64_t delta);

uint64_t generateID(LedgerTxnHeader& header);

int64_t getAvailableBalance(LedgerTxnHeader const& header,
                            LedgerEntry const& le);
int64_t getAvailableBalance(LedgerTxnHeader const& header,
                            LedgerTxnEntry const& entry);
int64_t getAvailableBalance(LedgerTxnHeader const& header,
                            ConstLedgerTxnEntry const& entry);

int64_t getBuyingLiabilities(LedgerTxnHeader const& header,
                             LedgerEntry const& le);
int64_t getBuyingLiabilities(LedgerTxnHeader const& header,
                             LedgerTxnEntry const& offer);

int64_t getMaxAmountReceive(LedgerTxnHeader const& header,
                            LedgerEntry const& le);
int64_t getMaxAmountReceive(LedgerTxnHeader const& header,
                            LedgerTxnEntry const& entry);
int64_t getMaxAmountReceive(LedgerTxnHeader const& header,
                            ConstLedgerTxnEntry const& entry);

int64_t getMinBalance(LedgerTxnHeader const& header, uint32_t ownerCount);

int64_t getMinimumLimit(LedgerTxnHeader const& header, LedgerEntry const& le);
int64_t getMinimumLimit(LedgerTxnHeader const& header,
                        LedgerTxnEntry const& entry);
int64_t getMinimumLimit(LedgerTxnHeader const& header,
                        ConstLedgerTxnEntry const& entry);

int64_t getOfferBuyingLiabilities(LedgerTxnHeader const& header,
                                  LedgerEntry const& entry);
int64_t getOfferBuyingLiabilities(LedgerTxnHeader const& header,
                                  LedgerTxnEntry const& entry);

int64_t getOfferSellingLiabilities(LedgerTxnHeader const& header,
                                   LedgerEntry const& entry);
int64_t getOfferSellingLiabilities(LedgerTxnHeader const& header,
                                   LedgerTxnEntry const& entry);

int64_t getSellingLiabilities(LedgerTxnHeader const& header,
                              LedgerEntry const& le);
int64_t getSellingLiabilities(LedgerTxnHeader const& header,
                              LedgerTxnEntry const& offer);

uint64_t getStartingSequenceNumber(uint32_t ledgerSeq);
uint64_t getStartingSequenceNumber(LedgerTxnHeader const& header);

bool isAuthorized(LedgerEntry const& le);
bool isAuthorized(LedgerTxnEntry const& entry);
bool isAuthorized(ConstLedgerTxnEntry const& entry);

bool isAuthorizedToMaintainLiabilities(LedgerEntry const& le);
bool isAuthorizedToMaintainLiabilities(LedgerTxnEntry const& entry);
bool isAuthorizedToMaintainLiabilities(ConstLedgerTxnEntry const& entry);

bool isAuthRequired(ConstLedgerTxnEntry const& entry);

bool isImmutableAuth(LedgerTxnEntry const& entry);

void normalizeSigners(LedgerTxnEntry& entry);
void normalizeSigners(AccountEntry& acc);

void releaseLiabilities(AbstractLedgerTxn& ltx, LedgerTxnHeader const& header,
                        LedgerTxnEntry const& offer);

AccountID toAccountID(MuxedAccount const& m);
MuxedAccount toMuxedAccount(AccountID const& a);

void setAuthorized(LedgerTxnHeader const& header, LedgerTxnEntry& entry,
                   uint32_t authorized);

bool trustLineFlagIsValid(uint32_t flag, uint32_t ledgerVersion);
bool trustLineFlagIsValid(uint32_t flag, LedgerTxnHeader const& header);

bool hasMuxedAccount(TransactionEnvelope const& e);
}
