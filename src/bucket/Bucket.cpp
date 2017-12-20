// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/Bucket.h"
// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "bucket/BucketApplicator.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "bucket/LedgerCmp.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "ledger/AccountFrame.h"
#include "ledger/DataFrame.h"
#include "ledger/EntryFrame.h"
#include "ledger/LedgerDelta.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "medida/medida.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include "util/TmpDir.h"
#include "util/XDRStream.h"
#include "util/make_unique.h"
#include "xdrpp/message.h"
#include <cassert>
#include <future>

namespace stellar
{

static std::string
randomBucketName(std::string const& tmpDir)
{
    for (;;)
    {
        std::string name =
            tmpDir + "/tmp-bucket-" + binToHex(randomBytes(8)) + ".xdr";
        std::ifstream ifile(name);
        if (!ifile)
        {
            return name;
        }
    }
}

Bucket::Bucket(std::string const& filename, Hash const& hash)
    : mFilename(filename), mHash(hash)
{
    assert(filename.empty() || fs::exists(filename));
    if (!filename.empty())
    {
        CLOG(TRACE, "Bucket")
            << "Bucket::Bucket() created, file exists : " << mFilename;
    }
}

Bucket::~Bucket()
{
    if (!mFilename.empty() && !mRetain)
    {
        CLOG(TRACE, "Bucket") << "Bucket::~Bucket removing file: " << mFilename;
        std::remove(mFilename.c_str());
    }
}

Bucket::Bucket()
{
}

Hash const&
Bucket::getHash() const
{
    return mHash;
}

std::string const&
Bucket::getFilename() const
{
    return mFilename;
}

void
Bucket::setRetain(bool r)
{
    mRetain = r;
}

/**
 * Helper class that reads from the file underlying a bucket, keeping the bucket
 * alive for the duration of its existence.
 */
void
Bucket::InputIterator::loadEntry()
{
    if (mIn.readOne(mEntry))
    {
        mEntryPtr = &mEntry;
    }
    else
    {
        mEntryPtr = nullptr;
    }
}

Bucket::InputIterator::operator bool() const
{
    return mEntryPtr != nullptr;
}

BucketEntry const& Bucket::InputIterator::operator*()
{
    return *mEntryPtr;
}

Bucket::InputIterator::InputIterator(std::shared_ptr<Bucket const> bucket)
    : mBucket(bucket), mEntryPtr(nullptr)
{
    if (!mBucket->mFilename.empty())
    {
        CLOG(TRACE, "Bucket") << "Bucket::InputIterator opening file to read: "
                              << mBucket->mFilename;
        mIn.open(mBucket->mFilename);
        loadEntry();
    }
}

Bucket::InputIterator::~InputIterator()
{
    mIn.close();
}

Bucket::InputIterator& Bucket::InputIterator::operator++()
{
    if (mIn)
    {
        loadEntry();
    }
    else
    {
        mEntryPtr = nullptr;
    }
    return *this;
}

/**
 * Helper class that points to an output tempfile. Absorbs BucketEntries and
 * hashes them while writing to either destination. Produces a Bucket when done.
 */
Bucket::OutputIterator::OutputIterator(std::string const& tmpDir,
                                       bool keepDeadEntries)
    : mFilename(randomBucketName(tmpDir))
    , mBuf(nullptr)
    , mHasher(SHA256::create())
    , mKeepDeadEntries(keepDeadEntries)
{
    CLOG(TRACE, "Bucket") << "Bucket::OutputIterator opening file to write: "
                          << mFilename;
    mOut.open(mFilename);
}

void
Bucket::OutputIterator::put(BucketEntry const& e)
{
    if (!mKeepDeadEntries && e.type() == DEADENTRY)
    {
        return;
    }

    // Check to see if there's an existing buffered entry.
    if (mBuf)
    {
        BucketEntryIdCmp cmp;
        // cmp(e, *mBuf) means e < *mBuf; this should never be true since
        // it would mean that we're getting entries out of order.
        assert(!cmp(e, *mBuf));

        // Check to see if the new entry should flush (greater identity), or
        // merely replace (same identity), the buffered entry.
        if (cmp(*mBuf, e))
        {
            mOut.writeOne(*mBuf, mHasher.get(), &mBytesPut);
            mObjectsPut++;
        }
    }
    else
    {
        mBuf = make_unique<BucketEntry>();
    }

    // In any case, replace *mBuf with e.
    *mBuf = e;
}

std::shared_ptr<Bucket>
Bucket::OutputIterator::getBucket(BucketManager& bucketManager)
{
    assert(mOut);
    if (mBuf)
    {
        mOut.writeOne(*mBuf, mHasher.get(), &mBytesPut);
        mObjectsPut++;
        mBuf.reset();
    }

    mOut.close();
    if (mObjectsPut == 0 || mBytesPut == 0)
    {
        assert(mObjectsPut == 0);
        assert(mBytesPut == 0);
        CLOG(DEBUG, "Bucket") << "Deleting empty bucket file " << mFilename;
        std::remove(mFilename.c_str());
        return std::make_shared<Bucket>();
    }
    return bucketManager.adoptFileAsBucket(mFilename, mHasher->finish(),
                                           mObjectsPut, mBytesPut);
}

bool
Bucket::containsBucketIdentity(BucketEntry const& id) const
{
    BucketEntryIdCmp cmp;
    Bucket::InputIterator iter(shared_from_this());
    while (iter)
    {
        if (!(cmp(*iter, id) || cmp(id, *iter)))
        {
            return true;
        }
        ++iter;
    }
    return false;
}

std::pair<size_t, size_t>
Bucket::countLiveAndDeadEntries() const
{
    size_t live = 0, dead = 0;
    Bucket::InputIterator iter(shared_from_this());
    while (iter)
    {
        if ((*iter).type() == LIVEENTRY)
        {
            ++live;
        }
        else
        {
            ++dead;
        }
        ++iter;
    }
    return std::make_pair(live, dead);
}

void
Bucket::apply(Database& db) const
{
    BucketApplicator applicator(db, shared_from_this());
    while (applicator)
    {
        applicator.advance();
    }
}

std::shared_ptr<Bucket>
Bucket::fresh(BucketManager& bucketManager,
              std::vector<LedgerEntry> const& liveEntries,
              std::vector<LedgerKey> const& deadEntries)
{
    std::vector<BucketEntry> live, dead, combined;
    live.reserve(liveEntries.size());
    dead.reserve(deadEntries.size());

    for (auto const& e : liveEntries)
    {
        BucketEntry ce;
        ce.type(LIVEENTRY);
        ce.liveEntry() = e;
        live.push_back(ce);
    }

    for (auto const& e : deadEntries)
    {
        BucketEntry ce;
        ce.type(DEADENTRY);
        ce.deadEntry() = e;
        dead.push_back(ce);
    }

    std::sort(live.begin(), live.end(), BucketEntryIdCmp());

    std::sort(dead.begin(), dead.end(), BucketEntryIdCmp());

    OutputIterator liveOut(bucketManager.getTmpDir(), true);
    OutputIterator deadOut(bucketManager.getTmpDir(), true);
    for (auto const& e : live)
    {
        liveOut.put(e);
    }
    for (auto const& e : dead)
    {
        deadOut.put(e);
    }

    auto liveBucket = liveOut.getBucket(bucketManager);
    auto deadBucket = deadOut.getBucket(bucketManager);
    return Bucket::merge(bucketManager, liveBucket, deadBucket);
}

inline void
maybePut(Bucket::OutputIterator& out, BucketEntry const& entry,
         std::vector<Bucket::InputIterator>& shadowIterators)
{
    BucketEntryIdCmp cmp;
    for (auto& si : shadowIterators)
    {
        // Advance the shadowIterator while it's less than the candidate
        while (si && cmp(*si, entry))
        {
            ++si;
        }
        // We have stepped si forward to the point that either si is exhausted,
        // or else *si >= entry; we now check the opposite direction to see if
        // we have equality.
        if (si && !cmp(entry, *si))
        {
            // If so, then entry is shadowed in at least one level and we will
            // not be doing a 'put'; we return early. There is no need to
            // advance the other iterators, they will advance as and if
            // necessary in future calls to maybePut.
            return;
        }
    }
    // Nothing shadowed.
    out.put(entry);
}

std::shared_ptr<Bucket>
Bucket::merge(BucketManager& bucketManager,
              std::shared_ptr<Bucket> const& oldBucket,
              std::shared_ptr<Bucket> const& newBucket,
              std::vector<std::shared_ptr<Bucket>> const& shadows,
              bool keepDeadEntries)
{
    // This is the key operation in the scheme: merging two (read-only)
    // buckets together into a new 3rd bucket, while calculating its hash,
    // in a single pass.

    assert(oldBucket);
    assert(newBucket);

    Bucket::InputIterator oi(oldBucket);
    Bucket::InputIterator ni(newBucket);

    std::vector<Bucket::InputIterator> shadowIterators(shadows.begin(),
                                                       shadows.end());

    auto timer = bucketManager.getMergeTimer().TimeScope();
    Bucket::OutputIterator out(bucketManager.getTmpDir(), keepDeadEntries);

    BucketEntryIdCmp cmp;
    while (oi || ni)
    {
        if (!ni)
        {
            // Out of new entries, take old entries.
            maybePut(out, *oi, shadowIterators);
            ++oi;
        }
        else if (!oi)
        {
            // Out of old entries, take new entries.
            maybePut(out, *ni, shadowIterators);
            ++ni;
        }
        else if (cmp(*oi, *ni))
        {
            // Next old-entry has smaller key, take it.
            maybePut(out, *oi, shadowIterators);
            ++oi;
        }
        else if (cmp(*ni, *oi))
        {
            // Next new-entry has smaller key, take it.
            maybePut(out, *ni, shadowIterators);
            ++ni;
        }
        else
        {
            // Old and new are for the same key, take new.
            maybePut(out, *ni, shadowIterators);
            ++oi;
            ++ni;
        }
    }
    return out.getBucket(bucketManager);
}

static void
compareSizes(std::string const& objType, uint64_t inDatabase,
             uint64_t inBucketlist)
{
    if (inDatabase != inBucketlist)
    {
        throw std::runtime_error(fmt::format(
            "{} object count mismatch: DB has {}, BucketList has {}", objType,
            inDatabase, inBucketlist));
    }
}

// FIXME issue NNN: this should be refactored to run in a read-transaction on a
// background thread and take a soci::session& rather than Database&. For now we
// code it to run on main thread because there's a bunch of code to move around
// in the ledger frames and db layer to make that work.

void
checkDBAgainstBuckets(medida::MetricsRegistry& metrics,
                      BucketManager& bucketManager, Database& db,
                      BucketList& bl)
{
    CLOG(INFO, "Bucket") << "CheckDB starting";
    auto execTimer =
        metrics.NewTimer({"bucket", "checkdb", "execute"}).TimeScope();

    // Step 1: Collect all buckets to merge.
    std::vector<std::shared_ptr<Bucket>> buckets;
    for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        CLOG(INFO, "Bucket") << "CheckDB collecting buckets from level " << i;
        auto& level = bl.getLevel(i);
        auto& next = level.getNext();
        if (next.isLive())
        {
            CLOG(INFO, "Bucket")
                << "CheckDB resolving future bucket on level " << i;
            buckets.push_back(next.resolve());
        }
        buckets.push_back(level.getCurr());
        buckets.push_back(level.getSnap());
    }

    if (buckets.empty())
    {
        CLOG(INFO, "Bucket") << "CheckDB found no buckets, returning";
        return;
    }

    // Step 2: merge all buckets into a single super-bucket.
    auto i = buckets.begin();
    assert(i != buckets.end());
    std::shared_ptr<Bucket> superBucket = *i;
    while (++i != buckets.end())
    {
        auto mergeTimer =
            metrics.NewTimer({"bucket", "checkdb", "merge"}).TimeScope();
        assert(superBucket);
        assert(*i);
        superBucket = Bucket::merge(bucketManager, *i, superBucket);
        assert(superBucket);
    }

    CLOG(INFO, "Bucket") << "CheckDB starting object comparison";

    // Step 3: scan the superbucket, checking each object against the DB and
    // counting objects along the way.
    uint64_t nAccounts = 0, nTrustLines = 0, nOffers = 0, nData = 0;
    {
        auto& meter = metrics.NewMeter({"bucket", "checkdb", "object-compare"},
                                       "comparison");
        auto compareTimer =
            metrics.NewTimer({"bucket", "checkdb", "compare"}).TimeScope();
        for (Bucket::InputIterator iter(superBucket); iter; ++iter)
        {
            meter.Mark();
            auto& e = *iter;
            if (e.type() == LIVEENTRY)
            {
                switch (e.liveEntry().data.type())
                {
                case ACCOUNT:
                    ++nAccounts;
                    break;
                case TRUSTLINE:
                    ++nTrustLines;
                    break;
                case OFFER:
                    ++nOffers;
                    break;
                case DATA:
                    ++nData;
                    break;
                }
                auto s = EntryFrame::checkAgainstDatabase(e.liveEntry(), db);
                if (!s.empty())
                {
                    throw std::runtime_error{s};
                }
                if (meter.count() % 100 == 0)
                {
                    CLOG(INFO, "Bucket")
                        << "CheckDB compared " << meter.count() << " objects";
                }
            }
        }
    }

    // Step 4: confirm size of datasets matches size of datasets in DB.
    soci::session& sess = db.getSession();
    compareSizes("account", AccountFrame::countObjects(sess), nAccounts);
    compareSizes("trustline", TrustFrame::countObjects(sess), nTrustLines);
    compareSizes("offer", OfferFrame::countObjects(sess), nOffers);
    compareSizes("data", DataFrame::countObjects(sess), nData);
}
}
