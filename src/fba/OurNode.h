#ifndef __OURNODE__
#define __OURNODE__

#include <chrono>
#include "fba/Node.h"
#include "fba/Statement.h"
#include "fba/QuorumSet.h"

using namespace std;

namespace stellar
{
    class Application;
    typedef std::shared_ptr<Application> ApplicationPtr;

	/// we want to limit the amount of statements we spew out if we are running through a bunch of incoming statements
	class OurNode : public Node
	{
        ApplicationPtr mApp;
		chrono::system_clock::time_point mTimeSent[Statement::NUM_TYPES];

		Ballot::pointer mPreferredBallot;

		void sendNewPrepare(QuorumSet::pointer qset);


		void setPending(Statement::pointer pending);
		void progressPrepare(QuorumSet::pointer);
		void progressPrepared(QuorumSet::pointer);
		void progressCommit(QuorumSet::pointer);
		void progressCommitted(QuorumSet::pointer);

		void sendStatement(Statement::StatementType type, Ballot::pointer ballot);


		Ballot::pointer whatRatified(Statement::StatementType type);
		
		
	public:
		typedef std::shared_ptr<OurNode> pointer;

		OurNode(ApplicationPtr app);

		void startNewRound(Ballot::pointer firstBallot);

		void progressFBA();

	};

}

#endif