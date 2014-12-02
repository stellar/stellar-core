#include "main/Application.h"
#include "lib/util/Logging.h"

_INITIALIZE_EASYLOGGINGPP

using namespace stellar;

int main(int argc, char* argv[])
{
    Application::pointer app = std::make_shared<Application>();
    app->mConfig.load(std::string("hayashi.cfg"));
    Logging::setUpLogging(app);
    
    LOG(INFO) << "Starting Hayashi...";

    app->start();
}

/*
add transaction

Admin commands:
stop
reload config
connect to peer
logrotate
peers
server_info
*/

/*
Left to figure out:
Transaction Format
Account Entry Format
Generic Storage
Generic Token


*/




