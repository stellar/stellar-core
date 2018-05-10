#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "item/ItemKey.h"
#include "xdr/Stellar-SCP.h"

#include <functional>
#include <map>
#include <set>

namespace stellar
{

/**
 * Maps bidirectionally envelopes with items that those require.
 */
class EnvelopeItemMap
{
  public:
    /**
     * Add set of new mappings between envelope and each of items.
     */
    void add(SCPEnvelope const& envelope, std::vector<ItemKey> const& items);

    /**
     * Add new mappings between envelope itemKey.
     */
    void add(SCPEnvelope const& envelope, ItemKey itemKey);

    /**
     * Remove all mappings with envelope. Returns set of itemKeys that do not
     * have any more mapped envelopes.
     */
    std::set<ItemKey> remove(SCPEnvelope const& envelope);

    /**
     * Remove all mappings with itemKey. Returns set of envelopes that do not
     * have any more mapped itemKeys.
     */
    std::set<SCPEnvelope> remove(ItemKey itemKey);

    /**
     * Remove all mapping matching predicate cond. Returns set of itemKeys that
     * do not have any more mapped envelopes.
     */
    std::set<ItemKey> removeIf(std::function<bool(SCPEnvelope const&)> cond);

    /**
     * Return all envelopes that are mapped to itemKey.
     */
    std::set<SCPEnvelope> envelopes(ItemKey itemKey);

  private:
    std::map<SCPEnvelope, std::set<ItemKey>> mEnvelopeItems;
    std::map<ItemKey, std::set<SCPEnvelope>> mItemEnvelopes;

    bool removeEnvelopeLink(SCPEnvelope const& envelope, ItemKey itemKey);
    bool removeItemLink(SCPEnvelope const& envelope, ItemKey itemKey);
};
}
