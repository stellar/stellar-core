// -*- C++ -*-
// Scaffolding originally generated from stellar.x.
// Edit to add functionality.

#ifndef __XDR_STELLAR_SERVER_HH_INCLUDED__
#define __XDR_STELLAR_SERVER_HH_INCLUDED__ 1

#include "stellar.hh"

namespace stellarxdr {

	class first_server {
	public:
		using rpc_interface_type = first;

		std::unique_ptr<int> hello(const int &arg);
		std::unique_ptr<PeersReply> getPeers();
		std::unique_ptr<HistoryReply> getHistory(const int &arg1, const int &arg2);
		std::unique_ptr<DeltaReply> getDelta(const int &arg1, const uint256 &arg2);
		std::unique_ptr<TransactionSetReply> getTxSet(const uint256 &arg);
		std::unique_ptr<ValidationsReply> getValidations(const uint256 &arg);
		std::unique_ptr<VoidReply> transaction(const Transaction &arg);
		std::unique_ptr<QuorumSetReply> getQuorumSet(const uint256 &arg);
		std::unique_ptr<VoidReply> prepareFBA(const PrepareEnvelope &arg);
		std::unique_ptr<VoidReply> preparedFBA(const BallotEnvelope &arg);
		std::unique_ptr<VoidReply> commitFBA(const BallotEnvelope &arg);
		std::unique_ptr<VoidReply> commitedFBA(const BallotEnvelope &arg);
	};

}
#endif
