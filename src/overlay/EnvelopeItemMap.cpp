// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/EnvelopeItemMap.h"
#include "util/XDROperators.h"

namespace stellar
{

void
EnvelopeItemMap::add(SCPEnvelope const& envelope,
                     std::vector<ItemKey> const& items)
{
    for (auto const& itemKey : items)
    {
        add(envelope, itemKey);
    }
}

void
EnvelopeItemMap::add(SCPEnvelope const& envelope, ItemKey itemKey)
{
    mItemEnvelopes[itemKey].insert(envelope);
    mEnvelopeItems[envelope].insert(itemKey);
}

std::set<ItemKey>
EnvelopeItemMap::remove(SCPEnvelope const& envelope)
{
    auto result = std::set<ItemKey>{};
    auto it = mEnvelopeItems.find(envelope);
    if (it == std::end(mEnvelopeItems))
    {
        return result;
    }

    for (auto const& itemKey : it->second)
    {
        if (removeEnvelopeLink(envelope, itemKey))
        {
            // it was last envelope waiting for this item
            result.insert(itemKey);
        }
    }

    mEnvelopeItems.erase(it);
    return result;
}

std::set<SCPEnvelope>
EnvelopeItemMap::remove(ItemKey itemKey)
{
    auto result = std::set<SCPEnvelope>{};
    auto it = mItemEnvelopes.find(itemKey);
    if (it == std::end(mItemEnvelopes))
    {
        return result;
    }

    for (auto const& envelope : it->second)
    {
        if (removeItemLink(envelope, itemKey))
        {
            // it was last item this envelope was waiting for, we can add it to
            // result
            result.insert(envelope);
        }
    }

    mItemEnvelopes.erase(it);
    return result;
}

std::set<ItemKey>
EnvelopeItemMap::removeIf(std::function<bool(SCPEnvelope const&)> cond)
{
    auto result = std::set<ItemKey>{};
    for (auto it = std::begin(mEnvelopeItems); it != std::end(mEnvelopeItems);)
    {
        if (cond(it->first))
        {
            it = mEnvelopeItems.erase(it);
        }
        else
        {
            it++;
        }
    }

    for (auto it = std::begin(mItemEnvelopes); it != mItemEnvelopes.end();)
    {
        for (auto itE = std::begin(it->second); itE != std::end(it->second);)
        {
            if (cond(*itE))
            {
                itE = it->second.erase(itE);
            }
            else
            {
                itE++;
            }
        }
        if (it->second.empty())
        {
            result.insert(it->first);
            it = mItemEnvelopes.erase(it);
        }
        else
        {
            it++;
        }
    }

    return result;
}

std::set<SCPEnvelope>
EnvelopeItemMap::envelopes(ItemKey itemKey)
{
    auto it = mItemEnvelopes.find(itemKey);
    return it == std::end(mItemEnvelopes) ? std::set<SCPEnvelope>{}
                                          : it->second;
}

bool
EnvelopeItemMap::removeEnvelopeLink(SCPEnvelope const& envelope,
                                    ItemKey itemKey)
{
    auto it = mItemEnvelopes.find(itemKey);
    if (it == mItemEnvelopes.end())
    {
        return true;
    }

    it->second.erase(envelope);
    if (it->second.empty())
    {
        mItemEnvelopes.erase(it);
        return true;
    }

    return false;
}

bool
EnvelopeItemMap::removeItemLink(SCPEnvelope const& envelope, ItemKey itemKey)
{
    auto it = mEnvelopeItems.find(envelope);
    if (it == mEnvelopeItems.end())
    {
        return true;
    }

    it->second.erase(itemKey);
    if (it->second.empty())
    {
        mEnvelopeItems.erase(it);
        return true;
    }

    return false;
}
}
