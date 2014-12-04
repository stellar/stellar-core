#include "Application.h"
#include "overlay/PeerMaster.h"

namespace stellar
{

	Application::Application()
        mState(BOOTING_STATE),
        mPeerMaster(*this),
        mTxHerder(*this),
        mFBAMaster(*this),
	{
	}

	void Application::start()
	{

		// load last known ledger
		// connect to peers
		// listen for ledger close from the network
		// sync to current ledger

		mPeerMaster.start();
	}

    OverlayGateway& Application::getOverlayGateway() { return(mPeerMaster); }
}