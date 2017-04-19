#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/optional.h"
#include "xdr/Stellar-ledger-entries.h"

#include <unordered_map>
#include <vector>

namespace stellar
{

class Database;
class StatementContext;
struct LedgerKey;

void createOffersTable(Database& db);
std::vector<LedgerEntry> selectBestOffers(size_t numOffers, size_t offset,
                                          Asset const& selling,
                                          Asset const& buying, Database& db);
optional<LedgerEntry const> selectOffer(AccountID const& sellerID,
                                        uint64_t offerID, Database& db);
void insertOffer(LedgerEntry const& entry, Database& db);
void updateOffer(LedgerEntry const& entry, Database& db);
bool offerExists(LedgerKey const& key, Database& db);
void deleteOffer(LedgerKey const& key, Database& db);

std::unordered_map<AccountID, int> selectOfferCountPerAccount(Database& db);
uint64_t countOffers(Database& db);
}
