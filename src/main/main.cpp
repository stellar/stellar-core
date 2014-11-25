#include "main/Application.h"
#include "lib/util/Logging.h"

_INITIALIZE_EASYLOGGINGPP

using namespace stellar;

void main(int argv, char* argc[])
{
    Logging::setUpLogging();
    
    LOG(INFO) << "Starting Hayashi...";

    gApp.start();
}




