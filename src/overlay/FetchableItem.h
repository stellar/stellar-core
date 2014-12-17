#ifndef __FETCHABLEITEM__
#define __FETCHABLEITEM__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

/*
parent of QuorumSet and TransactionSet
*/

namespace stellar
{
	class FetchableItem
	{
		uint256 mItemID;
	public:
		uint256 getItemID();

		virtual Message::pointer createMessage()=0;
	};
}

#endif
