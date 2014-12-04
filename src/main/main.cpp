#include "main/Application.h"
#include "lib/util/Logging.h"

_INITIALIZE_EASYLOGGINGPP

using namespace stellar;

int main(int argc, char* argv[])
{
    Config cfg;
    cfg.load("hayashi.cfg");
    Logging::setUpLogging(cfg.LOG_FILE_PATH);
    LOG(INFO) << "Starting Hayashi...";
    Application app(cfg);
    app.joinAllThreads();
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




