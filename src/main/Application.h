#ifndef __APPLICATION__
#define __APPLICATION__


#include "fba/FBAMaster.h"
#include "ledger/LedgerMaster.h"
#include "main/Config.h"
#include "txherder/TxHerder.h"
#include "overlay/OverlayGateway.h"
#include "overlay/PeerMaster.h"
#include "clf/CLFGateway.h"
/*
The state of the world and the main loop

*/

namespace stellar 
{
	class Application : public enable_shared_from_this<Application>
	{
        PeerMaster mPeerMaster;
		LedgerMaster mLedgerMaster;
		TxHerder mTxHerder;
		FBAMaster mFBAMaster;
	public:
        
        enum { BOOTING_STATE,   // loading last known ledger from disk
			CONNECTING_STATE,	// trying to connect to other peers 
			CONNECTED_STATE,	// connected to other peers and receiving validations
			CATCHING_UP_STATE,	// getting the current ledger from the network
			SYNCED_STATE,		// we are on the current ledger and are keeping up with deltas
			NUM_STATE   };
		
        int mState;
		Config mConfig;

        typedef std::shared_ptr<Application> pointer;

		Application();

		LedgerGateway& getLedgerGateway(){ return(mLedgerMaster); }
		FBAGateway& getFBAGateway(){ return(mFBAMaster); }
		CLFGateway& getCLFGateway();
		//HistoryGateway& getHistoryGateway();
		TxHerderGateway& getTxHerderGateway(){ return(mTxHerder); }
        OverlayGateway& getOverlayGateway();
        PeerMaster& getPeerMaster() { return(mPeerMaster); }

		void start();

	};

}

#endif
