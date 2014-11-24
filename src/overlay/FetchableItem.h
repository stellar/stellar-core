
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