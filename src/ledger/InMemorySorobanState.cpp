// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/InMemorySorobanState.h"
#include "bucket/SearchableBucketList.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/SorobanMetrics.h"
#include "util/GlobalChecks.h"
#include <cstdint>

namespace stellar
{

namespace
{
uint32_t
contractCodeSizeForRent(LedgerEntry const& ledgerEntry,
                        SorobanNetworkConfig const& sorobanConfig,
                        uint32_t ledgerVersion)
{
    releaseAssertOrThrow(ledgerEntry.data.type() == CONTRACT_CODE);
    // Subtle: in-memory state size accounting is only used starting from
    // protocol 23, but the cache itself may be populated in an earlier
    // protocol. In order to have the correct size immediately on the update
    // to protocol 23 we compute the size using the Soroban host for at least
    // protocol 23.
    uint32_t ledgerVersionForSize =
        std::max(ledgerVersion, static_cast<uint32_t>(ProtocolVersion::V_23));
    return ledgerEntrySizeForRent(ledgerEntry, xdr::xdr_size(ledgerEntry),
                                  ledgerVersionForSize, sorobanConfig);
}
} // namespace

bool
TTLData::isDefault() const
{
    if (liveUntilLedgerSeq == 0)
    {
        releaseAssert(lastModifiedLedgerSeq == 0);
        return true;
    }
    else
    {
        releaseAssert(lastModifiedLedgerSeq != 0);
        return false;
    }
}

void
InMemorySorobanState::updateContractDataTTL(
    std::unordered_set<InternalContractDataMapEntry,
                       InternalContractDataEntryHash>::iterator dataIt,
    TTLData newTtlData)
{
    // Since entries are immutable, we must erase and re-insert
    auto ledgerEntryPtr = dataIt->get().ledgerEntry;
    mContractDataEntries.erase(dataIt);
    mContractDataEntries.emplace(
        InternalContractDataMapEntry(std::move(ledgerEntryPtr), newTtlData));
}

void
InMemorySorobanState::updateTTL(LedgerEntry const& ttlEntry)
{
    releaseAssertOrThrow(ttlEntry.data.type() == TTL);

    auto lk = LedgerEntryKey(ttlEntry);
    auto newTtlData = TTLData(ttlEntry.data.ttl().liveUntilLedgerSeq,
                              ttlEntry.lastModifiedLedgerSeq);

    // TTL updates can apply to either ContractData or ContractCode entries.
    // First check if this TTL belongs to a stored ContractData entry.
    auto dataIt = mContractDataEntries.find(InternalContractDataMapEntry(lk));
    if (dataIt != mContractDataEntries.end())
    {
        updateContractDataTTL(dataIt, newTtlData);
    }
    else
    {
        // Since we're updating a TTL that exists, if we get here it must belong
        // to a contract code entry.
        auto codeIt = mContractCodeEntries.find(lk.ttl().keyHash);
        releaseAssertOrThrow(codeIt != mContractCodeEntries.end());
        codeIt->second.ttlData = newTtlData;
    }
}

void
InMemorySorobanState::updateContractData(LedgerEntry const& ledgerEntry)
{
    releaseAssertOrThrow(ledgerEntry.data.type() == CONTRACT_DATA);

    // Entry must already exist since this is an update
    auto lk = LedgerEntryKey(ledgerEntry);
    auto dataIt = mContractDataEntries.find(InternalContractDataMapEntry(lk));
    releaseAssertOrThrow(dataIt != mContractDataEntries.end());
    releaseAssertOrThrow(dataIt->get().ledgerEntry != nullptr);

    uint32_t oldSize = xdr::xdr_size(*dataIt->get().ledgerEntry);
    uint32_t newSize = xdr::xdr_size(ledgerEntry);
    updateStateSizeOnEntryUpdate(oldSize, newSize, /*isContractCode=*/false);

    // Preserve the existing TTL while updating the data
    auto preservedTTL = dataIt->get().ttlData;
    mContractDataEntries.erase(dataIt);
    mContractDataEntries.emplace(
        InternalContractDataMapEntry(ledgerEntry, preservedTTL));
}

void
InMemorySorobanState::createContractDataEntry(LedgerEntry const& ledgerEntry)
{
    releaseAssertOrThrow(ledgerEntry.data.type() == CONTRACT_DATA);

    // Verify entry doesn't already exist
    auto dataIt = mContractDataEntries.find(
        InternalContractDataMapEntry(LedgerEntryKey(ledgerEntry)));
    releaseAssertOrThrow(dataIt == mContractDataEntries.end());

    // Check if we've already seen this entry's TTL (can happen during
    // initialization when TTL is written before the data)
    auto ttlKey = getTTLKey(LedgerEntryKey(ledgerEntry));
    auto ttlData = TTLData();

    auto ttlIt = mPendingTTLs.find(ttlKey);
    if (ttlIt != mPendingTTLs.end())
    {
        // Found orphaned TTL - adopt it and remove from temporary storage
        ttlData = TTLData(ttlIt->second.data.ttl().liveUntilLedgerSeq,
                          ttlIt->second.lastModifiedLedgerSeq);
        mPendingTTLs.erase(ttlIt);
    }
    // else: TTL hasn't arrived yet, initialize to 0 (will be updated later)

    updateStateSizeOnEntryUpdate(0, xdr::xdr_size(ledgerEntry),
                                 /*isContractCode=*/false);
    mContractDataEntries.emplace(
        InternalContractDataMapEntry(ledgerEntry, ttlData));
}

bool
InMemorySorobanState::isInMemoryType(LedgerKey const& ledgerKey)
{
    return ledgerKey.type() == CONTRACT_DATA ||
           ledgerKey.type() == CONTRACT_CODE || ledgerKey.type() == TTL;
}

void
InMemorySorobanState::createTTL(LedgerEntry const& ttlEntry)
{
    releaseAssertOrThrow(ttlEntry.data.type() == TTL);

    auto lk = LedgerEntryKey(ttlEntry);
    auto newTtlData = TTLData(ttlEntry.data.ttl().liveUntilLedgerSeq,
                              ttlEntry.lastModifiedLedgerSeq);

    // Check if the corresponding ContractData entry already exists
    // (can happen during initialization when entries arrive out of order)
    auto dataIt = mContractDataEntries.find(InternalContractDataMapEntry(lk));
    if (dataIt != mContractDataEntries.end())
    {
        // ContractData exists but has no TTL yet - update it
        // Verify TTL hasn't been set yet (should be default initialized)
        releaseAssertOrThrow(dataIt->get().ttlData.isDefault());
        updateContractDataTTL(dataIt, newTtlData);
    }
    else
    {
        // Check if this TTL belongs to a ContractCode entry that hasn't arrived
        // yet
        auto codeIt = mContractCodeEntries.find(lk.ttl().keyHash);
        if (codeIt != mContractCodeEntries.end())
        {
            // ContractCode exists but has no TTL yet - update it
            // Verify TTL hasn't been set yet (should be default initialized)
            releaseAssertOrThrow(codeIt->second.ttlData.isDefault());
            codeIt->second.ttlData = newTtlData;
        }
        else
        {
            // No ContractData or ContractCode yet - store TTL for later
            auto [_, inserted] = mPendingTTLs.emplace(lk, ttlEntry);
            releaseAssertOrThrow(inserted);
        }
    }
}

void
InMemorySorobanState::deleteContractData(LedgerKey const& ledgerKey)
{
    releaseAssertOrThrow(ledgerKey.type() == CONTRACT_DATA);
    auto it =
        mContractDataEntries.find(InternalContractDataMapEntry(ledgerKey));
    releaseAssertOrThrow(it != mContractDataEntries.end());
    releaseAssertOrThrow(it->get().ledgerEntry != nullptr);
    updateStateSizeOnEntryUpdate(xdr::xdr_size(*it->get().ledgerEntry), 0,
                                 /*isContractCode=*/false);
    mContractDataEntries.erase(it);
}

std::shared_ptr<LedgerEntry const>
InMemorySorobanState::get(LedgerKey const& ledgerKey) const
{
    switch (ledgerKey.type())
    {
    case CONTRACT_DATA:
    {
        auto it =
            mContractDataEntries.find(InternalContractDataMapEntry(ledgerKey));
        if (it == mContractDataEntries.end())
        {
            return nullptr;
        }
        return it->get().ledgerEntry;
    }
    case CONTRACT_CODE:
    {
        auto ttlKey = getTTLKey(ledgerKey);
        auto keyHash = ttlKey.ttl().keyHash;
        auto it = mContractCodeEntries.find(keyHash);
        if (it == mContractCodeEntries.end())
        {
            return nullptr;
        }

        return it->second.ledgerEntry;
    }
    case TTL:
        return getTTL(ledgerKey);
    default:
        throw std::runtime_error("InMemorySorobanState::get: invalid key type");
    }
}

void
InMemorySorobanState::createContractCodeEntry(
    LedgerEntry const& ledgerEntry, SorobanNetworkConfig const& sorobanConfig,
    uint32_t ledgerVersion)
{
    releaseAssertOrThrow(ledgerEntry.data.type() == CONTRACT_CODE);

    // Get the TTL key hash
    auto ttlKey = getTTLKey(LedgerEntryKey(ledgerEntry));
    auto keyHash = ttlKey.ttl().keyHash;

    // Verify entry doesn't already exist
    auto codeIt = mContractCodeEntries.find(keyHash);
    releaseAssertOrThrow(codeIt == mContractCodeEntries.end());

    // Check if we've already seen this entry's TTL (can happen during
    // initialization when TTL is written before the code)
    auto ttlData = TTLData();

    auto ttlIt = mPendingTTLs.find(ttlKey);
    if (ttlIt != mPendingTTLs.end())
    {
        // Found orphaned TTL - adopt it and remove from temporary storage
        ttlData = TTLData(ttlIt->second.data.ttl().liveUntilLedgerSeq,
                          ttlIt->second.lastModifiedLedgerSeq);
        mPendingTTLs.erase(ttlIt);
    }
    // else: TTL hasn't arrived yet, initialize to 0 (will be updated later)

    uint32_t entrySize =
        contractCodeSizeForRent(ledgerEntry, sorobanConfig, ledgerVersion);
    updateStateSizeOnEntryUpdate(0, entrySize, /*isContractCode=*/true);

    mContractCodeEntries.emplace(
        keyHash,
        ContractCodeMapEntryT(std::make_shared<LedgerEntry const>(ledgerEntry),
                              ttlData, entrySize));
}

void
InMemorySorobanState::updateContractCode(
    LedgerEntry const& ledgerEntry, SorobanNetworkConfig const& sorobanConfig,
    uint32_t ledgerVersion)
{
    releaseAssertOrThrow(ledgerEntry.data.type() == CONTRACT_CODE);
    auto ttlKey = getTTLKey(LedgerEntryKey(ledgerEntry));
    auto keyHash = ttlKey.ttl().keyHash;

    // Entry must already exist since this is an update
    auto codeIt = mContractCodeEntries.find(keyHash);
    releaseAssertOrThrow(codeIt != mContractCodeEntries.end());

    uint32_t newEntrySize =
        contractCodeSizeForRent(ledgerEntry, sorobanConfig, ledgerVersion);

    updateStateSizeOnEntryUpdate(codeIt->second.sizeBytes, newEntrySize,
                                 /*isContractCode=*/true);

    // Preserve the existing TTL while updating the code
    auto ttlData = codeIt->second.ttlData;
    releaseAssertOrThrow(!ttlData.isDefault());
    codeIt->second =
        ContractCodeMapEntryT(std::make_shared<LedgerEntry const>(ledgerEntry),
                              ttlData, newEntrySize);
}

void
InMemorySorobanState::deleteContractCode(LedgerKey const& ledgerKey)
{
    releaseAssertOrThrow(ledgerKey.type() == CONTRACT_CODE);

    auto ttlKey = getTTLKey(ledgerKey);
    auto keyHash = ttlKey.ttl().keyHash;
    auto it = mContractCodeEntries.find(keyHash);
    releaseAssertOrThrow(it != mContractCodeEntries.end());
    updateStateSizeOnEntryUpdate(it->second.sizeBytes, 0,
                                 /*isContractCode=*/true);
    mContractCodeEntries.erase(it);
}

bool
InMemorySorobanState::hasTTL(LedgerKey const& ledgerKey) const
{
    releaseAssertOrThrow(ledgerKey.type() == TTL);

    // Check if this is a pending TTL
    if (mPendingTTLs.find(ledgerKey) != mPendingTTLs.end())
    {
        return true;
    }

    // Check if this is a ContractData TTL (stored with the data)
    auto dataIt =
        mContractDataEntries.find(InternalContractDataMapEntry(ledgerKey));
    if (dataIt != mContractDataEntries.end())
    {
        // Only return true if TTL has been set (non-zero)
        // During initialization, entries may exist with default constructed
        // TTLs
        return !dataIt->get().ttlData.isDefault();
    }

    // Check if this is a ContractCode TTL (stored with the code)
    auto codeIt = mContractCodeEntries.find(ledgerKey.ttl().keyHash);
    if (codeIt != mContractCodeEntries.end())
    {
        // Only return true if TTL has been set (non-zero)
        // During initialization, entries may exist with default constructed
        // TTLs
        return !codeIt->second.ttlData.isDefault();
    }

    return false;
}

bool
InMemorySorobanState::isEmpty() const
{
    return mContractDataEntries.empty() && mContractCodeEntries.empty() &&
           mPendingTTLs.empty();
}

uint32_t
InMemorySorobanState::getLedgerSeq() const
{
    return mLastClosedLedgerSeq;
}

std::shared_ptr<LedgerEntry const>
InMemorySorobanState::getTTL(LedgerKey const& ledgerKey) const
{
    releaseAssertOrThrow(ledgerKey.type() == TTL);

    // This should never be called when we are mid-update
    releaseAssertOrThrow(mPendingTTLs.empty());

    auto constructTTLEntry = [&ledgerKey](TTLData const& ttlData) {
        releaseAssertOrThrow(!ttlData.isDefault());
        auto ttlEntry = std::make_shared<LedgerEntry>();
        ttlEntry->data.type(TTL);
        ttlEntry->data.ttl().keyHash = ledgerKey.ttl().keyHash;
        ttlEntry->data.ttl().liveUntilLedgerSeq = ttlData.liveUntilLedgerSeq;
        ttlEntry->lastModifiedLedgerSeq = ttlData.lastModifiedLedgerSeq;
        return ttlEntry;
    };

    // Since the TTL key is the hash of the associated LedgerKey, we don't know
    // which map it could belong in, so check both.
    auto dataIt =
        mContractDataEntries.find(InternalContractDataMapEntry(ledgerKey));
    if (dataIt != mContractDataEntries.end())
    {
        return constructTTLEntry(dataIt->get().ttlData);
    }

    auto codeIt = mContractCodeEntries.find(ledgerKey.ttl().keyHash);
    if (codeIt != mContractCodeEntries.end())
    {
        return constructTTLEntry(codeIt->second.ttlData);
    }

    return nullptr;
}

void
InMemorySorobanState::initializeStateFromSnapshot(
    SearchableSnapshotConstPtr snap, SorobanNetworkConfig const* sorobanConfig,
    uint32_t ledgerVersion)
{
    releaseAssertOrThrow(mContractDataEntries.empty());
    releaseAssertOrThrow(mContractCodeEntries.empty());
    releaseAssertOrThrow(mPendingTTLs.empty());

    if (protocolVersionStartsFrom(ledgerVersion, SOROBAN_PROTOCOL_VERSION))
    {
        releaseAssertOrThrow(sorobanConfig != nullptr);
        // Check if entry is a DEADENTRY and add it to deletedKeys. Otherwise,
        // check if the entry is shadowed by a DEADENTRY.
        std::unordered_set<LedgerKey> deletedKeys;
        auto shouldAddToMap = [&deletedKeys](BucketEntry const& be,
                                             LedgerEntryType expectedType) {
            if (be.type() == DEADENTRY)
            {
                deletedKeys.insert(be.deadEntry());
                return false;
            }

            releaseAssertOrThrow(be.type() == LIVEENTRY ||
                                 be.type() == INITENTRY);
            auto lk = LedgerEntryKey(be.liveEntry());
            releaseAssertOrThrow(lk.type() == expectedType);
            return deletedKeys.find(lk) == deletedKeys.end();
        };

        auto contractDataHandler = [this,
                                    &shouldAddToMap](BucketEntry const& be) {
            if (!shouldAddToMap(be, CONTRACT_DATA))
            {
                return Loop::INCOMPLETE;
            }

            auto lk = LedgerEntryKey(be.liveEntry());
            if (!get(lk))
            {
                createContractDataEntry(be.liveEntry());
            }

            return Loop::INCOMPLETE;
        };

        auto ttlHandler = [this, &shouldAddToMap](BucketEntry const& be) {
            if (!shouldAddToMap(be, TTL))
            {
                return Loop::INCOMPLETE;
            }

            auto lk = LedgerEntryKey(be.liveEntry());
            if (!hasTTL(lk))
            {
                createTTL(be.liveEntry());
            }

            return Loop::INCOMPLETE;
        };

        auto contractCodeHandler = [this, &shouldAddToMap, &sorobanConfig,
                                    ledgerVersion](BucketEntry const& be) {
            if (!shouldAddToMap(be, CONTRACT_CODE))
            {
                return Loop::INCOMPLETE;
            }

            auto lk = LedgerEntryKey(be.liveEntry());
            if (!get(lk))
            {
                createContractCodeEntry(be.liveEntry(), *sorobanConfig,
                                        ledgerVersion);
            }

            return Loop::INCOMPLETE;
        };

        snap->scanForEntriesOfType(CONTRACT_DATA, contractDataHandler);
        snap->scanForEntriesOfType(TTL, ttlHandler);
        snap->scanForEntriesOfType(CONTRACT_CODE, contractCodeHandler);
    }

    mLastClosedLedgerSeq = snap->getLedgerSeq();
    checkUpdateInvariants();
}

void
InMemorySorobanState::updateState(std::vector<LedgerEntry> const& initEntries,
                                  std::vector<LedgerEntry> const& liveEntries,
                                  std::vector<LedgerKey> const& deadEntries,
                                  LedgerHeader const& lh,
                                  SorobanNetworkConfig const* sorobanConfig)
{
    // After initialization, we must apply every ledger in order to the
    // in-memory state with no gaps.
    releaseAssertOrThrow(mLastClosedLedgerSeq + 1 == lh.ledgerSeq);
    mLastClosedLedgerSeq = lh.ledgerSeq;

    // We only store soroban entries, no reason to check before protocol 20
    if (protocolVersionStartsFrom(lh.ledgerVersion, SOROBAN_PROTOCOL_VERSION))
    {
        releaseAssertOrThrow(sorobanConfig != nullptr);
        uint32_t ledgerVersion = lh.ledgerVersion;
        for (auto const& entry : initEntries)
        {
            if (entry.data.type() == CONTRACT_DATA)
            {
                createContractDataEntry(entry);
            }
            else if (entry.data.type() == CONTRACT_CODE)
            {
                createContractCodeEntry(entry, *sorobanConfig, ledgerVersion);
            }
            else if (entry.data.type() == TTL)
            {
                createTTL(entry);
            }
        }

        for (auto const& entry : liveEntries)
        {
            if (entry.data.type() == CONTRACT_DATA)
            {
                updateContractData(entry);
            }
            else if (entry.data.type() == CONTRACT_CODE)
            {
                updateContractCode(entry, *sorobanConfig, ledgerVersion);
            }
            else if (entry.data.type() == TTL)
            {
                updateTTL(entry);
            }
        }

        for (auto const& key : deadEntries)
        {
            if (key.type() == CONTRACT_DATA)
            {
                deleteContractData(key);
            }
            else if (key.type() == CONTRACT_CODE)
            {
                deleteContractCode(key);
            }
            // No need to evict TTLs, they are stored with their associated
            // entry
        }
    }

    checkUpdateInvariants();
}

void
InMemorySorobanState::recomputeContractCodeSize(
    SorobanNetworkConfig const& sorobanConfig, uint32_t ledgerVersion)
{
    for (auto& [_, entry] : mContractCodeEntries)
    {
        uint32_t newSize = contractCodeSizeForRent(
            *entry.ledgerEntry, sorobanConfig, ledgerVersion);
        updateStateSizeOnEntryUpdate(entry.sizeBytes, newSize,
                                     /*isContractCode=*/true);
        entry.sizeBytes = newSize;
    }
}

uint64_t
InMemorySorobanState::getSize() const
{
    releaseAssertOrThrow(mContractCodeStateSize >= 0);
    releaseAssertOrThrow(mContractDataStateSize >= 0);
    return static_cast<uint64_t>(mContractCodeStateSize +
                                 mContractDataStateSize);
}

void
InMemorySorobanState::reportMetrics(SorobanMetrics& metrics) const
{
    metrics.mContractCodeStateSize.set_count(mContractCodeStateSize);
    metrics.mContractDataStateSize.set_count(mContractDataStateSize);
    metrics.mContractCodeEntryCount.set_count(mContractCodeEntries.size());
    metrics.mContractDataEntryCount.set_count(mContractDataEntries.size());
}

void
InMemorySorobanState::manuallyAdvanceLedgerHeader(LedgerHeader const& lh)
{
    mLastClosedLedgerSeq = lh.ledgerSeq;
}

void
InMemorySorobanState::checkUpdateInvariants() const
{
    // No TTLs should be orphaned after finishing an update
    releaseAssertOrThrow(mPendingTTLs.empty());
}
void
InMemorySorobanState::updateStateSizeOnEntryUpdate(uint32_t oldEntrySize,
                                                   uint32_t newEntrySize,
                                                   bool isContractCode)
{
    int64_t sizeDelta =
        static_cast<int64_t>(newEntrySize) - static_cast<int64_t>(oldEntrySize);

    if (isContractCode)
    {
        releaseAssertOrThrow(mContractCodeStateSize + sizeDelta >= 0);
        releaseAssertOrThrow(sizeDelta <= 0 ||
                             mContractCodeStateSize <=
                                 std::numeric_limits<int64_t>::max() -
                                     sizeDelta);
        mContractCodeStateSize += sizeDelta;
    }
    else
    {
        releaseAssertOrThrow(mContractDataStateSize + sizeDelta >= 0);
        releaseAssertOrThrow(sizeDelta <= 0 ||
                             mContractDataStateSize <=
                                 std::numeric_limits<int64_t>::max() -
                                     sizeDelta);
        mContractDataStateSize += sizeDelta;
    }
}

#ifdef BUILD_TESTS
void
InMemorySorobanState::clearForTesting()
{
    mContractDataEntries.clear();
    mContractCodeEntries.clear();
    mPendingTTLs.clear();
    mLastClosedLedgerSeq = 0;
    mContractCodeStateSize = 0;
    mContractDataStateSize = 0;
}
#endif
}
