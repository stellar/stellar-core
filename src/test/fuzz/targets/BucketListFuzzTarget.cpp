// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/fuzz/FuzzTargetRegistry.h"

#include "bucket/BucketManager.h"
#include "bucket/HotArchiveBucketList.h"
#include "bucket/LiveBucketList.h"
#include "bucket/test/BucketTestUtils.h"
#include "crypto/SHA.h"
#include "ledger/ImmutableLedgerView.h"
#include "main/Application.h"
#include "test/Catch2.h"
#include "test/fuzz/FuzzUtils.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"

#include <algorithm>
#include <map>
#include <set>

namespace stellar
{
namespace
{

// This harness fuzzes the bucket-list storage layer directly. It decodes input
// into a small deterministic stream of live-bucket and hot-archive mutations,
// applies them with LiveBucketList::addBatch and HotArchiveBucketList::addBatch,
// and checks the resulting ApplyLedgerView against a local model. It is meant
// to catch bucket-list bugs such as stale reads after shadowing, lost updates,
// bad delete/restore tombstone handling, live/archive cross-contamination, and
// merge-order mistakes across bucket levels.
//
// It does not emulate the production Soroban expiration pipeline. In
// production, persistent entries move into the hot archive only after higher
// level ledger-close logic interprets paired TTL entries and decides that live
// entries have expired. This target starts below that abstraction boundary: an
// ARCHIVE_UPSERT means "the higher layer has already decided this entry should
// be archived." For that reason the model intentionally avoids TTL entries and
// cannot catch bugs in TTL extension, expiration discovery, or the live-to-
// archive decision process. Those belong in a higher-level Soroban archival
// lifecycle harness.

constexpr size_t MAX_OPS = 128;
constexpr uint8_t KEY_SLOT_COUNT = 16;
constexpr size_t MAX_BATCH_CHANGES = 8;

enum class BucketOpKind : uint8_t
{
    LIVE_UPSERT = 0,
    LIVE_DELETE = 1,
    ARCHIVE_UPSERT = 2,
    ARCHIVE_RESTORE = 3,
    FLUSH = 4,
    CHECKPOINT = 5
};

struct BucketOp
{
    BucketOpKind kind;
    uint8_t keySlot;
    uint8_t valueVariant;
};

struct PendingBatch
{
    std::vector<LedgerEntry> initEntries;
    std::vector<LedgerEntry> liveEntries;
    std::vector<LedgerKey> deadEntries;
    std::vector<LedgerEntry> archiveEntries;
    std::vector<LedgerKey> restoredArchiveEntries;
    std::set<LedgerKey, LedgerEntryIdCmp> keys;

    bool
    empty() const
    {
         return initEntries.empty() && liveEntries.empty() &&
             deadEntries.empty() && archiveEntries.empty() &&
             restoredArchiveEntries.empty();
    }

    size_t
    size() const
    {
        return initEntries.size() + liveEntries.size() + deadEntries.size() +
               archiveEntries.size() + restoredArchiveEntries.size();
    }

    bool
    hasKey(LedgerKey const& key) const
    {
        return keys.find(key) != keys.end();
    }

    void
    noteKey(LedgerKey const& key)
    {
        keys.insert(key);
    }

    void
    clear()
    {
        initEntries.clear();
        liveEntries.clear();
        deadEntries.clear();
        archiveEntries.clear();
        restoredArchiveEntries.clear();
        keys.clear();
    }
};

struct ExpectedState
{
    std::map<LedgerKey, LedgerEntry, LedgerEntryIdCmp> live;
    std::set<LedgerKey, LedgerEntryIdCmp> deletedLive;
    std::map<LedgerKey, LedgerEntry, LedgerEntryIdCmp> archive;
    std::set<LedgerKey, LedgerEntryIdCmp> restoredArchive;
};

class ByteCursor
{
  public:
    ByteCursor(uint8_t const* data, size_t size) : mData(data), mSize(size)
    {
    }

    bool
    empty() const
    {
        return mPos >= mSize;
    }

    uint8_t
    nextU8()
    {
        releaseAssert(!empty());
        return mData[mPos++];
    }

  private:
    uint8_t const* mData;
    size_t mSize;
    size_t mPos{0};
};

LedgerHeader
makeHeader(uint32_t seq, uint32_t protocolVersion)
{
    LedgerHeader h;
    h.ledgerSeq = seq;
    h.ledgerVersion = protocolVersion;
    return h;
}

LedgerEntry
makeContractCodeEntry(uint8_t keySlot, uint8_t valueVariant, uint32_t seq)
{
    auto code = "bucketlist-fuzz-" + std::to_string(keySlot) + "-" +
                std::to_string(valueVariant);

    LedgerEntry e;
    e.lastModifiedLedgerSeq = seq;
    e.data.type(CONTRACT_CODE);
    auto& contractCode = e.data.contractCode();
    contractCode.ext.v(0);
    contractCode.hash = sha256("bucketlist-fuzz-key-" +
                               std::to_string(keySlot));
    contractCode.code.assign(code.begin(), code.end());
    e.ext.v(0);
    return e;
}

LedgerKey
makeContractDataKey(uint8_t keySlot)
{
    LedgerKey k(CONTRACT_DATA);
    auto& contractData = k.contractData();
    contractData.contract.type(SC_ADDRESS_TYPE_CONTRACT);
    contractData.contract.contractId() = sha256("bucketlist-fuzz-contract");
    contractData.key = makeSymbolSCVal("bucketlist-fuzz-key-" +
                                       std::to_string(keySlot));
    contractData.durability = PERSISTENT;
    return k;
}

LedgerEntry
makeContractDataEntry(uint8_t keySlot, uint8_t valueVariant, uint32_t seq)
{
    LedgerEntry e;
    e.lastModifiedLedgerSeq = seq;
    e.data.type(CONTRACT_DATA);
    auto& contractData = e.data.contractData();
    contractData.ext.v(0);
    contractData.contract.type(SC_ADDRESS_TYPE_CONTRACT);
    contractData.contract.contractId() = sha256("bucketlist-fuzz-contract");
    contractData.key = makeSymbolSCVal("bucketlist-fuzz-key-" +
                                       std::to_string(keySlot));
    contractData.durability = PERSISTENT;
    contractData.val = makeU64SCVal(valueVariant & 0x7f);
    e.ext.v(0);
    return e;
}

bool
useContractData(uint8_t valueVariant)
{
    return (valueVariant & 0x80) != 0;
}

LedgerEntry
makeEntry(uint8_t keySlot, uint8_t valueVariant, uint32_t seq)
{
    if (useContractData(valueVariant))
    {
        return makeContractDataEntry(keySlot, valueVariant, seq);
    }
    return makeContractCodeEntry(keySlot, valueVariant, seq);
}

LedgerKey
keyForSlot(uint8_t keySlot, bool contractData)
{
    if (contractData)
    {
        return makeContractDataKey(keySlot);
    }

    LedgerKey k(CONTRACT_CODE);
    k.contractCode().hash = sha256("bucketlist-fuzz-key-" +
                                   std::to_string(keySlot));
    return k;
}

LedgerKey
keyForOp(BucketOp const& op)
{
    return keyForSlot(op.keySlot, useContractData(op.valueVariant));
}

std::vector<BucketOp>
decodeOps(uint8_t const* data, size_t size)
{
    ByteCursor cursor(data, size);
    std::vector<BucketOp> ops;
    ops.reserve(std::min(MAX_OPS, size / 3));
    while (!cursor.empty() && ops.size() < MAX_OPS)
    {
        auto kind = static_cast<BucketOpKind>(cursor.nextU8() % 6);
        if (cursor.empty())
        {
            break;
        }
        auto keySlot = static_cast<uint8_t>(cursor.nextU8() % KEY_SLOT_COUNT);
        if (cursor.empty())
        {
            break;
        }
        auto valueVariant = cursor.nextU8();
        ops.push_back({kind, keySlot, valueVariant});
    }
    return ops;
}

void
checkExpectedState(Application& app, ExpectedState const& expected)
{
    auto applyView = app.getLedgerManager().copyApplyLedgerView();
    for (auto const& [key, entry] : expected.live)
    {
        auto loaded = applyView.loadLiveEntry(key);
        if (!loaded)
        {
            throw std::runtime_error("bucketlist live entry missing");
        }
        if (*loaded != entry)
        {
            throw std::runtime_error("bucketlist live entry stale");
        }
        if (applyView.loadArchiveEntry(key))
        {
            throw std::runtime_error("bucketlist live entry also archived");
        }
    }

    for (auto const& key : expected.deletedLive)
    {
        if (expected.live.find(key) == expected.live.end() &&
            applyView.loadLiveEntry(key))
        {
            throw std::runtime_error("bucketlist deleted live entry present");
        }
    }

    for (auto const& [key, entry] : expected.archive)
    {
        auto loaded = applyView.loadArchiveEntry(key);
        if (!loaded)
        {
            throw std::runtime_error("bucketlist archive entry missing");
        }
        if (loaded->type() != HOT_ARCHIVE_ARCHIVED ||
            loaded->archivedEntry() != entry)
        {
            throw std::runtime_error("bucketlist archive entry stale");
        }
        if (applyView.loadLiveEntry(key))
        {
            throw std::runtime_error("bucketlist archive entry also live");
        }
    }

    for (auto const& key : expected.restoredArchive)
    {
        if (expected.archive.find(key) == expected.archive.end() &&
            applyView.loadArchiveEntry(key))
        {
            throw std::runtime_error("bucketlist restored archive entry present");
        }
    }

    for (uint8_t keySlot = 0; keySlot < KEY_SLOT_COUNT; ++keySlot)
    {
        for (bool contractData : {false, true})
        {
            auto key = keyForSlot(keySlot, contractData);
            if (expected.live.find(key) == expected.live.end() &&
                expected.deletedLive.find(key) == expected.deletedLive.end() &&
                applyView.loadLiveEntry(key))
            {
                throw std::runtime_error("bucketlist absent live entry present");
            }
            if (expected.archive.find(key) == expected.archive.end() &&
                expected.restoredArchive.find(key) ==
                    expected.restoredArchive.end() &&
                applyView.loadArchiveEntry(key))
            {
                throw std::runtime_error(
                    "bucketlist absent archive entry present");
            }
        }
    }
}

void
flushBatch(Application& app, PendingBatch& pending, ExpectedState const& expected,
           uint32_t& ledgerSeq, uint32_t protocolVersion)
{
    if (pending.empty())
    {
        return;
    }

    auto header = makeHeader(ledgerSeq, protocolVersion);
    auto& bm = app.getBucketManager();
    bm.getLiveBucketList().addBatch(app, header.ledgerSeq,
                                    header.ledgerVersion, pending.initEntries,
                                    pending.liveEntries, pending.deadEntries);
    bm.getHotArchiveBucketList().addBatch(app, header.ledgerSeq,
                                          header.ledgerVersion,
                                          pending.archiveEntries,
                                          pending.restoredArchiveEntries);
    app.getLedgerManager().updateCanonicalStateForTesting(header);
    pending.clear();
    checkExpectedState(app, expected);
    ++ledgerSeq;
}

class BucketListFuzzTarget : public FuzzTarget
{
  public:
    std::string
    name() const override
    {
        return "bucketlist";
    }

    std::string
    description() const override
    {
        return "Fuzz live/hot-archive bucketlist transitions with snapshot invariant checks";
    }

    void
    initialize() override
    {
        reinitializeAllGlobalStateForFuzzing(1);
    }

    FuzzResultCode
    run(uint8_t const* data, size_t size) override
    {
        if (data == nullptr || size < 3 || size > maxInputSize())
        {
            return FuzzResultCode::FUZZ_REJECTED;
        }

        auto ops = decodeOps(data, size);
        if (ops.empty())
        {
            return FuzzResultCode::FUZZ_REJECTED;
        }

        VirtualClock clock;
        auto cfg = getFuzzConfig(601);
        auto app = createTestApplication<BucketTestUtils::BucketTestApplication>(
            clock, cfg);
        auto protocolVersion = BucketTestUtils::getAppLedgerVersion(*app);
        auto ledgerSeq = app->getLedgerManager().getLastClosedLedgerNum() + 1;

        PendingBatch pending;
        ExpectedState expected;
        for (auto const& op : ops)
        {
            auto key = keyForOp(op);
            if (pending.hasKey(key))
            {
                flushBatch(*app, pending, expected, ledgerSeq, protocolVersion);
            }

            switch (op.kind)
            {
            case BucketOpKind::LIVE_UPSERT:
            {
                auto isUpdate = expected.live.find(key) != expected.live.end();
                auto entry = makeEntry(op.keySlot, op.valueVariant, ledgerSeq);
                if (isUpdate)
                {
                    pending.liveEntries.push_back(entry);
                }
                else
                {
                    pending.initEntries.push_back(entry);
                }
                expected.live[key] = entry;
                expected.deletedLive.erase(key);
                expected.archive.erase(key);
                expected.restoredArchive.insert(key);
                pending.restoredArchiveEntries.push_back(key);
                pending.noteKey(key);
                break;
            }
            case BucketOpKind::LIVE_DELETE:
                pending.deadEntries.push_back(key);
                expected.live.erase(key);
                expected.deletedLive.insert(key);
                pending.noteKey(key);
                break;
            case BucketOpKind::ARCHIVE_UPSERT:
            {
                auto entry = makeEntry(op.keySlot, op.valueVariant, ledgerSeq);
                pending.archiveEntries.push_back(entry);
                expected.archive[key] = entry;
                expected.restoredArchive.erase(key);
                expected.live.erase(key);
                expected.deletedLive.insert(key);
                pending.deadEntries.push_back(key);
                pending.noteKey(key);
                break;
            }
            case BucketOpKind::ARCHIVE_RESTORE:
                pending.restoredArchiveEntries.push_back(key);
                expected.archive.erase(key);
                expected.restoredArchive.insert(key);
                pending.noteKey(key);
                break;
            case BucketOpKind::FLUSH:
                flushBatch(*app, pending, expected, ledgerSeq, protocolVersion);
                break;
            case BucketOpKind::CHECKPOINT:
                flushBatch(*app, pending, expected, ledgerSeq, protocolVersion);
                checkExpectedState(*app, expected);
                break;
            }

            if (pending.size() >= MAX_BATCH_CHANGES)
            {
                flushBatch(*app, pending, expected, ledgerSeq, protocolVersion);
            }
        }

        flushBatch(*app, pending, expected, ledgerSeq, protocolVersion);
        checkExpectedState(*app, expected);
        app->gracefulStop();

        return FuzzResultCode::FUZZ_SUCCESS;
    }

    void
    shutdown() override
    {
    }

    size_t
    maxInputSize() const override
    {
        return 4 * 1024;
    }

    std::vector<uint8_t>
    generateSeedInput() override
    {
        return {
            static_cast<uint8_t>(BucketOpKind::LIVE_UPSERT),    0, 10,
            static_cast<uint8_t>(BucketOpKind::FLUSH),          0, 0,
            static_cast<uint8_t>(BucketOpKind::LIVE_UPSERT),    0, 11,
            static_cast<uint8_t>(BucketOpKind::LIVE_UPSERT),    1, 20,
            static_cast<uint8_t>(BucketOpKind::FLUSH),          0, 0,
            static_cast<uint8_t>(BucketOpKind::LIVE_DELETE),    0, 0,
            static_cast<uint8_t>(BucketOpKind::ARCHIVE_UPSERT), 2, 30,
            static_cast<uint8_t>(BucketOpKind::FLUSH),          0, 0,
            static_cast<uint8_t>(BucketOpKind::LIVE_UPSERT),    0, 12,
            static_cast<uint8_t>(BucketOpKind::ARCHIVE_RESTORE), 2, 0,
            static_cast<uint8_t>(BucketOpKind::LIVE_UPSERT),    3, 0x81,
            static_cast<uint8_t>(BucketOpKind::ARCHIVE_UPSERT), 4, 0x82,
            static_cast<uint8_t>(BucketOpKind::CHECKPOINT),     0, 0,
        };
    }
};

REGISTER_FUZZ_TARGET(
    BucketListFuzzTarget, "bucketlist",
    "Fuzz live/hot-archive bucketlist transitions with snapshot invariant checks");

} // namespace
} // namespace stellar
