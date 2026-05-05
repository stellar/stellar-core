// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// RustVecXdrMarshal.h must come before any include that pulls in xdrpp's
// marshal.h: it declares an overload of `xdr::detail::bytes_to_void` that
// teaches xdrpp how to ingest `rust::Vec<uint8_t>` byte buffers, and the
// overload has to be visible before the relevant template is instantiated.
#include "rust/RustVecXdrMarshal.h"

#include "ledger/InMemorySorobanState.h"
#include "ledger/NetworkConfig.h"
#include "main/Config.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{

InMemorySorobanState::InMemorySorobanState()
    : mState(rust_bridge::new_soroban_state())
{
}

bool
InMemorySorobanState::isInMemoryType(LedgerKey const& ledgerKey)
{
    return ledgerKey.type() == CONTRACT_DATA ||
           ledgerKey.type() == CONTRACT_CODE || ledgerKey.type() == TTL;
}

bool
InMemorySorobanState::hasTTL(LedgerKey const& ledgerKey) const
{
    auto keyBuf = toCxxBuf(ledgerKey);
    return mState->has_ttl_xdr(keyBuf);
}

bool
InMemorySorobanState::isEmpty() const
{
    return mState->is_empty();
}

uint32_t
InMemorySorobanState::getLedgerSeq() const
{
    return mState->ledger_seq();
}

void
InMemorySorobanState::assertLastClosedLedger(uint32_t expectedLedgerSeq) const
{
    mState->assert_last_closed_ledger(expectedLedgerSeq);
}

uint64_t
InMemorySorobanState::getSize() const
{
    return mState->size();
}

std::shared_ptr<LedgerEntry const>
InMemorySorobanState::get(LedgerKey const& ledgerKey) const
{
    auto keyBuf = toCxxBuf(ledgerKey);
    auto resultBuf = mState->lookup_entry_xdr(keyBuf);
    if (resultBuf.data.empty())
    {
        return nullptr;
    }
    auto entry = std::make_shared<LedgerEntry>();
    xdr::xdr_from_opaque(resultBuf.data, *entry);
    return entry;
}

size_t
InMemorySorobanState::getContractDataEntryCount() const
{
    return mState->contract_data_entry_count();
}

size_t
InMemorySorobanState::getContractCodeEntryCount() const
{
    return mState->contract_code_entry_count();
}

void
InMemorySorobanState::initializeFromBucketFiles(
    std::vector<std::string> const& bucketPaths, uint32_t lastClosedLedgerSeq,
    uint32_t ledgerVersion,
    std::optional<SorobanNetworkConfig const> const& sorobanConfig)
{
    rust::Vec<rust::String> rustPaths;
    rustPaths.reserve(bucketPaths.size());
    for (auto const& p : bucketPaths)
    {
        rustPaths.push_back(rust::String(p));
    }

    // Pre-Soroban: pass empty cost params; Rust will skip the bucket scan.
    CxxBuf cpuBuf{std::make_unique<std::vector<uint8_t>>()};
    CxxBuf memBuf{std::make_unique<std::vector<uint8_t>>()};
    if (sorobanConfig)
    {
        cpuBuf = toCxxBuf(sorobanConfig->cpuCostParams());
        memBuf = toCxxBuf(sorobanConfig->memCostParams());
    }

    mState->initialize_from_bucket_files(
        rustPaths, lastClosedLedgerSeq, ledgerVersion,
        Config::CURRENT_LEDGER_PROTOCOL_VERSION, cpuBuf, memBuf);
}

void
InMemorySorobanState::manuallyAdvanceLedgerHeader(LedgerHeader const& lh)
{
    mState->manually_advance_ledger_header(lh.ledgerSeq);
}

void
InMemorySorobanState::evictEntries(
    std::vector<LedgerEntry> const& archivedEntries,
    std::vector<LedgerKey> const& deletedKeys)
{
    rust::Vec<CxxBuf> archivedKeyBufs;
    archivedKeyBufs.reserve(archivedEntries.size());
    for (auto const& entry : archivedEntries)
    {
        archivedKeyBufs.push_back(toCxxBuf(LedgerEntryKey(entry)));
    }
    rust::Vec<CxxBuf> deletedKeyBufs;
    deletedKeyBufs.reserve(deletedKeys.size());
    for (auto const& key : deletedKeys)
    {
        deletedKeyBufs.push_back(toCxxBuf(key));
    }
    mState->evict_entries_xdr(archivedKeyBufs, deletedKeyBufs);
}

void
InMemorySorobanState::recomputeContractCodeSize(
    SorobanNetworkConfig const& sorobanConfig, uint32_t ledgerVersion)
{
    auto cpu = toCxxBuf(sorobanConfig.cpuCostParams());
    auto mem = toCxxBuf(sorobanConfig.memCostParams());
    mState->recompute_contract_code_size_xdr(
        Config::CURRENT_LEDGER_PROTOCOL_VERSION, ledgerVersion, cpu, mem);
}

rust::Box<rust_bridge::SorobanState>&
InMemorySorobanState::getRustStateForBridge()
{
    return mState;
}

#ifdef BUILD_TESTS
void
InMemorySorobanState::clearForTesting()
{
    mState->clear();
}
#endif
}
