#include "Application.h"
#include "overlay/PeerMaster.h"

namespace stellar
{
    // SANITY get rid of global so we can simulate many nodes in one process
    Application gApp;

	Application::Application()
	{
		mState = BOOTING_STATE;
	}

	void Application::start()
	{
		// load last known ledger
		// connect to peers
		// listen for ledger close from the network
		// sync to current ledger

		gPeerMaster.start();

		
	}

    OverlayGateway& Application::getOverlayGateway() { return(gPeerMaster); }
}