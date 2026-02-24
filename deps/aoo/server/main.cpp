#include "aoo/aoo.h"
#include "aoo/aoo_server.hpp"

#include "common/udp_server.hpp"
#include "common/tcp_server.hpp"
#include "common/sync.hpp"

#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <thread>
#include <list>
#include <vector>

#ifdef _WIN32
# include <windows.h>
#else
# include <signal.h>
# include <stdio.h>
#endif

#ifndef AOO_DEFAULT_SERVER_PORT
# define AOO_DEFAULT_SERVER_PORT 7078
#endif

AooLogLevel g_loglevel = kAooLogLevelWarning;

void log_function(AooLogLevel level, const AooChar *msg) {
    if (level <= g_loglevel) {
        switch (level) {
        case kAooLogLevelDebug:
            std::cout << "[debug] ";
            break;
        case kAooLogLevelVerbose:
            std::cout << "[verbose] ";
            break;
        case kAooLogLevelWarning:
            std::cout << "[warning] ";
            break;
        case kAooLogLevelError:
            std::cout << "[error] ";
            break;
        default:
            break;
        }
        std::cout << msg << std::endl;
    }
}

AooServer::Ptr g_aoo_server;

int g_error = 0;
aoo::sync::semaphore g_semaphore;

void stop_server(int error) {
    g_error = error;
    g_semaphore.post();
}

// Support multiple server instances for Split Dual Stack
std::list<aoo::udp_server> g_udp_servers;
std::list<aoo::tcp_server> g_tcp_servers;

void handle_udp_receive(aoo::udp_server* server, int e, const aoo::ip_address& addr,
                        const AooByte *data, AooSize size) {
    // DIAGNOSTIC: Print everything to prove reception
    /*
    if (e == 0) {
         // std::cout << "RX UDP from: " << addr.name_unmapped() << " size: " << size << std::endl;
    }
    */

    if (g_loglevel >= kAooLogLevelVerbose) {
         // std::cout << "UDP received " << size << " bytes from " << addr << std::endl;
    }
    if (e == 0) {
        if (size >= 9 && !memcmp(data, "[SONOLOG]", 9)) {
            auto unmapped = addr.unmapped();
            std::cout << "[remote-log " << addr;
            if (unmapped != addr) {
                std::cout << " -> " << unmapped;
            }
            std::cout << "] " << std::string((const char *)data + 9, size - 9) << std::endl;
            return;
        }
        // Use the specific server instance to reply, ensuring correct Source IP
        g_aoo_server->handleUdpMessage(data, size, addr.address(), addr.length(),
            [](void *user, const AooByte *data, AooInt32 size,
                    const void *address, AooAddrSize addrlen, AooFlag) {
                auto* srv = static_cast<aoo::udp_server*>(user);
                aoo::ip_address addr((const struct sockaddr *)address, addrlen);
                if (g_loglevel >= kAooLogLevelVerbose) {
                     // std::cout << "UDP sending " << size << " bytes to " << addr << std::endl;
                }
                return srv->send(addr, data, size);
            }, server);
    } else {
        if (g_loglevel >= kAooLogLevelError)
            std::cout << "UDP server: recv() failed: " << aoo::socket_strerror(e) << std::endl;
        stop_server(e);
    }
}

AooId handle_tcp_accept(aoo::tcp_server* server, int e, const aoo::ip_address& addr) {
    if (g_loglevel >= kAooLogLevelVerbose) {
         std::cout << "TCP connection attempt from " << addr << std::endl;
    }
    if (e == 0) {
        // add new client
        AooId id;
        // Bind the specific TCP server instance for replies
        g_aoo_server->addClient([](void *user, AooId client, const AooByte *data, AooSize size) {
            auto* srv = static_cast<aoo::tcp_server*>(user);
            return srv->send(client, data, size);
        }, server, &id);
        if (g_loglevel >= kAooLogLevelVerbose) {
            std::cout << "Add new client " << id << std::endl;
        }
        return id;
    } else {
        // error
        if (g_loglevel >= kAooLogLevelError)
            std::cout << "TCP server: accept() failed: " << aoo::socket_strerror(e) << std::endl;
    #if 1
        stop_server(e);
    #endif
        return kAooIdInvalid;
    }
}

void handle_tcp_receive(aoo::tcp_server* server, int e, AooId client, const aoo::ip_address& addr,
                        const AooByte *data, AooSize size) {
    if (e == 0 && size > 0) {
        // handle client message
        if (auto err = g_aoo_server->handleClientMessage(client, data, size); err != kAooOk) {
            // remove misbehaving client
            g_aoo_server->removeClient(client);
            server->close(client);
            if (g_loglevel >= kAooLogLevelWarning)
                std::cout << "Close client " << client << " after error: " << aoo_strerror(err) << std::endl;
        }
    } else {
        // close client
        if (e != 0) {
            if (g_loglevel >= kAooLogLevelWarning)
                std::cout << "Close client after error: " << aoo::socket_strerror(e) << std::endl;
        } else {
            if (g_loglevel >= kAooLogLevelVerbose)
                std::cout << "Client " << client << " has disconnected" << std::endl;
        }
        g_aoo_server->removeClient(client);
    }
}

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD signal) {
    switch (signal) {
    case CTRL_C_EVENT:
        stop_server(0);
        return TRUE;
    case CTRL_CLOSE_EVENT:
        return TRUE;
    // Pass other signals to the next handler.
    default:
        return FALSE;
    }
}
#else
bool set_signal_handler(int sig, sig_t handler) {
    struct sigaction sa;
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(sig, &sa, nullptr) == 0) {
        return true;
    } else {
        perror("sigaction");
        return false;
    }
}

bool set_signal_handlers() {
    // NB: stop_server() is async-signal-safe!
    auto handler = [](int) { stop_server(0); };
    return set_signal_handler(SIGINT, handler)
           && set_signal_handler(SIGTERM, handler);
}
#endif

void print_usage() {
    std::cout
        << "Usage: aooserver [OPTIONS]...\n"
        << "Run an AOO server instance\n"
        << "Options:\n"
        << "  -h, --help             display help and exit\n"
        << "  -v, --version          print version and exit\n"
        << "  -p, --port             port number (default = " << AOO_DEFAULT_SERVER_PORT << ")\n"
        << "  -b, --bind             bind to specific address\n"
        << "  -r, --relay            enable server relay\n"
        << "  -l, --log-level=LEVEL  set log level\n"
        << std::endl;
}

bool check_arguments(const char **argv, int argc, int numargs) {
    if (argc > numargs) {
        return true;
    } else {
        std::cout << "Missing argument(s) for option '" << argv[0] << "'";
        return false;
    }
}

bool match_option(const char *str, const char *short_option, const char *long_option) {
    return (short_option && !strcmp(str, short_option))
           || (long_option && !strcmp(str, long_option));
}

int main(int argc, const char **argv) {
    // set control handler
#ifdef _WIN32
    if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
        std::cout << "Could not set console handler" << std::endl;
        return EXIT_FAILURE;
    }
#else
    if (!set_signal_handlers()) {
        return EXIT_FAILURE;
    }
#endif

    // parse command line options
    int port = AOO_DEFAULT_SERVER_PORT;
    std::string bind_addr_str;
    bool relay = false;

    argc--; argv++;

    try {
        while ((argc > 0) && (argv[0][0] == '-')) {
            if (match_option(argv[0], "-h", "--help")) {
                print_usage();
                return EXIT_SUCCESS;
            } else if (match_option(argv[0], "-v", "--version")) {
                std::cout << "aooserver " << aoo_getVersionString() << std::endl;
                return EXIT_SUCCESS;
            } else if (match_option(argv[0], "-p", "--port")) {
                if (!check_arguments(argv, argc, 1)) {
                    return EXIT_FAILURE;
                }
                port = std::stoi(argv[1]);
                if (port <= 0 || port > 65535) {
                    std::cout << "Port number " << port << " out of range" << std::endl;
                    return EXIT_FAILURE;
                }
                argc--; argv++;
            } else if (match_option(argv[0], "-b", "--bind")) {
                if (!check_arguments(argv, argc, 1)) {
                    return EXIT_FAILURE;
                }
                bind_addr_str = argv[1];
                argc--; argv++;
            } else if (match_option(argv[0], "-r", "--relay")) {
                relay = true;
            } else if (match_option(argv[0], "-l", "--log-level")) {
                if (!check_arguments(argv, argc, 1)) {
                    return EXIT_FAILURE;
                }
                g_loglevel = std::stoi(argv[1]);
                argc--; argv++;
            } else {
                std::cout << "Unknown command line option '" << argv[0] << "'" << std::endl;
                print_usage();
                return EXIT_FAILURE;
            }
            argc--; argv++;
        }
    } catch (const std::exception& e) {
        std::cout << "Bad argument for option '" << argv[0] << "'" << std::endl;
        return EXIT_FAILURE;
    }

    AooSettings settings;
    AooSettings_init(&settings);
    settings.logFunc = log_function;
    if (auto err = aoo_initialize(&settings); err != kAooOk) {
        std::cout << "Could not initialize AOO library: "
                  << aoo_strerror(err) << std::endl;
        return EXIT_FAILURE;
    }

    AooError err;
    g_aoo_server = AooServer::create(&err);
    if (!g_aoo_server) {
        std::cout << "Could not create AooServer: "
                  << aoo_strerror(err) << std::endl;
        return EXIT_FAILURE;
    }

    std::vector<aoo::ip_address> bind_addresses;

    if (!bind_addr_str.empty()) {
        aoo::ip_address addr(bind_addr_str, port);
        if (!addr.valid()) {
            std::cout << "Invalid bind address: " << bind_addr_str << std::endl;
            return EXIT_FAILURE;
        }
        
        bind_addresses.push_back(addr);

        // If specific IPv6 address (and not wildcard), also bind to IPv4 wildcard
        // to support "Split Dual Stack"
        if (addr.type() == aoo::ip_address::IPv6 && bind_addr_str != "::") {
             bind_addresses.push_back(aoo::ip_address("0.0.0.0", port));
             std::cout << "Enabling Split Dual Stack (Specific IPv6 + IPv4 Wildcard)" << std::endl;
        }

    } else {
        // Default wildcard IPv6 (Dual Stack if supported)
        bind_addresses.push_back(aoo::ip_address(port, aoo::ip_address::IPv6));
    }

    // setup UDP servers
    for (const auto& bind_addr : bind_addresses) {
        try {
            g_udp_servers.emplace_back();
            auto& server = g_udp_servers.back();
            
            // Bind handler with pointer to this specific server instance
            server.start(bind_addr, [&server](int e, const aoo::ip_address& addr, const AooByte *data, AooSize size) {
                handle_udp_receive(&server, e, addr, data, size);
            });
        } catch (const std::exception& e) {
            std::cout << "Could not start UDP server on " << bind_addr << ": " << e.what() << std::endl;
            return EXIT_FAILURE;
        }
    }

    // setup TCP servers
    for (const auto& bind_addr : bind_addresses) {
        try {
            g_tcp_servers.emplace_back();
            auto& server = g_tcp_servers.back();

            server.start(bind_addr, 
                [&server](int e, const aoo::ip_address& addr) {
                    return handle_tcp_accept(&server, e, addr);
                },
                [&server](int e, AooId client, const aoo::ip_address& addr, const AooByte *data, AooSize size) {
                    handle_tcp_receive(&server, e, client, addr, data, size);
                }
            );
        } catch (const std::exception& e) {
            std::cout << "Could not start TCP server on " << bind_addr << ": " << e.what() << std::endl;
            return EXIT_FAILURE;
        }
    }

    // setup AooServer
    // Use DualStack flag generally, as we want to support everything. 
    // The specific binding handles the restriction.
    AooSocketFlags flags = kAooSocketDualStack;
    
    if (auto err = g_aoo_server->setup(port, flags); err != kAooOk) {
        std::cout << "Could not setup AooServer: " << aoo_strerror(err) << std::endl;
        return EXIT_FAILURE;
    }

    g_aoo_server->setServerRelay(relay);

    std::vector<std::thread> threads;

    // start network threads
    for (auto& server : g_udp_servers) {
        threads.emplace_back([&server]() { server.run(); });
    }
    for (auto& server : g_tcp_servers) {
        threads.emplace_back([&server]() { server.run(); });
    }

    if (g_loglevel >= kAooLogLevelVerbose) {
        std::cout << "Server started." << std::endl;
        for (auto& server : g_udp_servers) {
            std::cout << "UDP server listening on " << server.address() << std::endl;
        }
        for (auto& server : g_tcp_servers) {
            std::cout << "TCP server listening on " << server.address() << std::endl;
        }
    }

    // fetch and display public IP addresses
    std::thread([port]() {
        auto fetch_url = [](const char *url) -> std::string {
            std::string result;
            std::string cmd = std::string("curl -s -m 3 ") + url;
#ifdef _WIN32
            FILE *fp = _popen(cmd.c_str(), "r");
#else
            FILE *fp = popen(cmd.c_str(), "r");
#endif
            if (fp) {
                char buffer[128];
                while (fgets(buffer, sizeof(buffer), fp) != nullptr) {
                    result += buffer;
                }
#ifdef _WIN32
                _pclose(fp);
#else
                pclose(fp);
#endif
                // trim whitespace
                const char *ws = " \t\n\r\f\v";
                result.erase(result.find_last_not_of(ws) + 1);
                result.erase(0, result.find_first_not_of(ws));
            }
            return result;
        };

        std::string v4 = fetch_url("https://4.icanhazip.com");
        if (v4.empty()) v4 = fetch_url("https://ifconfig.me/ip");
        if (v4.empty()) v4 = fetch_url("https://api.ipify.org");

        std::vector<std::pair<std::string, std::string>> v6_addrs;
#ifdef __APPLE__
        // On macOS, collect global IPv6 addresses from ifconfig
        FILE *fp = popen("ifconfig", "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp) != nullptr) {
                std::string s(line);
                if (s.find("inet6 ") != std::string::npos && 
                    s.find("fe80::") == std::string::npos) { // ignore link-local
                    
                    size_t start = s.find("inet6 ") + 6;
                    size_t end = s.find(" ", start);
                    if (start != std::string::npos && end != std::string::npos) {
                        std::string addr = s.substr(start, end - start);
                        std::string type = "global";
                        if (s.find(" secured") != std::string::npos) type = "secured";
                        if (s.find(" temporary") != std::string::npos) type = "temporary";
                        if (s.find(" deprecated") != std::string::npos) continue; // ignore deprecated
                        
                        v6_addrs.push_back({addr, type});
                    }
                }
            }
            pclose(fp);
        }
#endif
        // Fallback or non-macOS
        if (v6_addrs.empty()) {
             std::string v6 = fetch_url("https://6.icanhazip.com");
             if (v6.empty()) v6 = fetch_url("https://ifconfig.co/ip");
             if (!v6.empty()) v6_addrs.push_back({v6, "wan"});
        }

        if (!v4.empty()) {
            std::cout << "WAN IPv4: " << v4 << ":" << port << std::endl;
        }
        for (auto& pair : v6_addrs) {
            std::cout << "WAN IPv6 (" << pair.second << "): [" << pair.first << "]:" << port << std::endl;
        }
    }).detach();

    // wait for stop signal
    g_semaphore.wait();

    if (g_error == 0) {
        std::cout << "Program stopped by the user" << std::endl;
    } else {
        std::cout << "Program stopped because of an error: "
                  << aoo::socket_strerror(g_error) << std::endl;
    }

    // stop UDP and TCP server and exit
    for (auto& server : g_udp_servers) server.stop();
    for (auto& server : g_tcp_servers) server.stop();
    
    for (auto& thread : threads) {
        if (thread.joinable()) thread.join();
    }

    aoo_terminate();

    return EXIT_SUCCESS;
}
