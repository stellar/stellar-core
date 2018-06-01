// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/ItemFetcherImpl.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/HerderUtils.h"
#include "main/Application.h"
#include "overlay/ItemFetchQueue.h"
#include "scp/QuorumSetUtils.h"
#include "util/Logging.h"

#include <xdrpp/marshal.h>

#define QSET_CACHE_SIZE 10000
#define TXSET_CACHE_SIZE 10000

namespace stellar
{

namespace
{

std::vector<ItemKey>
envelopeItems(SCPEnvelope const& envelope)
{
    auto result = std::vector<ItemKey>{};
    auto qSet = getQuorumSetHash(envelope);
    result.push_back(ItemKey{ItemType::QUORUM_SET, qSet});

    auto txSetHashes = getTxSetHashes(envelope);
    for (auto const& txSetHash : txSetHashes)
    {
        result.push_back(ItemKey{ItemType::TX_SET, txSetHash});
    }

    return result;
}
}

ItemFetcherImpl::ItemFetcherImpl(Application& app)
    : mApp(app)
    , mItemFetchQueue{app}
    , mQSetCache{QSET_CACHE_SIZE}
    , mTxSetCache{TXSET_CACHE_SIZE}
{
}

ItemFetcherImpl::AddResult
ItemFetcherImpl::add(SCPQuorumSet const& qSet, bool force)
{
    auto hash = sha256(xdr::xdr_to_opaque(qSet));
    CLOG(TRACE, "Overlay") << "Add SCPQSet " << hexAbbrev(hash)
                           << (force ? ", forced" : "");

    auto itemKey = ItemKey{ItemType::QUORUM_SET, hash};
    if (!mItemFetchQueue.stopFetch(itemKey) && !force)
    {
        return std::make_pair(false, itemKey);
    }

    if (!isQuorumSetSane(qSet, false))
    {
        return std::make_pair(false, itemKey);
    }

    mQSetCache.put(hash, std::make_shared<SCPQuorumSet>(qSet));
    return std::make_pair(true, itemKey);
}

ItemFetcherImpl::AddResult
ItemFetcherImpl::add(TransactionSet const& txSet, bool force)
{
    auto txSetFrame = std::make_shared<TxSetFrame>(mApp.getNetworkID(), txSet);
    auto hash = txSetFrame->getContentsHash();
    CLOG(TRACE, "Overlay") << "Add TxSet " << hexAbbrev(hash)
                           << (force ? ", forced" : "");

    auto itemKey = ItemKey{ItemType::TX_SET, hash};
    if (!mItemFetchQueue.stopFetch(itemKey) && !force)
    {
        return std::make_pair(false, itemKey);
    }

    mTxSetCache.put(hash, txSetFrame);
    return std::make_pair(true, itemKey);
}

void
ItemFetcherImpl::touch(ItemKey key)
{
    if (!contains(key))
    {
        return;
    }

    switch (key.getType())
    {
    case ItemType::QUORUM_SET:
    {
        mQSetCache.get(key.getHash());
        break;
    }
    case ItemType::TX_SET:
    {
        mTxSetCache.get(key.getHash());
        break;
    }
    }
}

bool
ItemFetcherImpl::contains(ItemKey key) const
{
    switch (key.getType())
    {
    case ItemType::QUORUM_SET:
    {
        return mQSetCache.exists(key.getHash());
    }
    case ItemType::TX_SET:
    {
        return mTxSetCache.exists(key.getHash());
    }
    }

    return false;
}

SCPQuorumSetPtr
ItemFetcherImpl::getQuorumSet(Hash hash)
{
    if (mQSetCache.exists(hash))
    {
        return mQSetCache.get(hash);
    }

    return SCPQuorumSetPtr{};
}

TxSetFramePtr
ItemFetcherImpl::getTxSet(Hash hash)
{
    if (mTxSetCache.exists(hash))
    {
        return mTxSetCache.get(hash);
    }

    return TxSetFramePtr();
}

std::vector<ItemKey>
ItemFetcherImpl::fetchFor(SCPEnvelope const& envelope, Peer::pointer peer)
{
    auto result = std::vector<ItemKey>{};
    for (auto const& itemKey : envelopeItems(envelope))
    {
        if (!contains(itemKey))
        {
            mItemFetchQueue.addKnowing(peer, itemKey);
            mItemFetchQueue.startFetch(itemKey);
            result.push_back(itemKey);
        }
    }
    return result;
}

void
ItemFetcherImpl::forget(ItemKey itemKey)
{
    mItemFetchQueue.stopFetch(itemKey);
}

void
ItemFetcherImpl::removeKnowing(Peer::pointer peer, ItemKey itemKey)
{
    mItemFetchQueue.removeKnowing(peer, itemKey);
}
}
