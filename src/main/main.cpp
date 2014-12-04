#include "main/Application.h"
#include "util/StellardVersion.h"
#include "lib/util/Logging.h"
#include "lib/util/getopt.h"

_INITIALIZE_EASYLOGGINGPP

enum opttag {
  OPT_VERSION = 0x100,
  OPT_HELP,
  OPT_TEST,
  OPT_CONF,
};

static const struct option stellard_options[] = {
    {"version", no_argument, nullptr, OPT_VERSION},
    {"help", no_argument, nullptr, OPT_HELP},
    {"test", no_argument, nullptr, OPT_TEST},
    {"conf", required_argument, nullptr, OPT_CONF},
    {nullptr, 0, nullptr, 0}
};

static void
usage(int err = 1)
{
    std::ostream &os = err ? cerr : cout;
    os << "usage: stellard [OPTIONS]\n"
        "where OPTIONS can be any of:\n"
        "      --help        To display this string\n"
        "      --version     To print version information\n"
        "      --test        To run self-tests\n"
        "      --conf FILE   To specify a config file (default stellard.cfg)\n";
    exit(err);
}


namespace stellar {
    int test();
}

int
main(int argc, char* argv[])
{
    using namespace stellar;

    std::string cfgFile("stellard.cfg");

    int opt;
    while ((opt = getopt_long_only(argc, argv, "", stellard_options,
                                     nullptr)) != -1)
    switch (opt) {
    case OPT_TEST:
        return test();
    case OPT_CONF:
        cfgFile = std::string(optarg);
        break;
    case OPT_VERSION:
        cout << STELLARD_VERSION;
        return 0;
    case OPT_HELP:
        usage(0);
        return 0;
    }

    Config cfg;
    cfg.load(cfgFile);
    Logging::setUpLogging(cfg.LOG_FILE_PATH);
    LOG(INFO) << "Starting stellard-hayashi " << STELLARD_VERSION;
    LOG(INFO) << "Config from " << cfgFile;
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




