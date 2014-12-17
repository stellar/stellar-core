#ifndef __CLFDELTA__
#define __CLFDELTA__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

/*

This is a delta that is sent by peers to catch other peers up to the network

*/

namespace stellar
{
    class CLFDelta
    {
        std::vector<LedgerEntry> mEntries;
    public:
        typedef std::shared_ptr<CLFDelta> pointer;
    };
}

#endif
