// Scaffolding originally generated from stellar.x.
// Edit to add functionality.

#include "stellar.server.hh"

namespace stellarxdr {

	std::unique_ptr<int>
		first_server::hello(const int &arg)
	{
		std::unique_ptr<int> res(new int);

		// Fill in function body here

		return res;
	}

	std::unique_ptr<PeersReply>
		first_server::getPeers()
	{
		std::unique_ptr<PeersReply> res(new PeersReply);

		// Fill in function body here

		return res;
	}

	std::unique_ptr<HistoryReply>
		first_server::getHistory(const int &arg1, const int &arg2)
	{
		std::unique_ptr<HistoryReply> res(new HistoryReply);

		// Fill in function body here

		return res;
	}

	std::unique_ptr<DeltaReply>
		first_server::getDelta(const int &arg1, const uint256 &arg2)
	{
		std::unique_ptr<DeltaReply> res(new DeltaReply);

		// Fill in function body here

		return res;
	}

	std::unique_ptr<TransactionSetReply>
		first_server::getTxSet(const uint256 &arg)
	{
		std::unique_ptr<TransactionSetReply> res(new TransactionSetReply);

		// Fill in function body here

		return res;
	}

	std::unique_ptr<ValidationsReply>
		first_server::getValidations(const uint256 &arg)
	{
		std::unique_ptr<ValidationsReply> res(new ValidationsReply);

		// Fill in function body here

		return res;
	}

	std::unique_ptr<VoidReply>
		first_server::transaction(const Transaction &arg)
	{
		std::unique_ptr<VoidReply> res(new VoidReply);

		// Fill in function body here

		return res;
	}

	std::unique_ptr<QuorumSetReply>
		first_server::getQuorumSet(const uint256 &arg)
	{
		std::unique_ptr<QuorumSetReply> res(new QuorumSetReply);

		// Fill in function body here

		return res;
	}

	std::unique_ptr<VoidReply>
		first_server::prepareFBA(const PrepareEnvelope &arg)
	{
		std::unique_ptr<VoidReply> res(new VoidReply);

		// Fill in function body here

		return res;
	}

	std::unique_ptr<VoidReply>
		first_server::preparedFBA(const BallotEnvelope &arg)
	{
		std::unique_ptr<VoidReply> res(new VoidReply);

		// Fill in function body here

		return res;
	}

	std::unique_ptr<VoidReply>
		first_server::commitFBA(const BallotEnvelope &arg)
	{
		std::unique_ptr<VoidReply> res(new VoidReply);

		// Fill in function body here

		return res;
	}

	std::unique_ptr<VoidReply>
		first_server::commitedFBA(const BallotEnvelope &arg)
	{
		std::unique_ptr<VoidReply> res(new VoidReply);

		// Fill in function body here

		return res;
	}

}
