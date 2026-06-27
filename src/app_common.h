#pragma once

#ifdef HAVE_MSQUIC
#include <msquic.h>
#endif
#ifdef HAVE_LSQUIC
#include <lsquic.h>
#endif

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <csignal>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <fcntl.h>
#include <limits>
#include <linux/sctp.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <map>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <poll.h>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace proto_test {
using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

constexpr uint32_t kMessageMagic = 0x4D53514C; // MSQL
constexpr char kDefaultAlpn[] = "msquic-load";

enum class Protocol {
    MsQuic,
    LsQuic,
    Sctp,
};

inline std::string ProtocolName(Protocol protocol) {
    switch (protocol) {
    case Protocol::MsQuic:
        return "msquic";
    case Protocol::LsQuic:
        return "lsquic";
    case Protocol::Sctp:
        return "sctp";
    }
    return "unknown";
}

inline Protocol ParseProtocol(const std::string& value) {
    if (value == "msquic") {
        return Protocol::MsQuic;
    }
    if (value == "lsquic") {
        return Protocol::LsQuic;
    }
    if (value == "sctp") {
        return Protocol::Sctp;
    }
    throw std::runtime_error("unsupported protocol: " + value + " (expected msquic, lsquic, or sctp)");
}

inline std::atomic<bool> g_stop_requested{false};

inline void SignalHandler(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

inline uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<Nanoseconds>(Clock::now().time_since_epoch()).count());
}

inline uint64_t RealtimeNs() {
    timespec ts{};
    if (::clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        throw std::runtime_error(std::string("clock_gettime(CLOCK_REALTIME) failed: ") + std::strerror(errno));
    }
    return (static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL) + static_cast<uint64_t>(ts.tv_nsec);
}

#ifdef HAVE_MSQUIC
inline std::string StatusToHex(QUIC_STATUS status) {
    std::ostringstream stream;
    stream << "0x" << std::hex << static_cast<uint32_t>(status);
    return stream.str();
}
#endif

#ifndef OPENSSL_NO_SCTP
inline std::string ReadTextFile(const char* path) {
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return "";
    }
    char buffer[128] {};
    const size_t bytes = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    return std::string(buffer, bytes);
}

inline bool LinuxSctpAuthEnabled() {
    const std::string value = ReadTextFile("/proc/sys/net/sctp/auth_enable");
    return !value.empty() && value[0] == '1';
}
#endif

inline sockaddr_in ResolveIpv4Address(const std::string& host, uint16_t port, const char* context) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1) {
        return address;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_SCTP;

    addrinfo* results = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &results);
    if (rc != 0) {
        throw std::runtime_error(std::string(context) + " failed to resolve IPv4 address: " + host + ": " +
                                 gai_strerror(rc));
    }

    for (addrinfo* current = results; current != nullptr; current = current->ai_next) {
        if (current->ai_family == AF_INET && current->ai_addrlen >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
            address = *reinterpret_cast<sockaddr_in*>(current->ai_addr);
            address.sin_port = htons(port);
            ::freeaddrinfo(results);
            return address;
        }
    }

    ::freeaddrinfo(results);
    throw std::runtime_error(std::string(context) + " did not resolve to an IPv4 address: " + host);
}

template <typename T>
T ParseNumber(const std::string& text, const char* name) {
    try {
        if constexpr (std::is_same_v<T, uint16_t>) {
            const auto value = std::stoul(text);
            if (value > std::numeric_limits<uint16_t>::max()) {
                throw std::out_of_range("uint16_t overflow");
            }
            return static_cast<uint16_t>(value);
        } else if constexpr (std::is_same_v<T, uint32_t>) {
            const auto value = std::stoull(text);
            if (value > std::numeric_limits<uint32_t>::max()) {
                throw std::out_of_range("uint32_t overflow");
            }
            return static_cast<uint32_t>(value);
        } else if constexpr (std::is_same_v<T, uint64_t>) {
            return std::stoull(text);
        } else if constexpr (std::is_same_v<T, int>) {
            return std::stoi(text);
        } else {
            static_assert(sizeof(T) == 0, "Unsupported numeric type");
        }
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("invalid value for ") + name + ": " + text);
    }
}

struct Args {
    std::string mode;
    std::map<std::string, std::string> values;
};

inline Args ParseArgs(int argc, char** argv) {
    if (argc < 2) {
        throw std::runtime_error("usage: msquic-loadtest <server|client> [--key=value]");
    }

    Args args;
    args.mode = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string token = argv[i];
        if (token == "--help" || token == "-h") {
            args.values["help"] = "1";
            continue;
        }
        if (token.rfind("--", 0) != 0) {
            throw std::runtime_error("expected --key=value, got: " + token);
        }
        token.erase(0, 2);
        const auto eq = token.find('=');
        if (eq == std::string::npos) {
            args.values[token] = "1";
        } else {
            args.values[token.substr(0, eq)] = token.substr(eq + 1);
        }
    }
    return args;
}

inline std::optional<std::string> FindArg(const Args& args, const std::string& key) {
    const auto it = args.values.find(key);
    if (it == args.values.end()) {
        return std::nullopt;
    }
    return it->second;
}

inline bool GetBool(const Args& args, const std::string& key, bool default_value) {
    const auto value = FindArg(args, key);
    if (!value.has_value()) {
        return default_value;
    }
    if (*value == "1" || *value == "true" || *value == "yes") {
        return true;
    }
    if (*value == "0" || *value == "false" || *value == "no") {
        return false;
    }
    throw std::runtime_error("invalid boolean for --" + key + ": " + *value);
}

inline std::string GetRequired(const Args& args, const std::string& key) {
    const auto value = FindArg(args, key);
    if (!value.has_value()) {
        throw std::runtime_error("missing required argument --" + key);
    }
    return *value;
}

template <typename T>
T GetNumber(const Args& args, const std::string& key, T default_value) {
    const auto value = FindArg(args, key);
    if (!value.has_value()) {
        return default_value;
    }
    return ParseNumber<T>(*value, key.c_str());
}

inline std::string GetString(const Args& args, const std::string& key, const std::string& default_value) {
    const auto value = FindArg(args, key);
    return value.has_value() ? *value : default_value;
}

inline void PrintUsage() {
    std::cout
        << "Usage:\n"
        << "  msquic-loadtest server [options]\n"
        << "  msquic-loadtest client --target=HOST [options]\n\n"
        << "Common options:\n"
        << "  --protocol=msquic|lsquic|sctp Transport protocol, default msquic\n"
        << "  --base-port=PORT           First port, default 15443\n"
        << "  --server-count=N           Number of listeners, default 1\n"
        << "  --stream-count=N           Streams per connection/association, default 1\n"
        << "  --message-size=BYTES       Fixed frame size, default 1024\n"
        << "  --idle-timeout-ms=N        Idle timeout, default 30000\n"
        << "  --stats-interval-ms=N      Stats interval, default 1000\n"
        << "  --qualcomm-method=1        Enable single-client stack timing trace mode\n"
        << "  --message-count=N          Messages to send in Qualcomm mode, default 30000\n"
        << "  --qualcomm-gap-ms=N        Gap between Qualcomm-mode message cycles, default 0\n"
        << "  --trace-file=FILE          App timestamp CSV for Qualcomm mode\n\n"
        << "Server options:\n"
        << "  --cert=FILE                PEM certificate (msquic, or sctp with --sctp-tls=1)\n"
        << "  --key=FILE                 PEM private key (msquic, or sctp with --sctp-tls=1)\n"
        << "  --password=TEXT            Optional private key password\n"
        << "  --bind=ADDR                Listener bind address, default 0.0.0.0\n"
        << "  --alpn=TEXT                ALPN, default msquic-load\n"
        << "  --sctp-nodelay=1           Disable SCTP Nagle-like bundling, default enabled\n"
        << "  --sctp-stream-id=N         SCTP stream id, default 0\n"
        << "  --sctp-tls=1               Enable DTLS-over-SCTP, default disabled\n"
        << "  --stream-profile=SPEC      Per-stream profiles name:size:pps:max_inflight[:count]\n"
        << "  --ca=FILE                  CA bundle for peer verification\n\n"
        << "Client options:\n"
        << "  --target=HOST              Server name or IP\n"
        << "  --clients=N                Number of connections, default 1\n"
        << "  --max-inflight=N           Max outstanding echoed messages per connection, default 64\n"
        << "  --send-server-index=N      Only send on connections mapped to this server index, default all\n"
        << "  --send-pps=N               Total offered message rate across all clients, default unlimited\n"
        << "  --send-pps-per-client=N    Offered message rate per sending client connection, default disabled\n"
        << "  --duration-sec=N           Active send duration, default 30\n"
        << "  --drain-timeout-ms=N       Drain time after send stop, default 5000\n"
        << "  --verify-peer=1            Enable certificate validation, default disabled\n"
        << "  --alpn=TEXT                ALPN, default msquic-load\n"
        << "  --sctp-nodelay=1           Disable SCTP Nagle-like bundling, default enabled\n"
        << "  --sctp-stream-id=N         SCTP stream id, default 0\n"
        << "  --sctp-tls=1               Enable DTLS-over-SCTP, default disabled\n"
        << "  --stream-profile=SPEC      Per-stream profiles name:size:pps:max_inflight[:count]\n"
        << "  --ca=FILE                  CA bundle for peer verification\n";
}

struct AppConfig {
    struct StreamProfile {
        std::string name;
        uint32_t message_size{1024};
        uint32_t max_inflight{64};
        uint64_t send_pps{0};
    };

    std::string mode;
    Protocol protocol{Protocol::MsQuic};
    std::string alpn{kDefaultAlpn};
    uint16_t base_port{15443};
    uint32_t server_count{1};
    uint32_t stream_count{1};
    uint32_t message_size{1024};
    uint64_t idle_timeout_ms{30000};
    uint64_t stats_interval_ms{1000};

    std::string bind{"0.0.0.0"};
    std::string cert_file;
    std::string key_file;
    std::string password;

    std::string target{"127.0.0.1"};
    uint32_t client_count{1};
    uint32_t max_inflight{64};
    int send_server_index{-1};
    uint64_t send_pps{0};
    uint64_t send_pps_per_client{0};
    uint64_t duration_sec{30};
    uint64_t drain_timeout_ms{5000};
    bool verify_peer{false};
    bool sctp_nodelay{true};
    uint16_t sctp_stream_id{0};
    bool sctp_tls{false};
    std::string ca_file;
    std::vector<StreamProfile> stream_profiles;
    bool qualcomm_method{false};
    uint64_t message_count{30000};
    uint64_t qualcomm_gap_ms{0};
    std::string trace_file;
};

inline bool HasStreamProfilePacing(const AppConfig& config) {
    for (const auto& profile : config.stream_profiles) {
        if (profile.send_pps > 0) {
            return true;
        }
    }
    return false;
}

inline std::vector<std::string> SplitString(const std::string& input, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(input);
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        parts.push_back(part);
    }
    return parts;
}

inline std::vector<AppConfig::StreamProfile> ParseStreamProfiles(const std::string& value) {
    std::vector<AppConfig::StreamProfile> profiles;
    for (const auto& raw_entry : SplitString(value, ',')) {
        if (raw_entry.empty()) {
            continue;
        }
        const auto fields = SplitString(raw_entry, ':');
        if (fields.size() < 4 || fields.size() > 5) {
            throw std::runtime_error(
                "invalid --stream-profile entry '" + raw_entry +
                "' (expected name:message_size:send_pps:max_inflight[:count])");
        }
        const std::string& name = fields[0];
        const uint32_t message_size = ParseNumber<uint32_t>(fields[1], "stream-profile message_size");
        const uint64_t send_pps = ParseNumber<uint64_t>(fields[2], "stream-profile send_pps");
        const uint32_t max_inflight = ParseNumber<uint32_t>(fields[3], "stream-profile max_inflight");
        const uint32_t count =
            fields.size() == 5 ? ParseNumber<uint32_t>(fields[4], "stream-profile count") : 1;
        if (message_size < 24) {
            throw std::runtime_error("stream-profile message_size must be at least 24 bytes");
        }
        if (max_inflight == 0) {
            throw std::runtime_error("stream-profile max_inflight must be >= 1");
        }
        if (count == 0) {
            throw std::runtime_error("stream-profile count must be >= 1");
        }
        for (uint32_t i = 0; i < count; ++i) {
            profiles.push_back(AppConfig::StreamProfile{
                count == 1 ? name : name + "-" + std::to_string(i),
                message_size,
                max_inflight,
                send_pps,
            });
        }
    }
    return profiles;
}

inline AppConfig LoadConfig(const Args& args) {
    AppConfig config;
    config.mode = args.mode;
    config.qualcomm_method = GetBool(args, "qualcomm-method", false);
    config.protocol = ParseProtocol(GetString(args, "protocol", "msquic"));
    config.alpn = GetString(args, "alpn", kDefaultAlpn);
    config.base_port = GetNumber<uint16_t>(args, "base-port", 15443);
    config.server_count = GetNumber<uint32_t>(args, "server-count", 1);
    config.stream_count = GetNumber<uint32_t>(args, "stream-count", 1);
    config.message_size = GetNumber<uint32_t>(args, "message-size", config.qualcomm_method ? 50U : 1024U);
    config.idle_timeout_ms = GetNumber<uint64_t>(args, "idle-timeout-ms", 30000);
    config.stats_interval_ms = GetNumber<uint64_t>(args, "stats-interval-ms", config.qualcomm_method ? 0U : 1000U);
    config.sctp_nodelay = GetBool(args, "sctp-nodelay", true);
    config.sctp_stream_id = GetNumber<uint16_t>(args, "sctp-stream-id", 0);
    config.sctp_tls = GetBool(args, "sctp-tls", false);
    config.ca_file = GetString(args, "ca", "");
    config.message_count = GetNumber<uint64_t>(args, "message-count", 30000);
    config.qualcomm_gap_ms = GetNumber<uint64_t>(args, "qualcomm-gap-ms", 0);
    config.trace_file = GetString(args, "trace-file", "");
    const auto stream_profile_arg = FindArg(args, "stream-profile");
    if (stream_profile_arg.has_value()) {
        config.stream_profiles = ParseStreamProfiles(*stream_profile_arg);
        config.stream_count = static_cast<uint32_t>(config.stream_profiles.size());
    }

    if (config.message_size < 24) {
        throw std::runtime_error("--message-size must be at least 24 bytes");
    }
    if (config.server_count == 0) {
        throw std::runtime_error("--server-count must be >= 1");
    }
    if (config.stream_count == 0) {
        throw std::runtime_error("--stream-count must be >= 1");
    }
    if (config.protocol == Protocol::Sctp && config.sctp_tls && config.stream_count > 1) {
        throw std::runtime_error("SCTP multi-stream is not supported with --sctp-tls=1 yet");
    }
    if (config.qualcomm_method) {
        if (config.server_count != 1) {
            throw std::runtime_error("--qualcomm-method=1 requires --server-count=1");
        }
        if (config.stream_count != 1) {
            throw std::runtime_error("--qualcomm-method=1 requires --stream-count=1");
        }
        if (config.message_size != 50) {
            throw std::runtime_error("--qualcomm-method=1 requires --message-size=50");
        }
        if (config.message_count == 0) {
            throw std::runtime_error("--message-count must be >= 1 in Qualcomm mode");
        }
        if (!config.stream_profiles.empty()) {
            throw std::runtime_error("--qualcomm-method=1 cannot be combined with --stream-profile");
        }
        if (config.trace_file.empty()) {
            throw std::runtime_error("--qualcomm-method=1 requires --trace-file=FILE");
        }
    }

    if (config.mode == "server") {
        config.bind = GetString(args, "bind", "0.0.0.0");
        if (config.protocol == Protocol::MsQuic || config.protocol == Protocol::LsQuic) {
            config.cert_file = GetRequired(args, "cert");
            config.key_file = GetRequired(args, "key");
            config.password = GetString(args, "password", "");
        } else if (config.sctp_tls) {
            config.cert_file = GetRequired(args, "cert");
            config.key_file = GetRequired(args, "key");
            config.password = GetString(args, "password", "");
        }
    } else if (config.mode == "client") {
        config.target = GetRequired(args, "target");
        config.client_count = GetNumber<uint32_t>(args, "clients", 1);
        config.max_inflight = GetNumber<uint32_t>(args, "max-inflight", 64);
        config.send_server_index = GetNumber<int>(args, "send-server-index", -1);
        config.send_pps = GetNumber<uint64_t>(args, "send-pps", 0);
        config.send_pps_per_client = GetNumber<uint64_t>(args, "send-pps-per-client", 0);
        config.duration_sec = GetNumber<uint64_t>(args, "duration-sec", 30);
        config.drain_timeout_ms = GetNumber<uint64_t>(args, "drain-timeout-ms", 5000);
        config.verify_peer = GetBool(args, "verify-peer", false);
        if (config.client_count == 0) {
            throw std::runtime_error("--clients must be >= 1");
        }
        if (config.max_inflight == 0) {
            throw std::runtime_error("--max-inflight must be >= 1");
        }
        if (config.send_server_index < -1) {
            throw std::runtime_error("--send-server-index must be >= -1");
        }
        if (config.send_server_index >= static_cast<int>(config.server_count)) {
            throw std::runtime_error("--send-server-index must be in [0, server-count-1]");
        }
        if (config.send_pps > 0 && config.send_pps_per_client > 0) {
            throw std::runtime_error("use only one of --send-pps or --send-pps-per-client");
        }
        if (!config.stream_profiles.empty() && (config.send_pps > 0 || config.send_pps_per_client > 0)) {
            throw std::runtime_error("use --stream-profile send rates instead of --send-pps/--send-pps-per-client");
        }
        if (config.qualcomm_method) {
            if (config.client_count != 1) {
                throw std::runtime_error("--qualcomm-method=1 requires --clients=1");
            }
            if (config.max_inflight != 1) {
                throw std::runtime_error("--qualcomm-method=1 requires --max-inflight=1");
            }
            if (config.send_pps != 0 || config.send_pps_per_client != 0) {
                throw std::runtime_error("--qualcomm-method=1 bypasses app pacing; do not set --send-pps or --send-pps-per-client");
            }
        }
    } else {
        throw std::runtime_error("mode must be 'server' or 'client'");
    }

    return config;
}

inline uint32_t ActiveSendConnectionCount(const AppConfig& config) {
    if (config.client_count == 0) {
        return 0;
    }
    if (config.send_server_index < 0) {
        return config.client_count;
    }

    uint32_t count = 0;
    for (uint32_t i = 0; i < config.client_count; ++i) {
        if ((i % config.server_count) == static_cast<uint32_t>(config.send_server_index)) {
            ++count;
        }
    }
    return count;
}

#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic;
    uint32_t reserved;
    uint64_t sequence;
    uint64_t send_timestamp_ns;
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 24, "Unexpected wire header size");

inline uint32_t FrameSizeFromHeader(const MessageHeader& header, uint32_t fallback_size) {
    if (header.reserved >= sizeof(MessageHeader)) {
        return header.reserved;
    }
    return fallback_size;
}

inline std::optional<uint32_t> TryPeekFrameSize(const std::vector<uint8_t>& buffer, uint32_t fallback_size) {
    if (buffer.size() < sizeof(MessageHeader)) {
        return std::nullopt;
    }
    MessageHeader header{};
    std::memcpy(&header, buffer.data(), sizeof(header));
    if (header.magic != kMessageMagic) {
        throw std::runtime_error("invalid frame magic");
    }
    const uint32_t frame_size = FrameSizeFromHeader(header, fallback_size);
    if (frame_size < sizeof(MessageHeader)) {
        throw std::runtime_error("invalid frame size");
    }
    if (buffer.size() < frame_size) {
        return std::nullopt;
    }
    return frame_size;
}

inline AppConfig::StreamProfile DefaultStreamProfile(const AppConfig& config) {
    return AppConfig::StreamProfile{
        "default",
        config.message_size,
        config.max_inflight,
        config.send_pps_per_client,
    };
}

inline AppConfig::StreamProfile StreamProfileForOrdinal(const AppConfig& config, uint32_t ordinal) {
    if (config.stream_profiles.empty()) {
        return DefaultStreamProfile(config);
    }
    return config.stream_profiles.at(ordinal);
}

constexpr size_t kQualcommHopOffset = sizeof(MessageHeader);
constexpr uint8_t kQualcommHopInitial = 0;
constexpr uint8_t kQualcommHopReflected = 1;
constexpr uint8_t kQualcommHopFinal = 2;

inline uint8_t QualcommHop(const std::vector<uint8_t>& frame) {
    return frame.size() > kQualcommHopOffset ? frame[kQualcommHopOffset] : 0xff;
}

inline void SetQualcommHop(std::vector<uint8_t>& frame, uint8_t hop) {
    if (frame.size() <= kQualcommHopOffset) {
        throw std::runtime_error("Qualcomm mode frame is too small for hop marker");
    }
    frame[kQualcommHopOffset] = hop;
}

class QualcommTraceWriter {
  public:
    QualcommTraceWriter(const AppConfig& config, std::string role)
        : enabled_(config.qualcomm_method),
          role_(std::move(role)),
          protocol_(ProtocolName(config.protocol)) {
        if (!enabled_) {
            return;
        }
        output_.open(config.trace_file, std::ios::out | std::ios::trunc);
        if (!output_) {
            throw std::runtime_error("failed to open trace file: " + config.trace_file);
        }
        output_ << "event,role,protocol,sequence,message_size,hop_in,hop_out,"
                << "app_entry_realtime_ns,app_exit_realtime_ns,app_entry_mono_ns,app_exit_mono_ns\n";
    }

    void Log(
        const char* event,
        uint64_t sequence,
        uint32_t message_size,
        uint8_t hop_in,
        uint8_t hop_out,
        uint64_t entry_realtime_ns,
        uint64_t exit_realtime_ns,
        uint64_t entry_mono_ns,
        uint64_t exit_mono_ns) {
        if (!enabled_) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        output_ << event << ','
                << role_ << ','
                << protocol_ << ','
                << sequence << ','
                << message_size << ','
                << static_cast<uint32_t>(hop_in) << ','
                << static_cast<uint32_t>(hop_out) << ','
                << entry_realtime_ns << ','
                << exit_realtime_ns << ','
                << entry_mono_ns << ','
                << exit_mono_ns << '\n';
        output_.flush();
    }

  private:
    bool enabled_{false};
    std::string role_;
    std::string protocol_;
    std::mutex mutex_;
    std::ofstream output_;
};

#ifdef HAVE_MSQUIC
struct SendBuffer {
    QUIC_BUFFER quic_buffer{};
    std::vector<uint8_t> storage;

    explicit SendBuffer(size_t size) : storage(size) {
        quic_buffer.Buffer = storage.data();
        quic_buffer.Length = static_cast<uint32_t>(storage.size());
    }
};
#endif

class Stats {
  public:
    struct LatencySnapshot {
        uint64_t p50_ns{std::numeric_limits<uint64_t>::max()};
        uint64_t p75_ns{std::numeric_limits<uint64_t>::max()};
        uint64_t p99_ns{std::numeric_limits<uint64_t>::max()};
    };

    struct LatencyDetailSnapshot {
        size_t count{0};
        uint64_t min_ns{std::numeric_limits<uint64_t>::max()};
        uint64_t max_ns{std::numeric_limits<uint64_t>::max()};
        uint64_t p90_ns{std::numeric_limits<uint64_t>::max()};
        uint64_t p95_ns{std::numeric_limits<uint64_t>::max()};
        uint64_t p99_ns{std::numeric_limits<uint64_t>::max()};
        uint64_t p999_ns{std::numeric_limits<uint64_t>::max()};
        double mean_ns{0.0};
        double stddev_ns{0.0};
        std::array<uint64_t, 7> bucket_counts{};
    };

    void AddSent(uint64_t bytes) {
        sent_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        sent_messages_.fetch_add(1, std::memory_order_relaxed);
    }

    void AddReceived(uint64_t bytes) {
        recv_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        recv_messages_.fetch_add(1, std::memory_order_relaxed);
    }

    void AddLatencyNs(uint64_t latency_ns) {
        std::lock_guard<std::mutex> lock(latency_samples_mutex_);
        latency_samples_.push_back(latency_ns);
    }

    struct Snapshot {
        uint64_t sent_bytes;
        uint64_t recv_bytes;
        uint64_t sent_messages;
        uint64_t recv_messages;
        LatencySnapshot latency;
    };

    Snapshot SnapshotNow() const {
        return Snapshot{
            sent_bytes_.load(std::memory_order_relaxed),
            recv_bytes_.load(std::memory_order_relaxed),
            sent_messages_.load(std::memory_order_relaxed),
            recv_messages_.load(std::memory_order_relaxed),
            BuildLatencySnapshot(),
        };
    }

  private:
    LatencySnapshot BuildLatencySnapshot() const {
        LatencySnapshot snapshot;
        std::vector<uint64_t> samples;
        {
            std::lock_guard<std::mutex> lock(latency_samples_mutex_);
            samples = latency_samples_;
        }

        if (samples.empty()) {
            return snapshot;
        }

        std::sort(samples.begin(), samples.end());
        snapshot.p50_ns = PercentileValue(samples, 0.50);
        snapshot.p75_ns = PercentileValue(samples, 0.75);
        snapshot.p99_ns = PercentileValue(samples, 0.99);
        return snapshot;
    }

    static uint64_t PercentileValue(const std::vector<uint64_t>& sorted, double percentile) {
        if (sorted.empty()) {
            return std::numeric_limits<uint64_t>::max();
        }
        const double scaled = percentile * static_cast<double>(sorted.size() - 1);
        const size_t index = static_cast<size_t>(scaled + 0.5);
        return sorted[std::min(index, sorted.size() - 1)];
    }

    std::atomic<uint64_t> sent_bytes_{0};
    std::atomic<uint64_t> recv_bytes_{0};
    std::atomic<uint64_t> sent_messages_{0};
    std::atomic<uint64_t> recv_messages_{0};
    mutable std::mutex latency_samples_mutex_;
    std::vector<uint64_t> latency_samples_;
};

struct StreamMetricSnapshot {
    uint64_t sent_messages{0};
    uint64_t echoed_messages{0};
    uint64_t sent_bytes{0};
    uint64_t echoed_bytes{0};
    Stats::LatencySnapshot latency;
    Stats::LatencyDetailSnapshot latency_detail;
};

class StreamMetrics {
  public:
    void AddSent(uint64_t bytes) {
        sent_bytes_ += bytes;
        sent_messages_ += 1;
    }

    void AddReceived(uint64_t bytes, uint64_t latency_ns) {
        echoed_bytes_ += bytes;
        echoed_messages_ += 1;
        latency_samples_.push_back(latency_ns);
    }

    StreamMetricSnapshot Snapshot() const {
        StreamMetricSnapshot snapshot;
        snapshot.sent_messages = sent_messages_;
        snapshot.echoed_messages = echoed_messages_;
        snapshot.sent_bytes = sent_bytes_;
        snapshot.echoed_bytes = echoed_bytes_;
        if (!latency_samples_.empty()) {
            auto samples = latency_samples_;
            std::sort(samples.begin(), samples.end());
            snapshot.latency.p50_ns = PercentileValue(samples, 0.50);
            snapshot.latency.p75_ns = PercentileValue(samples, 0.75);
            snapshot.latency.p99_ns = PercentileValue(samples, 0.99);
            snapshot.latency_detail.count = samples.size();
            snapshot.latency_detail.min_ns = samples.front();
            snapshot.latency_detail.max_ns = samples.back();
            snapshot.latency_detail.p90_ns = PercentileValue(samples, 0.90);
            snapshot.latency_detail.p95_ns = PercentileValue(samples, 0.95);
            snapshot.latency_detail.p99_ns = PercentileValue(samples, 0.99);
            snapshot.latency_detail.p999_ns = PercentileValue(samples, 0.999);
            double sum = 0.0;
            for (uint64_t sample : samples) {
                sum += static_cast<double>(sample);
                const double ms = static_cast<double>(sample) / 1'000'000.0;
                if (ms <= 0.5) {
                    snapshot.latency_detail.bucket_counts[0] += 1;
                } else if (ms <= 1.0) {
                    snapshot.latency_detail.bucket_counts[1] += 1;
                } else if (ms <= 2.0) {
                    snapshot.latency_detail.bucket_counts[2] += 1;
                } else if (ms <= 5.0) {
                    snapshot.latency_detail.bucket_counts[3] += 1;
                } else if (ms <= 10.0) {
                    snapshot.latency_detail.bucket_counts[4] += 1;
                } else if (ms <= 50.0) {
                    snapshot.latency_detail.bucket_counts[5] += 1;
                } else {
                    snapshot.latency_detail.bucket_counts[6] += 1;
                }
            }
            snapshot.latency_detail.mean_ns = sum / static_cast<double>(samples.size());
            double variance_sum = 0.0;
            for (uint64_t sample : samples) {
                const double delta = static_cast<double>(sample) - snapshot.latency_detail.mean_ns;
                variance_sum += delta * delta;
            }
            snapshot.latency_detail.stddev_ns = std::sqrt(variance_sum / static_cast<double>(samples.size()));
        }
        return snapshot;
    }

  private:
    static uint64_t PercentileValue(const std::vector<uint64_t>& sorted, double percentile) {
        const double scaled = percentile * static_cast<double>(sorted.size() - 1);
        const size_t index = static_cast<size_t>(scaled + 0.5);
        return sorted[std::min(index, sorted.size() - 1)];
    }

    uint64_t sent_messages_{0};
    uint64_t echoed_messages_{0};
    uint64_t sent_bytes_{0};
    uint64_t echoed_bytes_{0};
    std::vector<uint64_t> latency_samples_;
};

inline std::string FormatRateMbps(uint64_t bytes, double seconds) {
    if (seconds <= 0.0) {
        return "0.00";
    }
    const double mbps = (static_cast<double>(bytes) * 8.0) / seconds / 1'000'000.0;
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << mbps;
    return stream.str();
}

inline std::string FormatDouble(double value, int precision) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

inline std::string FormatLatencyMs(uint64_t ns) {
    if (ns == std::numeric_limits<uint64_t>::max()) {
        return "n/a";
    }
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3)
           << (static_cast<double>(ns) / 1'000'000.0);
    return stream.str();
}

inline std::string FormatLatencySummary(const Stats::LatencySnapshot& latency) {
    std::ostringstream stream;
    stream << FormatLatencyMs(latency.p50_ns) << "/"
           << FormatLatencyMs(latency.p75_ns) << "/"
           << FormatLatencyMs(latency.p99_ns);
    return stream.str();
}

inline std::string FormatLatencyDetailSummary(const Stats::LatencyDetailSnapshot& latency) {
    std::ostringstream stream;
    stream << "count=" << latency.count
           << " mean_ms=" << FormatDouble(latency.mean_ns / 1'000'000.0, 3)
           << " stddev_ms=" << FormatDouble(latency.stddev_ns / 1'000'000.0, 3)
           << " min_ms=" << FormatLatencyMs(latency.min_ns)
           << " p90_ms=" << FormatLatencyMs(latency.p90_ns)
           << " p95_ms=" << FormatLatencyMs(latency.p95_ns)
           << " p99_ms=" << FormatLatencyMs(latency.p99_ns)
           << " p999_ms=" << FormatLatencyMs(latency.p999_ns)
           << " max_ms=" << FormatLatencyMs(latency.max_ns)
           << " buckets_ms[<=0.5,<=1,<=2,<=5,<=10,<=50,>50]="
           << latency.bucket_counts[0] << ","
           << latency.bucket_counts[1] << ","
           << latency.bucket_counts[2] << ","
           << latency.bucket_counts[3] << ","
           << latency.bucket_counts[4] << ","
           << latency.bucket_counts[5] << ","
           << latency.bucket_counts[6];
    return stream.str();
}

class StatsPrinter {
  public:
    StatsPrinter(std::string name, const Stats& stats, uint64_t interval_ms)
        : name_(std::move(name)), stats_(stats), interval_ms_(interval_ms) {}

    void Start() {
        if (interval_ms_ == 0) {
            return;
        }
        worker_ = std::thread([this]() { Run(); });
    }

    void Stop() {
        stop_.store(true, std::memory_order_relaxed);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

  private:
    void Run() {
        auto previous = stats_.SnapshotNow();
        auto previous_time = Clock::now();

        while (!stop_.load(std::memory_order_relaxed) && !g_stop_requested.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));

            const auto now = Clock::now();
            const auto current = stats_.SnapshotNow();
            const double seconds = std::chrono::duration<double>(now - previous_time).count();

            const auto delta_sent_bytes = current.sent_bytes - previous.sent_bytes;
            const auto delta_recv_bytes = current.recv_bytes - previous.recv_bytes;
            const auto delta_recv_messages = current.recv_messages - previous.recv_messages;
            std::cout << "[" << name_ << "] "
                      << "tx=" << FormatRateMbps(delta_sent_bytes, seconds) << " Mbps "
                      << "rx=" << FormatRateMbps(delta_recv_bytes, seconds) << " Mbps "
                      << "msg/s=" << static_cast<uint64_t>(delta_recv_messages / std::max(seconds, 0.001)) << " "
                      << "latency_ms(p50/p75/p99)="
                      << FormatLatencySummary(current.latency)
                      << std::endl;

            previous = current;
            previous_time = now;
        }
    }

    std::string name_;
    const Stats& stats_;
    uint64_t interval_ms_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

inline std::string SocketErrorString(const char* action) {
    return std::string(action) + " failed: " + std::strerror(errno);
}

inline std::string OpenSslErrorString(const char* action) {
    std::ostringstream stream;
    stream << action << " failed";
    unsigned long error = 0;
    bool first = true;
    while ((error = ERR_get_error()) != 0) {
        char buffer[256];
        ERR_error_string_n(error, buffer, sizeof(buffer));
        stream << (first ? ": " : " | ") << buffer;
        first = false;
    }
    return stream.str();
}

inline int SslReadCompat(SSL* ssl, void* buffer, size_t length, size_t* read) {
#ifdef HAVE_LSQUIC
    const int capped = static_cast<int>(std::min<size_t>(length, static_cast<size_t>(INT_MAX)));
    const int rc = SSL_read(ssl, buffer, capped);
    if (rc > 0) {
        *read = static_cast<size_t>(rc);
        return 1;
    }
    *read = 0;
    return rc;
#else
    return SSL_read_ex(ssl, buffer, length, read);
#endif
}

inline int SslWriteCompat(SSL* ssl, const void* buffer, size_t length, size_t* written) {
#ifdef HAVE_LSQUIC
    const int capped = static_cast<int>(std::min<size_t>(length, static_cast<size_t>(INT_MAX)));
    const int rc = SSL_write(ssl, buffer, capped);
    if (rc > 0) {
        *written = static_cast<size_t>(rc);
        return 1;
    }
    *written = 0;
    return rc;
#else
    return SSL_write_ex(ssl, buffer, length, written);
#endif
}

#ifndef OPENSSL_NO_SCTP
inline std::string SctpSocketDebugString(int fd) {
    std::ostringstream stream;
    int value = 0;
    socklen_t value_len = sizeof(value);

    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &value, &value_len) == 0) {
        stream << " so_type=" << value;
    }
    value = 0;
    value_len = sizeof(value);
    if (::getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &value, &value_len) == 0) {
        stream << " so_protocol=" << value;
    }

    sctp_event_subscribe events{};
    socklen_t events_len = sizeof(events);
    if (::getsockopt(fd, IPPROTO_SCTP, SCTP_EVENTS, &events, &events_len) == 0) {
        stream << " sctp_events[data_io=" << static_cast<int>(events.sctp_data_io_event)
               << ",assoc=" << static_cast<int>(events.sctp_association_event)
               << ",auth=" << static_cast<int>(events.sctp_authentication_event)
               << ",shutdown=" << static_cast<int>(events.sctp_shutdown_event) << "]";
    }

    stream << " auth_enable=" << (LinuxSctpAuthEnabled() ? 1 : 0);
    return stream.str();
}
#endif

class OpenSslInitializer {
  public:
    OpenSslInitializer() {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_ssl_algorithms();
    }

    ~OpenSslInitializer() {
        EVP_cleanup();
    }
};

class ITransportConnection {
  public:
    virtual ~ITransportConnection() = default;
    virtual uint32_t Id() const = 0;
    virtual void SendCopy(const uint8_t* data, size_t length, uint16_t stream_id) = 0;
    virtual void CloseSend() = 0;
    virtual void Close() = 0;
};

class ITransportEventHandler {
  public:
    virtual ~ITransportEventHandler() = default;
    virtual void OnConnected(const std::shared_ptr<ITransportConnection>& connection) = 0;
    virtual void OnData(
        const std::shared_ptr<ITransportConnection>& connection,
        uint16_t stream_id,
        const uint8_t* data,
        size_t length) = 0;
    virtual void OnPeerClosed(const std::shared_ptr<ITransportConnection>& connection) = 0;
    virtual void OnClosed(uint32_t connection_id) = 0;
    virtual void OnTransportError(uint32_t connection_id, const std::string& message) = 0;
};

class ITransportRunner {
  public:
    virtual ~ITransportRunner() = default;
    virtual void Start() = 0;
    virtual void Stop() = 0;
};

class SctpTlsContext {
  public:
    enum class Role {
        Server,
        Client,
    };

    SctpTlsContext(const AppConfig& config, Role role)
        : enabled_(config.sctp_tls),
          role_(role) {
        if (!enabled_) {
            return;
        }

#ifdef OPENSSL_NO_SCTP
        throw std::runtime_error(
            "SCTP TLS requested, but this OpenSSL build does not support DTLS-over-SCTP (OPENSSL_NO_SCTP)");
#else
        static OpenSslInitializer init;

        if (!LinuxSctpAuthEnabled()) {
            throw std::runtime_error(
                "SCTP TLS requested, but Linux SCTP AUTH is disabled (net.sctp.auth_enable=0)");
        }

        ctx_ = SSL_CTX_new(DTLS_method());
        if (ctx_ == nullptr) {
            throw std::runtime_error(OpenSslErrorString("SSL_CTX_new(DTLS_method)"));
        }

        SSL_CTX_set_mode(
            ctx_,
            SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_AUTO_RETRY);
        SSL_CTX_set_read_ahead(ctx_, 1);

        if (role_ == Role::Server) {
            if (SSL_CTX_use_certificate_file(ctx_, config.cert_file.c_str(), SSL_FILETYPE_PEM) != 1) {
                throw std::runtime_error(OpenSslErrorString("SSL_CTX_use_certificate_file"));
            }
            if (SSL_CTX_use_PrivateKey_file(ctx_, config.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
                throw std::runtime_error(OpenSslErrorString("SSL_CTX_use_PrivateKey_file"));
            }
            if (SSL_CTX_check_private_key(ctx_) != 1) {
                throw std::runtime_error(OpenSslErrorString("SSL_CTX_check_private_key"));
            }
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
        } else if (config.verify_peer) {
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
            if (!config.ca_file.empty()) {
                if (SSL_CTX_load_verify_locations(ctx_, config.ca_file.c_str(), nullptr) != 1) {
                    throw std::runtime_error(OpenSslErrorString("SSL_CTX_load_verify_locations"));
                }
            } else if (SSL_CTX_set_default_verify_paths(ctx_) != 1) {
                throw std::runtime_error(OpenSslErrorString("SSL_CTX_set_default_verify_paths"));
            }
        } else {
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
        }
#endif
    }

    ~SctpTlsContext() {
        if (ctx_ != nullptr) {
            SSL_CTX_free(ctx_);
        }
    }

    bool enabled() const {
        return enabled_;
    }

    void PrimeSocket(int fd, const char* label) const {
        if (!enabled_) {
            return;
        }

#ifdef OPENSSL_NO_SCTP
        (void)fd;
        (void)label;
#else
        BIO* bio = BIO_new_dgram_sctp(fd, BIO_NOCLOSE);
        if (bio == nullptr) {
            const int saved_errno = errno;
            throw std::runtime_error(
                OpenSslErrorString(label) + " errno=" + std::to_string(saved_errno) +
                " (" + std::strerror(saved_errno) + ")" + SctpSocketDebugString(fd));
        }
        BIO_free(bio);
#endif
    }

    SSL* CreateAndHandshake(int fd) const {
        if (!enabled_) {
            return nullptr;
        }

#ifdef OPENSSL_NO_SCTP
        (void)fd;
        throw std::runtime_error(
            "SCTP TLS requested, but this OpenSSL build does not support DTLS-over-SCTP (OPENSSL_NO_SCTP)");
#else
        SSL* ssl = SSL_new(ctx_);
        if (ssl == nullptr) {
            throw std::runtime_error(OpenSslErrorString("SSL_new"));
        }

        BIO* bio = BIO_new_dgram_sctp(fd, BIO_NOCLOSE);
        if (bio == nullptr) {
            const int saved_errno = errno;
            SSL_free(ssl);
            throw std::runtime_error(
                OpenSslErrorString("BIO_new_dgram_sctp") + " errno=" + std::to_string(saved_errno) + " (" +
                std::strerror(saved_errno) + ")" + SctpSocketDebugString(fd));
        }

        SSL_set_bio(ssl, bio, bio);
        if (role_ == Role::Server) {
            SSL_set_accept_state(ssl);
            if (SSL_accept(ssl) != 1) {
                const std::string error = OpenSslErrorString("SSL_accept");
                SSL_free(ssl);
                throw std::runtime_error(error);
            }
        } else {
            SSL_set_connect_state(ssl);
            if (SSL_connect(ssl) != 1) {
                const std::string error = OpenSslErrorString("SSL_connect");
                SSL_free(ssl);
                throw std::runtime_error(error);
            }
        }
        return ssl;
#endif
    }

  private:
    bool enabled_{false};
    Role role_;
    SSL_CTX* ctx_{nullptr};
};


} // namespace proto_test
