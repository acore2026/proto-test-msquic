#include "app_common.h"
#include "lsquic_backend.h"
#include "msquic_backend.h"
#include "sctp_backend.h"

int main(int argc, char** argv) {
    std::signal(SIGINT, proto_test::SignalHandler);
    std::signal(SIGTERM, proto_test::SignalHandler);

    try {
        const auto args = proto_test::ParseArgs(argc, argv);
        if (args.values.count("help") != 0U) {
            proto_test::PrintUsage();
            return 0;
        }

        const auto config = proto_test::LoadConfig(args);
        if (config.protocol == proto_test::Protocol::Sctp) {
            if (config.mode == "server") {
                proto_test::RunSctpServer(config);
            } else {
                proto_test::RunSctpClient(config);
            }
        } else if (config.protocol == proto_test::Protocol::LsQuic) {
            proto_test::RunLsQuicEndpoint(config, config.mode == "server");
        } else {
            if (config.mode == "server") {
                proto_test::RunMsQuicServer(config);
            } else {
                proto_test::RunMsQuicClient(config);
            }
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        proto_test::PrintUsage();
        return 1;
    }
}
