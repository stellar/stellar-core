#include "main/Application.h"
#include "generated/StellardVersion.h"
#include "util/Logging.h"
#include "lib/util/getopt.h"
#include "main/test.h"
#include "lib/http/HttpClient.h"

_INITIALIZE_EASYLOGGINGPP

enum opttag
{
    OPT_VERSION = 0x100,
    OPT_HELP,
    OPT_TEST,
    OPT_CONF,
    OPT_CMD,
};

static const struct option stellard_options[] = {
    {"version", no_argument, nullptr, OPT_VERSION},
    {"help", no_argument, nullptr, OPT_HELP},
    {"test", no_argument, nullptr, OPT_TEST},
    {"conf", required_argument, nullptr, OPT_CONF},
    {"c", required_argument, nullptr, OPT_CMD},
    {nullptr, 0, nullptr, 0}};

static void
usage(int err = 1)
{
    std::ostream& os = err ? cerr : cout;
    os << "usage: stellard [OPTIONS]\n"
          "where OPTIONS can be any of:\n"
          "      --help        To display this string\n"
          "      --version     To print version information\n"
          "      --test        To run self-tests\n"
          "      --c           Command to send to local hayashi\n"
          "                stop\n"
          "                info\n"
          "                reload_cfg?file=newconfig.cfg\n"
          "                logrotate\n"
          "                peers\n"
          "                connect?ip=5.5.5.5&port=3424\n"
          "                tx?tx_json='blah'\n"
          "      --conf FILE   To specify a config file (default "
          "stellard.cfg)\n";
    exit(err);
}

void
sendCommand(const std::string& command, const std::vector<char*>& rest,
            int port)
{
    std::string ret;
    std::ostringstream path;
    path << "/" << command;

    int code = http_request("127.0.0.1", path.str(), port, ret);
    if (code == 200)
    {
        LOG(INFO) << ret;
    }
    else
    {
        LOG(INFO) << "http failed(" << code << ") port: " << port
                  << " command: " << command;
    }
}

int
main(int argc, char* const* argv)
{
    using namespace stellar;

    std::string cfgFile("stellard.cfg");
    std::string command;
    std::vector<char*> rest;

    int opt;
    while ((opt = getopt_long_only(argc, argv, "", stellard_options,
                                   nullptr)) != -1)
    {
        switch (opt)
        {
        case OPT_TEST:
        {
            rest.push_back(*argv);
            rest.insert(++rest.begin(), argv + optind, argv + argc);
            return test(rest.size(), &rest[0]);
        }
        case OPT_CONF:
            cfgFile = std::string(optarg);
            break;
        case OPT_CMD:
            command = optarg;
            rest.insert(rest.begin(), argv + optind, argv + argc);
            break;
        case OPT_VERSION:
            cout << STELLARD_VERSION;
            return 0;
        case OPT_HELP:
            usage(0);
            return 0;
        }
    }

    Config cfg;
    cfg.load(cfgFile);
    Logging::setUpLogging(cfg.LOG_FILE_PATH);

    if (command.size())
    {
        sendCommand(command, rest, cfg.HTTP_PORT);
        return 0;
    }

    LOG(INFO) << "Starting stellard-hayashi " << STELLARD_VERSION;
    LOG(INFO) << "Config from " << cfgFile;
    Application app(cfg);

    auto& io = app.getMainIOService();
    asio::io_service::work mainWork(io);
    while (!io.stopped())
    {
        io.run();
    }
}
