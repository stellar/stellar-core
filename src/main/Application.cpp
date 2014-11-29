#include "Application.h"
#include "overlay/PeerMaster.h"

namespace stellar
{

	Application::Application()
	{
		mState = BOOTING_STATE;
	}

	void Application::start()
	{
        mFBAMaster.setApplication(shared_from_this());
        mTxHerder.setApplication(shared_from_this());
        mPeerMaster.setApplication(shared_from_this());

		// load last known ledger
		// connect to peers
		// listen for ledger close from the network
		// sync to current ledger

		mPeerMaster.start();
	}

    OverlayGateway& Application::getOverlayGateway() { return(mPeerMaster); }
}