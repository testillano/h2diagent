/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
https://github.com/testillano/h2diagent
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

#include <csignal>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <thread>

#include <boost/asio.hpp>

#include <ert/tracing/Logger.hpp>
#include <ert/h2diagent/Gateway.hpp>

const char* progname = "h2diagent";
const char* version = "0.0.1";

namespace
{

void printBanner() {
    std::cout << R"(
 _     ____     _ _
| |__ |___ \ __| (_) __ _  __ _  ___ _ __ | |_
| '_ \  __) / _` | |/ _` |/ _` |/ _ \ '_ \| __|
| | | |/ __/ (_| | | (_| | (_| |  __/ | | | |_
|_| |_|_____\__,_|_|\__,_|\__, |\___|_| |_|\__|
                           |___/

https://github.com/testillano/h2diagent

)" << std::endl;
}

void usage(const char* name) {
    std::cout << name << " - C++ HTTP/2 - DIAMETER Gateway Service (translation agent)\n\n"
              << "Usage: " << name << " [options]\n\n"
              << "Options:\n\n"
              << "Diameter:\n"
              << "  --diameter-port <port>          Diameter listen port (default: 3868)\n"
              << "  --diameter-peer-host <host>     Remote Diameter peer host (for outbound)\n"
              << "  --diameter-peer-port <port>     Remote Diameter peer port (default: 3868)\n"
              << "  --origin-host <identity>        Origin-Host for CER (default: hostname)\n"
              << "  --origin-realm <realm>          Origin-Realm for CER\n"
              << "  --product-name <name>           Product-Name for CER (default: h2diagent)\n"
              << "  --dictionary <path>             Diameter dictionary JSON file path\n"
              << "  --watchdog-interval <seconds>   DWR interval (default: 30)\n\n"
              << "HTTP/2 (towards h2agent):\n"
              << "  --h2agent-host <host>           h2agent traffic server host (default: localhost)\n"
              << "  --h2agent-port <port>           h2agent traffic server port (default: 8000)\n\n"
              << "HTTP/2 (for outbound from h2agent):\n"
              << "  --http2-server-port <port>      HTTP/2 listen port for h2agent client (default: 8080)\n\n"
              << "General:\n"
              << "  --workers <n>                   Worker threads (default: nproc)\n"
              << "  [-l|--log-level <level>]        Log level: Debug|Informational|Notice|Warning|Error (default: Warning)\n"
              << "  [-v|--verbose]                  Output log traces on console\n"
              << "  --admin-port <port>             Admin API port (default: 8074)\n"
              << "  --prometheus-port <port>        Prometheus scrape port (default: 9090)\n"
              << "  --disable-metrics               Disable prometheus metrics\n\n"
              << "  [-V|--version]                  Program version\n"
              << "  [-h|--help]                     This help\n"
              << std::endl;
}

} // anonymous namespace

// Global for signal handler
static boost::asio::io_context* g_io = nullptr;

void signalHandler(int signum) {
    std::cerr << "\nSignal received: " << signum << std::endl;
    if (g_io) g_io->stop();
}

int main(int argc, char* argv[]) {

    ert::h2diagent::GatewayConfig config;
    bool verbose = false;
    std::string logLevel = "Warning";

    // Default origin-host to hostname
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    config.originHost = hostname;

    static struct option long_options[] = {
        {"diameter-port",       required_argument, nullptr, 0},
        {"diameter-peer-host",  required_argument, nullptr, 0},
        {"diameter-peer-port",  required_argument, nullptr, 0},
        {"origin-host",         required_argument, nullptr, 0},
        {"origin-realm",        required_argument, nullptr, 0},
        {"product-name",        required_argument, nullptr, 0},
        {"dictionary",          required_argument, nullptr, 0},
        {"watchdog-interval",   required_argument, nullptr, 0},
        {"h2agent-host",        required_argument, nullptr, 0},
        {"h2agent-port",        required_argument, nullptr, 0},
        {"http2-server-port",   required_argument, nullptr, 0},
        {"workers",             required_argument, nullptr, 0},
        {"log-level",           required_argument, nullptr, 'l'},
        {"verbose",             no_argument,       nullptr, 'v'},
        {"admin-port",          required_argument, nullptr, 0},
        {"prometheus-port",     required_argument, nullptr, 0},
        {"disable-metrics",     no_argument,       nullptr, 0},
        {"version",             no_argument,       nullptr, 'V'},
        {"help",                no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "l:vVh", long_options, &option_index)) != -1) {
        switch (opt) {
            case 0: {
                std::string name(long_options[option_index].name);
                if (name == "diameter-port") config.diameterPort = std::stoi(optarg);
                else if (name == "diameter-peer-host") config.diameterPeerHost = optarg;
                else if (name == "diameter-peer-port") config.diameterPeerPort = std::stoi(optarg);
                else if (name == "origin-host") config.originHost = optarg;
                else if (name == "origin-realm") config.originRealm = optarg;
                else if (name == "product-name") config.productName = optarg;
                else if (name == "dictionary") config.dictionaryPath = optarg;
                else if (name == "watchdog-interval") config.watchdogIntervalSec = std::stoi(optarg);
                else if (name == "h2agent-host") config.h2agentHost = optarg;
                else if (name == "h2agent-port") config.h2agentPort = std::stoi(optarg);
                else if (name == "http2-server-port") config.http2ServerPort = std::stoi(optarg);
                else if (name == "workers") config.workers = std::stoi(optarg);
                else if (name == "admin-port") config.adminPort = std::stoi(optarg);
                else if (name == "prometheus-port") config.prometheusPort = std::stoi(optarg);
                else if (name == "disable-metrics") config.metricsEnabled = false;
                break;
            }
            case 'l': logLevel = optarg; break;
            case 'v': verbose = true; break;
            case 'V': std::cout << progname << " " << version << std::endl; return 0;
            case 'h': usage(progname); return 0;
            default: usage(progname); return 1;
        }
    }

    // Initialize logger
    ert::tracing::Logger::initialize(progname);
    ert::tracing::Logger::verbose(verbose);

    if (logLevel == "Debug") ert::tracing::Logger::setLevel(ert::tracing::Logger::Debug);
    else if (logLevel == "Informational") ert::tracing::Logger::setLevel(ert::tracing::Logger::Informational);
    else if (logLevel == "Notice") ert::tracing::Logger::setLevel(ert::tracing::Logger::Notice);
    else if (logLevel == "Warning") ert::tracing::Logger::setLevel(ert::tracing::Logger::Warning);
    else if (logLevel == "Error") ert::tracing::Logger::setLevel(ert::tracing::Logger::Error);

    printBanner();

    std::cout << "Starting " << progname << std::endl;
    std::cout << "Log level: " << logLevel << std::endl;
    std::cout << "Verbose (stdout): " << (verbose ? "true" : "false") << std::endl;
    std::cout << "Diameter listen port: " << config.diameterPort << std::endl;
    if (!config.diameterPeerHost.empty())
        std::cout << "Diameter peer: " << config.diameterPeerHost << ":" << config.diameterPeerPort << std::endl;
    std::cout << "Origin-Host: " << config.originHost << std::endl;
    std::cout << "Origin-Realm: " << config.originRealm << std::endl;
    std::cout << "Dictionary: " << (config.dictionaryPath.empty() ? "<not provided>" : config.dictionaryPath) << std::endl;
    std::cout << "H2agent: " << config.h2agentHost << ":" << config.h2agentPort << std::endl;
    std::cout << "HTTP/2 server port (outbound): " << config.http2ServerPort << std::endl;
    std::cout << "Admin port: " << config.adminPort << std::endl;
    std::cout << "Prometheus port: " << (config.metricsEnabled ? std::to_string(config.prometheusPort) : "disabled") << std::endl;
    std::cout << std::endl;

    // Workers
    int workers = config.workers > 0 ? config.workers : std::thread::hardware_concurrency();

    // io_context
    boost::asio::io_context io(workers);
    g_io = &io;

    // Signal handling
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGINT, signalHandler);

    // Create and start gateway
    ert::h2diagent::Gateway gateway(io, config);
    gateway.start();

    // Run io_context with worker threads
    std::vector<std::thread> threads;
    for (int i = 1; i < workers; ++i) {
        threads.emplace_back([&io]() { io.run(); });
    }
    io.run(); // main thread also runs

    // Wait for threads
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    gateway.stop();

    std::cout << "\n" << progname << " terminated." << std::endl;
    return 0;
}
