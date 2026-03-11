#include <msquic.h>

#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <linux/sctp.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <map>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

constexpr uint32_t kMessageMagic = 0x4D53514C; // MSQL
constexpr char kDefaultAlpn[] = "msquic-load";

enum class Protocol {
    MsQuic,
    Sctp,
};

std::string ProtocolName(Protocol protocol) {
    switch (protocol) {
    case Protocol::MsQuic:
        return "msquic";
    case Protocol::Sctp:
        return "sctp";
    }
    return "unknown";
}

Protocol ParseProtocol(const std::string& value) {
    if (value == "msquic") {
        return Protocol::MsQuic;
    }
    if (value == "sctp") {
        return Protocol::Sctp;
    }
    throw std::runtime_error("unsupported protocol: " + value + " (expected msquic or sctp)");
}

std::atomic<bool> g_stop_requested{false};

void SignalHandler(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<Nanoseconds>(Clock::now().time_since_epoch()).count());
}

std::string StatusToHex(QUIC_STATUS status) {
    std::ostringstream stream;
    stream << "0x" << std::hex << static_cast<uint32_t>(status);
    return stream.str();
}

#ifndef OPENSSL_NO_SCTP
std::string ReadTextFile(const char* path) {
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return "";
    }
    char buffer[128] {};
    const size_t bytes = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    return std::string(buffer, bytes);
}

bool LinuxSctpAuthEnabled() {
    const std::string value = ReadTextFile("/proc/sys/net/sctp/auth_enable");
    return !value.empty() && value[0] == '1';
}
#endif

sockaddr_in ResolveIpv4Address(const std::string& host, uint16_t port, const char* context) {
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

Args ParseArgs(int argc, char** argv) {
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

std::optional<std::string> FindArg(const Args& args, const std::string& key) {
    const auto it = args.values.find(key);
    if (it == args.values.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool GetBool(const Args& args, const std::string& key, bool default_value) {
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

std::string GetRequired(const Args& args, const std::string& key) {
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

std::string GetString(const Args& args, const std::string& key, const std::string& default_value) {
    const auto value = FindArg(args, key);
    return value.has_value() ? *value : default_value;
}

void PrintUsage() {
    std::cout
        << "Usage:\n"
        << "  msquic-loadtest server [options]\n"
        << "  msquic-loadtest client --target=HOST [options]\n\n"
        << "Common options:\n"
        << "  --protocol=msquic|sctp    Transport protocol, default msquic\n"
        << "  --base-port=PORT           First port, default 15443\n"
        << "  --server-count=N           Number of listeners, default 1\n"
        << "  --message-size=BYTES       Fixed frame size, default 1024\n"
        << "  --idle-timeout-ms=N        Idle timeout, default 30000\n"
        << "  --stats-interval-ms=N      Stats interval, default 1000\n\n"
        << "Server options:\n"
        << "  --cert=FILE                PEM certificate (msquic, or sctp with --sctp-tls=1)\n"
        << "  --key=FILE                 PEM private key (msquic, or sctp with --sctp-tls=1)\n"
        << "  --password=TEXT            Optional private key password\n"
        << "  --bind=ADDR                Listener bind address, default 0.0.0.0\n"
        << "  --alpn=TEXT                ALPN, default msquic-load\n"
        << "  --sctp-nodelay=1           Disable SCTP Nagle-like bundling, default enabled\n"
        << "  --sctp-stream-id=N         SCTP stream id, default 0\n"
        << "  --sctp-tls=1               Enable DTLS-over-SCTP, default disabled\n"
        << "  --ca=FILE                  CA bundle for peer verification\n\n"
        << "Client options:\n"
        << "  --target=HOST              Server name or IP\n"
        << "  --clients=N                Number of connections, default 1\n"
        << "  --max-inflight=N           Max outstanding echoed messages per connection, default 64\n"
        << "  --send-server-index=N      Only send on connections mapped to this server index, default all\n"
        << "  --send-pps=N               Total offered message rate across all clients, default unlimited\n"
        << "  --duration-sec=N           Active send duration, default 30\n"
        << "  --drain-timeout-ms=N       Drain time after send stop, default 5000\n"
        << "  --verify-peer=1            Enable certificate validation, default disabled\n"
        << "  --alpn=TEXT                ALPN, default msquic-load\n"
        << "  --sctp-nodelay=1           Disable SCTP Nagle-like bundling, default enabled\n"
        << "  --sctp-stream-id=N         SCTP stream id, default 0\n"
        << "  --sctp-tls=1               Enable DTLS-over-SCTP, default disabled\n"
        << "  --ca=FILE                  CA bundle for peer verification\n";
}

struct AppConfig {
    std::string mode;
    Protocol protocol{Protocol::MsQuic};
    std::string alpn{kDefaultAlpn};
    uint16_t base_port{15443};
    uint32_t server_count{1};
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
    uint64_t duration_sec{30};
    uint64_t drain_timeout_ms{5000};
    bool verify_peer{false};
    bool sctp_nodelay{true};
    uint16_t sctp_stream_id{0};
    bool sctp_tls{false};
    std::string ca_file;
};

AppConfig LoadConfig(const Args& args) {
    AppConfig config;
    config.mode = args.mode;
    config.protocol = ParseProtocol(GetString(args, "protocol", "msquic"));
    config.alpn = GetString(args, "alpn", kDefaultAlpn);
    config.base_port = GetNumber<uint16_t>(args, "base-port", 15443);
    config.server_count = GetNumber<uint32_t>(args, "server-count", 1);
    config.message_size = GetNumber<uint32_t>(args, "message-size", 1024);
    config.idle_timeout_ms = GetNumber<uint64_t>(args, "idle-timeout-ms", 30000);
    config.stats_interval_ms = GetNumber<uint64_t>(args, "stats-interval-ms", 1000);
    config.sctp_nodelay = GetBool(args, "sctp-nodelay", true);
    config.sctp_stream_id = GetNumber<uint16_t>(args, "sctp-stream-id", 0);
    config.sctp_tls = GetBool(args, "sctp-tls", false);
    config.ca_file = GetString(args, "ca", "");

    if (config.message_size < 24) {
        throw std::runtime_error("--message-size must be at least 24 bytes");
    }
    if (config.server_count == 0) {
        throw std::runtime_error("--server-count must be >= 1");
    }

    if (config.mode == "server") {
        config.bind = GetString(args, "bind", "0.0.0.0");
        if (config.protocol == Protocol::MsQuic) {
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
    } else {
        throw std::runtime_error("mode must be 'server' or 'client'");
    }

    return config;
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

struct SendBuffer {
    QUIC_BUFFER quic_buffer{};
    std::vector<uint8_t> storage;

    explicit SendBuffer(size_t size) : storage(size) {
        quic_buffer.Buffer = storage.data();
        quic_buffer.Length = static_cast<uint32_t>(storage.size());
    }
};

class Stats {
  public:
    struct LatencySnapshot {
        uint64_t p50_ns{std::numeric_limits<uint64_t>::max()};
        uint64_t p75_ns{std::numeric_limits<uint64_t>::max()};
        uint64_t p99_ns{std::numeric_limits<uint64_t>::max()};
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

std::string FormatRateMbps(uint64_t bytes, double seconds) {
    if (seconds <= 0.0) {
        return "0.00";
    }
    const double mbps = (static_cast<double>(bytes) * 8.0) / seconds / 1'000'000.0;
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << mbps;
    return stream.str();
}

std::string FormatLatencyMs(uint64_t ns) {
    if (ns == std::numeric_limits<uint64_t>::max()) {
        return "n/a";
    }
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3)
           << (static_cast<double>(ns) / 1'000'000.0);
    return stream.str();
}

std::string FormatLatencySummary(const Stats::LatencySnapshot& latency) {
    std::ostringstream stream;
    stream << FormatLatencyMs(latency.p50_ns) << "/"
           << FormatLatencyMs(latency.p75_ns) << "/"
           << FormatLatencyMs(latency.p99_ns);
    return stream.str();
}

class StatsPrinter {
  public:
    StatsPrinter(std::string name, const Stats& stats, uint64_t interval_ms)
        : name_(std::move(name)), stats_(stats), interval_ms_(interval_ms) {}

    void Start() {
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

std::string SocketErrorString(const char* action) {
    return std::string(action) + " failed: " + std::strerror(errno);
}

std::string OpenSslErrorString(const char* action) {
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

#ifndef OPENSSL_NO_SCTP
std::string SctpSocketDebugString(int fd) {
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
    virtual void SendCopy(const uint8_t* data, size_t length) = 0;
    virtual void CloseSend() = 0;
    virtual void Close() = 0;
};

class ITransportEventHandler {
  public:
    virtual ~ITransportEventHandler() = default;
    virtual void OnConnected(const std::shared_ptr<ITransportConnection>& connection) = 0;
    virtual void OnData(const std::shared_ptr<ITransportConnection>& connection, const uint8_t* data, size_t length) = 0;
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

        SSL_CTX_set_mode(ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
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

class SctpConnection : public ITransportConnection, public std::enable_shared_from_this<SctpConnection> {
  public:
    SctpConnection(int fd, uint32_t id, uint16_t stream_id, ITransportEventHandler& handler, SSL* ssl)
        : fd_(fd), id_(id), stream_id_(stream_id), handler_(handler), ssl_(ssl) {
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SO_RCVTIMEO)"));
        }
    }

    ~SctpConnection() override {
        if (recv_thread_.joinable()) {
            if (recv_thread_.get_id() == std::this_thread::get_id()) {
                recv_thread_.detach();
            } else {
                recv_thread_.join();
            }
        }
        if (ssl_ != nullptr) {
            SSL_free(ssl_);
        }
        CloseFd();
    }

    uint32_t Id() const override {
        return id_;
    }

    void StartReceiveLoop() {
        recv_thread_ = std::thread([self = shared_from_this()]() { self->ReceiveLoop(); });
    }

    void Join() {
        if (recv_thread_.joinable()) {
            recv_thread_.join();
        }
    }

    void SendCopy(const uint8_t* data, size_t length) override {
        if (closed_.load(std::memory_order_relaxed)) {
            return;
        }

        if (ssl_ != nullptr) {
            size_t offset = 0;
            while (offset < length) {
                size_t written = 0;
                if (SSL_write_ex(ssl_, data + offset, length - offset, &written) != 1) {
                    throw std::runtime_error(OpenSslErrorString("SSL_write_ex"));
                }
                offset += written;
            }
            return;
        }

        size_t offset = 0;
        while (offset < length) {
            struct msghdr msg {};
            struct iovec iov {};
            iov.iov_base = const_cast<uint8_t*>(data + offset);
            iov.iov_len = length - offset;
            char control[CMSG_SPACE(sizeof(sctp_sndinfo))] {};

            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = control;
            msg.msg_controllen = sizeof(control);

            auto* cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = IPPROTO_SCTP;
            cmsg->cmsg_type = SCTP_SNDINFO;
            cmsg->cmsg_len = CMSG_LEN(sizeof(sctp_sndinfo));

            auto* sndinfo = reinterpret_cast<sctp_sndinfo*>(CMSG_DATA(cmsg));
            std::memset(sndinfo, 0, sizeof(*sndinfo));
            sndinfo->snd_sid = stream_id_;
            msg.msg_controllen = cmsg->cmsg_len;

            const ssize_t written = sendmsg(fd_, &msg, 0);
            if (written < 0) {
                throw std::runtime_error(SocketErrorString("sendmsg"));
            }
            offset += static_cast<size_t>(written);
        }
    }

    void CloseSend() override {
        bool expected = false;
        if (!send_closed_.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            return;
        }
        if (fd_.load(std::memory_order_relaxed) < 0) {
            return;
        }
        if (ssl_ != nullptr) {
            SSL_shutdown(ssl_);
        }
        if (::shutdown(fd_, SHUT_WR) != 0 && errno != ENOTCONN && errno != EBADF) {
            throw std::runtime_error(SocketErrorString("shutdown(SHUT_WR)"));
        }
    }

    void Close() override {
        if (closed_.exchange(true, std::memory_order_relaxed)) {
            return;
        }
        ::shutdown(fd_, SHUT_RDWR);
        CloseFd();
    }

  private:
    void ReceiveLoop() {
        std::vector<uint8_t> buffer(64 * 1024);

        while (!closed_.load(std::memory_order_relaxed) && !g_stop_requested.load(std::memory_order_relaxed)) {
            ssize_t bytes = -1;
            if (ssl_ != nullptr) {
                size_t read = 0;
                const int ok = SSL_read_ex(ssl_, buffer.data(), buffer.size(), &read);
                if (ok == 1) {
                    bytes = static_cast<ssize_t>(read);
                } else {
                    const int ssl_error = SSL_get_error(ssl_, ok);
                    if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                        bytes = 0;
                    } else if (ssl_error == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        if (closed_.load(std::memory_order_relaxed)) {
                            break;
                        }
                        continue;
                    } else if (ssl_error == SSL_ERROR_SYSCALL && errno != 0) {
                        bytes = -1;
                    } else {
                        handler_.OnTransportError(id_, OpenSslErrorString("SSL_read_ex"));
                        break;
                    }
                }
            } else {
                bytes = recv(fd_, buffer.data(), buffer.size(), 0);
            }
            if (bytes > 0) {
                try {
                    handler_.OnData(shared_from_this(), buffer.data(), static_cast<size_t>(bytes));
                } catch (const std::exception& ex) {
                    handler_.OnTransportError(id_, ex.what());
                    break;
                }
                continue;
            }
            if (bytes == 0) {
                try {
                    handler_.OnPeerClosed(shared_from_this());
                } catch (const std::exception& ex) {
                    handler_.OnTransportError(id_, ex.what());
                }
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (closed_.load(std::memory_order_relaxed)) {
                    break;
                }
                continue;
            }
            if (errno == ENOTCONN || errno == ECONNRESET || errno == EPIPE) {
                try {
                    handler_.OnPeerClosed(shared_from_this());
                } catch (const std::exception& ex) {
                    handler_.OnTransportError(id_, ex.what());
                }
                break;
            }
            if (closed_.load(std::memory_order_relaxed)) {
                break;
            }
            handler_.OnTransportError(id_, SocketErrorString("recv"));
            break;
        }

        Close();
        handler_.OnClosed(id_);
    }

    void CloseFd() {
        const int fd = fd_.exchange(-1, std::memory_order_relaxed);
        if (fd >= 0) {
            ::close(fd);
        }
    }

    std::atomic<int> fd_;
    const uint32_t id_;
    const uint16_t stream_id_;
    ITransportEventHandler& handler_;
    std::atomic<bool> closed_{false};
    std::atomic<bool> send_closed_{false};
    std::thread recv_thread_;
    SSL* ssl_{nullptr};
};

struct LoadServerConnectionState {
    std::shared_ptr<ITransportConnection> connection;
    std::vector<uint8_t> receive_buffer;
    bool peer_closed{false};
};

class LoadServerController : public ITransportEventHandler {
  public:
    LoadServerController(const AppConfig& config, Stats& stats)
        : config_(config), stats_(stats) {}

    void OnConnected(const std::shared_ptr<ITransportConnection>& connection) override {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_[connection->Id()].connection = connection;
        std::cerr << ProtocolName(config_.protocol) << " server connection " << connection->Id() << " established" << std::endl;
    }

    void OnData(const std::shared_ptr<ITransportConnection>& connection, const uint8_t* data, size_t length) override {
        std::vector<std::vector<uint8_t>> sends;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& state = connections_[connection->Id()];
            state.connection = connection;
            state.receive_buffer.insert(state.receive_buffer.end(), data, data + length);
            while (state.receive_buffer.size() >= config_.message_size) {
                std::vector<uint8_t> frame(config_.message_size);
                std::memcpy(frame.data(), state.receive_buffer.data(), config_.message_size);
                state.receive_buffer.erase(
                    state.receive_buffer.begin(),
                    state.receive_buffer.begin() + static_cast<std::ptrdiff_t>(config_.message_size));
                sends.push_back(std::move(frame));
            }
        }

        for (const auto& frame : sends) {
            connection->SendCopy(frame.data(), frame.size());
            stats_.AddReceived(frame.size());
            stats_.AddSent(frame.size());
        }
    }

    void OnPeerClosed(const std::shared_ptr<ITransportConnection>& connection) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(connection->Id());
        if (it != connections_.end()) {
            it->second.peer_closed = true;
        }
    }

    void OnClosed(uint32_t connection_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.erase(connection_id);
    }

    void OnTransportError(uint32_t connection_id, const std::string& message) override {
        std::cerr << ProtocolName(config_.protocol) << " server connection " << connection_id
                  << " error: " << message << std::endl;
    }

  private:
    const AppConfig& config_;
    Stats& stats_;
    std::mutex mutex_;
    std::map<uint32_t, LoadServerConnectionState> connections_;
};

struct LoadClientConnectionState {
    std::shared_ptr<ITransportConnection> connection;
    std::vector<uint8_t> receive_buffer;
    uint64_t next_sequence{0};
    uint64_t echoed_messages{0};
    double next_send_time_ns{0.0};
    bool connected{false};
    bool send_closed{false};
};

class LoadClientController : public ITransportEventHandler {
  public:
    LoadClientController(const AppConfig& config, Stats& stats)
        : config_(config),
          stats_(stats),
          paced_(config.send_pps > 0),
          pacing_interval_ns_(paced_
                                  ? (1'000'000'000.0 * static_cast<double>(config.client_count)) /
                                        static_cast<double>(config.send_pps)
                                  : 0.0) {}

    ~LoadClientController() {
        StopPacer();
    }

    void StartPacer() {
        if (!paced_) {
            return;
        }
        pacer_stop_.store(false, std::memory_order_relaxed);
        pacer_thread_ = std::thread([this]() { PacerLoop(); });
    }

    void StopPacer() {
        pacer_stop_.store(true, std::memory_order_relaxed);
        if (pacer_thread_.joinable()) {
            pacer_thread_.join();
        }
    }

    void OnConnected(const std::shared_ptr<ITransportConnection>& connection) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& state = states_[connection->Id()];
            state.connection = connection;
            state.connected = true;
            state.next_send_time_ns = static_cast<double>(NowNs());
        }
        std::cerr << ProtocolName(config_.protocol) << " client connection " << connection->Id() << " established" << std::endl;
        if (!paced_) {
            PumpSends(connection->Id());
        }
    }

    void OnData(const std::shared_ptr<ITransportConnection>& connection, const uint8_t* data, size_t length) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& state = states_[connection->Id()];
            state.connection = connection;
            state.receive_buffer.insert(state.receive_buffer.end(), data, data + length);

            while (state.receive_buffer.size() >= config_.message_size) {
                MessageHeader header{};
                std::memcpy(&header, state.receive_buffer.data(), sizeof(header));
                if (header.magic != kMessageMagic) {
                    throw std::runtime_error("invalid echoed frame magic on connection " + std::to_string(connection->Id()));
                }
                state.receive_buffer.erase(
                    state.receive_buffer.begin(),
                    state.receive_buffer.begin() + static_cast<std::ptrdiff_t>(config_.message_size));
                ++state.echoed_messages;
                stats_.AddReceived(config_.message_size);
                stats_.AddLatencyNs(NowNs() - header.send_timestamp_ns);
            }
        }
        if (!paced_) {
            PumpSends(connection->Id());
        }
    }

    void OnPeerClosed(const std::shared_ptr<ITransportConnection>&) override {}

    void OnClosed(uint32_t connection_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_connections_.insert(connection_id).second) {
            done_cv_.notify_all();
        }
    }

    void OnTransportError(uint32_t connection_id, const std::string& message) override {
        std::cerr << ProtocolName(config_.protocol) << " client connection " << connection_id
                  << " error: " << message << std::endl;
    }

    void RequestStopSending() {
        stop_sending_.store(true, std::memory_order_relaxed);

        std::vector<uint32_t> ids;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [id, _] : states_) {
                ids.push_back(id);
            }
        }
        for (uint32_t id : ids) {
            try {
                PumpSends(id);
            } catch (const std::exception& ex) {
                OnTransportError(id, ex.what());
            }
        }
    }

    void ForceShutdownAll() {
        std::vector<std::shared_ptr<ITransportConnection>> connections;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [_, state] : states_) {
                if (state.connection != nullptr) {
                    connections.push_back(state.connection);
                }
            }
        }
        for (const auto& connection : connections) {
            connection->Close();
        }
    }

    bool WaitUntilDone(uint32_t expected_connections, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return done_cv_.wait_for(lock, timeout, [&]() {
            return closed_connections_.size() >= expected_connections;
        });
    }

  private:
    bool ShouldSendOnConnection(uint32_t connection_id) const {
        return config_.send_server_index < 0 ||
               (connection_id % config_.server_count) == static_cast<uint32_t>(config_.send_server_index);
    }

    void PacerLoop() {
        while (!pacer_stop_.load(std::memory_order_relaxed) && !g_stop_requested.load(std::memory_order_relaxed)) {
            std::vector<uint32_t> ids;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ids.reserve(states_.size());
                for (const auto& [id, _] : states_) {
                    ids.push_back(id);
                }
            }
            for (uint32_t id : ids) {
                try {
                    PumpSends(id);
                } catch (const std::exception& ex) {
                    OnTransportError(id, ex.what());
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void PumpSends(uint32_t connection_id) {
        std::shared_ptr<ITransportConnection> connection;
        std::vector<std::vector<uint8_t>> sends;
        bool should_close_send = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = states_.find(connection_id);
            if (it == states_.end()) {
                return;
            }
            auto& state = it->second;
            if (!state.connected || state.connection == nullptr || state.send_closed) {
                return;
            }
            connection = state.connection;

            if (paced_ && state.next_send_time_ns == 0.0) {
                state.next_send_time_ns = static_cast<double>(NowNs());
            }

            while (ShouldSendOnConnection(connection_id) &&
                   !stop_sending_.load(std::memory_order_relaxed) &&
                   (state.next_sequence - state.echoed_messages) < config_.max_inflight) {
                if (paced_) {
                    const double now_ns = static_cast<double>(NowNs());
                    if (state.next_send_time_ns > now_ns) {
                        break;
                    }
                    state.next_send_time_ns += pacing_interval_ns_;
                    if (state.next_send_time_ns + (pacing_interval_ns_ * 4.0) < now_ns) {
                        state.next_send_time_ns = now_ns;
                    }
                }
                std::vector<uint8_t> frame(config_.message_size);
                MessageHeader header{};
                header.magic = kMessageMagic;
                header.sequence = state.next_sequence++;
                header.send_timestamp_ns = NowNs();
                std::memcpy(frame.data(), &header, sizeof(header));
                for (uint32_t i = sizeof(header); i < config_.message_size; ++i) {
                    frame[i] = static_cast<uint8_t>(header.sequence + i);
                }
                sends.push_back(std::move(frame));
            }

            if (stop_sending_.load(std::memory_order_relaxed) &&
                state.next_sequence == state.echoed_messages &&
                !state.send_closed) {
                state.send_closed = true;
                should_close_send = true;
            }
        }

        for (const auto& frame : sends) {
            try {
                connection->SendCopy(frame.data(), frame.size());
                stats_.AddSent(frame.size());
            } catch (const std::exception& ex) {
                connection->Close();
                throw;
            }
        }
        if (should_close_send) {
            try {
                connection->CloseSend();
            } catch (const std::exception&) {
                connection->Close();
                throw;
            }
        }
    }

    const AppConfig& config_;
    Stats& stats_;
    std::mutex mutex_;
    std::condition_variable done_cv_;
    std::map<uint32_t, LoadClientConnectionState> states_;
    std::set<uint32_t> closed_connections_;
    std::atomic<bool> stop_sending_{false};
    const bool paced_{false};
    const double pacing_interval_ns_{0.0};
    std::atomic<bool> pacer_stop_{false};
    std::thread pacer_thread_;
};

class SctpServerTransport : public ITransportRunner {
  public:
    SctpServerTransport(const AppConfig& config, ITransportEventHandler& handler)
        : config_(config), handler_(handler), tls_(config, SctpTlsContext::Role::Server) {}

    ~SctpServerTransport() override {
        Stop();
    }

    void Start() override {
        listeners_.reserve(config_.server_count);
        accept_threads_.reserve(config_.server_count);

        for (uint32_t i = 0; i < config_.server_count; ++i) {
            const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
            if (fd < 0) {
                throw std::runtime_error(SocketErrorString("socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP)"));
            }

            try {
                ConfigureSocket(fd);
                tls_.PrimeSocket(fd, "BIO_new_dgram_sctp(listener)");
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_port = htons(static_cast<uint16_t>(config_.base_port + i));
                if (::inet_pton(AF_INET, config_.bind.c_str(), &address.sin_addr) != 1) {
                    throw std::runtime_error("SCTP server currently supports IPv4 bind addresses only");
                }
                if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
                    throw std::runtime_error(SocketErrorString("bind"));
                }
                if (::listen(fd, 128) != 0) {
                    throw std::runtime_error(SocketErrorString("listen"));
                }
            } catch (...) {
                ::close(fd);
                throw;
            }

            listeners_.push_back(fd);
            accept_threads_.emplace_back([this, fd]() { AcceptLoop(fd); });
        }
    }

    void Stop() override {
        if (stopped_.exchange(true, std::memory_order_relaxed)) {
            return;
        }

        for (int fd : listeners_) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        listeners_.clear();

        for (auto& thread : accept_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        accept_threads_.clear();

        std::vector<std::shared_ptr<SctpConnection>> connections;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [_, connection] : connections_) {
                connections.push_back(connection);
            }
            connections_.clear();
        }
        for (const auto& connection : connections) {
            connection->Close();
        }
        for (const auto& connection : connections) {
            connection->Join();
        }
    }

  private:
    void ConfigureSocket(int fd) const {
        int reuse = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SO_REUSEADDR)"));
        }
        int nodelay = config_.sctp_nodelay ? 1 : 0;
        if (::setsockopt(fd, IPPROTO_SCTP, SCTP_NODELAY, &nodelay, sizeof(nodelay)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SCTP_NODELAY)"));
        }
        sctp_sndinfo sndinfo{};
        sndinfo.snd_sid = config_.sctp_stream_id;
        if (::setsockopt(fd, IPPROTO_SCTP, SCTP_DEFAULT_SNDINFO, &sndinfo, sizeof(sndinfo)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SCTP_DEFAULT_SNDINFO)"));
        }
        if (config_.sctp_tls) {
            sctp_event_subscribe events{};
            events.sctp_data_io_event = 1;
            events.sctp_association_event = 1;
            events.sctp_shutdown_event = 1;
            events.sctp_authentication_event = 1;
            if (::setsockopt(fd, IPPROTO_SCTP, SCTP_EVENTS, &events, sizeof(events)) != 0) {
                throw std::runtime_error(SocketErrorString("setsockopt(SCTP_EVENTS)"));
            }
        }
    }

    void AcceptLoop(int listener_fd) {
        while (!stopped_.load(std::memory_order_relaxed) && !g_stop_requested.load(std::memory_order_relaxed)) {
            const int accepted_fd = ::accept(listener_fd, nullptr, nullptr);
            if (accepted_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (stopped_.load(std::memory_order_relaxed)) {
                    return;
                }
                handler_.OnTransportError(std::numeric_limits<uint32_t>::max(), SocketErrorString("accept"));
                return;
            }

            try {
                ConfigureSocket(accepted_fd);
                SSL* ssl = tls_.CreateAndHandshake(accepted_fd);
                auto connection = std::make_shared<SctpConnection>(
                    accepted_fd,
                    next_connection_id_.fetch_add(1, std::memory_order_relaxed),
                    config_.sctp_stream_id,
                    handler_,
                    ssl);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    connections_[connection->Id()] = connection;
                }
                handler_.OnConnected(connection);
                connection->StartReceiveLoop();
            } catch (const std::exception& ex) {
                ::close(accepted_fd);
                handler_.OnTransportError(std::numeric_limits<uint32_t>::max(), ex.what());
            }
        }
    }

    const AppConfig& config_;
    ITransportEventHandler& handler_;
    std::atomic<bool> stopped_{false};
    std::atomic<uint32_t> next_connection_id_{0};
    std::mutex mutex_;
    std::map<uint32_t, std::shared_ptr<SctpConnection>> connections_;
    std::vector<int> listeners_;
    std::vector<std::thread> accept_threads_;
    SctpTlsContext tls_;
};

class SctpClientTransport : public ITransportRunner {
  public:
    SctpClientTransport(const AppConfig& config, ITransportEventHandler& handler)
        : config_(config), handler_(handler), tls_(config, SctpTlsContext::Role::Client) {}

    ~SctpClientTransport() override {
        Stop();
    }

    void Start() override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t i = 0; i < config_.client_count; ++i) {
            const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
            if (fd < 0) {
                throw std::runtime_error(SocketErrorString("socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP)"));
            }

            try {
                ConfigureSocket(fd);
                tls_.PrimeSocket(fd, "BIO_new_dgram_sctp(pre-connect)");
                sockaddr_in address = ResolveIpv4Address(
                    config_.target,
                    static_cast<uint16_t>(config_.base_port + (i % config_.server_count)),
                    "SCTP client");
                if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
                    throw std::runtime_error(SocketErrorString("connect"));
                }
            } catch (...) {
                ::close(fd);
                throw;
            }

            SSL* ssl = tls_.CreateAndHandshake(fd);
            auto connection = std::make_shared<SctpConnection>(fd, i, config_.sctp_stream_id, handler_, ssl);
            connections_[connection->Id()] = connection;
            handler_.OnConnected(connection);
            connection->StartReceiveLoop();
        }
    }

    void Stop() override {
        std::vector<std::shared_ptr<SctpConnection>> connections;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [_, connection] : connections_) {
                connections.push_back(connection);
            }
            connections_.clear();
        }
        for (const auto& connection : connections) {
            connection->Close();
        }
        for (const auto& connection : connections) {
            connection->Join();
        }
    }

  private:
    void ConfigureSocket(int fd) const {
        int nodelay = config_.sctp_nodelay ? 1 : 0;
        if (::setsockopt(fd, IPPROTO_SCTP, SCTP_NODELAY, &nodelay, sizeof(nodelay)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SCTP_NODELAY)"));
        }
        sctp_sndinfo sndinfo{};
        sndinfo.snd_sid = config_.sctp_stream_id;
        if (::setsockopt(fd, IPPROTO_SCTP, SCTP_DEFAULT_SNDINFO, &sndinfo, sizeof(sndinfo)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SCTP_DEFAULT_SNDINFO)"));
        }
        if (config_.sctp_tls) {
            sctp_event_subscribe events{};
            events.sctp_data_io_event = 1;
            events.sctp_association_event = 1;
            events.sctp_shutdown_event = 1;
            events.sctp_authentication_event = 1;
            if (::setsockopt(fd, IPPROTO_SCTP, SCTP_EVENTS, &events, sizeof(events)) != 0) {
                throw std::runtime_error(SocketErrorString("setsockopt(SCTP_EVENTS)"));
            }
        }
    }

    const AppConfig& config_;
    ITransportEventHandler& handler_;
    std::mutex mutex_;
    std::map<uint32_t, std::shared_ptr<SctpConnection>> connections_;
    SctpTlsContext tls_;
};

class SctpServer {
  public:
    explicit SctpServer(const AppConfig& config)
        : config_(config),
          stats_printer_("server", stats_, config_.stats_interval_ms),
          controller_(config_, stats_),
          transport_(config_, controller_) {}

    void Run() {
        transport_.Start();
        stats_printer_.Start();

        std::cout << "sctp server listening on " << config_.bind << " ports ";
        for (uint32_t i = 0; i < config_.server_count; ++i) {
            if (i != 0) {
                std::cout << ",";
            }
            std::cout << (config_.base_port + i);
        }
        std::cout << std::endl;

        while (!g_stop_requested.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        transport_.Stop();
        stats_printer_.Stop();
        PrintSummary();
    }

  private:
    void PrintSummary() {
        const auto snapshot = stats_.SnapshotNow();
        std::cout << "server summary: "
                  << "tx_messages=" << snapshot.sent_messages
                  << " rx_messages=" << snapshot.recv_messages
                  << " latency_ms(p50/p75/p99)=n/a/n/a/n/a"
                  << std::endl;
    }

    const AppConfig& config_;
    Stats stats_;
    StatsPrinter stats_printer_;
    LoadServerController controller_;
    SctpServerTransport transport_;
};

class SctpClient {
  public:
    explicit SctpClient(const AppConfig& config)
        : config_(config),
          stats_printer_("client", stats_, config_.stats_interval_ms),
          controller_(config_, stats_),
          transport_(config_, controller_) {}

    void Run() {
        const auto deadline = Clock::now() + std::chrono::seconds(config_.duration_sec);
        const auto drain_deadline = deadline + std::chrono::milliseconds(config_.drain_timeout_ms);

        controller_.StartPacer();
        transport_.Start();
        stats_printer_.Start();

        while (!g_stop_requested.load(std::memory_order_relaxed)) {
            const auto now = Clock::now();
            if (now >= deadline && !stop_requested_.exchange(true, std::memory_order_relaxed)) {
                controller_.RequestStopSending();
            }
            if (now >= drain_deadline) {
                break;
            }
            if (controller_.WaitUntilDone(config_.client_count, std::chrono::milliseconds(200))) {
                break;
            }
        }

        controller_.ForceShutdownAll();
        transport_.Stop();
        controller_.StopPacer();
        stats_printer_.Stop();
        PrintSummary();
    }

  private:
    void PrintSummary() {
        const auto snapshot = stats_.SnapshotNow();
        std::cout << "client summary: "
                  << "sent_messages=" << snapshot.sent_messages
                  << " echoed_messages=" << snapshot.recv_messages
                  << " sent_bytes=" << snapshot.sent_bytes
                  << " echoed_bytes=" << snapshot.recv_bytes
                  << " latency_ms(p50/p75/p99)="
                  << FormatLatencySummary(snapshot.latency)
                  << std::endl;
    }

    const AppConfig& config_;
    Stats stats_;
    StatsPrinter stats_printer_;
    LoadClientController controller_;
    SctpClientTransport transport_;
    std::atomic<bool> stop_requested_{false};
};

class MsQuicApi {
  public:
    MsQuicApi() {
        const auto status = MsQuicOpen2(&api_);
        if (QUIC_FAILED(status)) {
            throw std::runtime_error("MsQuicOpen2 failed: " + StatusToHex(status));
        }
    }

    ~MsQuicApi() {
        if (api_ != nullptr) {
            MsQuicClose(api_);
        }
    }

    const QUIC_API_TABLE* operator->() const {
        return api_;
    }

  private:
    const QUIC_API_TABLE* api_{nullptr};
};

class Registration {
  public:
    Registration(const MsQuicApi& api, const char* name) : api_(api) {
        QUIC_REGISTRATION_CONFIG config{name, QUIC_EXECUTION_PROFILE_TYPE_MAX_THROUGHPUT};
        const auto status = api_->RegistrationOpen(&config, &handle_);
        if (QUIC_FAILED(status)) {
            throw std::runtime_error("RegistrationOpen failed: " + StatusToHex(status));
        }
    }

    ~Registration() {
        if (handle_ != nullptr) {
            api_->RegistrationClose(handle_);
        }
    }

    HQUIC get() const {
        return handle_;
    }

  private:
    const MsQuicApi& api_;
    HQUIC handle_{nullptr};
};

class Configuration {
  public:
    Configuration(
        const MsQuicApi& api,
        HQUIC registration,
        const AppConfig& config,
        bool is_client)
        : api_(api) {
        QUIC_SETTINGS settings{};
        settings.IdleTimeoutMs = config.idle_timeout_ms;
        settings.IsSet.IdleTimeoutMs = TRUE;

        if (!is_client) {
            settings.PeerBidiStreamCount = 1;
            settings.IsSet.PeerBidiStreamCount = TRUE;
        }

        QUIC_BUFFER alpn{
            static_cast<uint32_t>(config.alpn.size()),
            reinterpret_cast<uint8_t*>(const_cast<char*>(config.alpn.data()))
        };

        const auto open_status =
            api_->ConfigurationOpen(registration, &alpn, 1, &settings, sizeof(settings), nullptr, &handle_);
        if (QUIC_FAILED(open_status)) {
            throw std::runtime_error("ConfigurationOpen failed: " + StatusToHex(open_status));
        }

        if (is_client) {
            QUIC_CREDENTIAL_CONFIG cred{};
            cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
            cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
            if (!config.verify_peer) {
                cred.Flags =
                    static_cast<QUIC_CREDENTIAL_FLAGS>(cred.Flags | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION);
            }
            const auto load_status = api_->ConfigurationLoadCredential(handle_, &cred);
            if (QUIC_FAILED(load_status)) {
                throw std::runtime_error("ConfigurationLoadCredential(client) failed: " + StatusToHex(load_status));
            }
            return;
        }

        helper_.emplace();
        helper_->cred.Flags = QUIC_CREDENTIAL_FLAG_NONE;
        if (!config.password.empty()) {
            helper_->file_protected.CertificateFile = const_cast<char*>(config.cert_file.c_str());
            helper_->file_protected.PrivateKeyFile = const_cast<char*>(config.key_file.c_str());
            helper_->file_protected.PrivateKeyPassword = const_cast<char*>(config.password.c_str());
            helper_->cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE_PROTECTED;
            helper_->cred.CertificateFileProtected = &helper_->file_protected;
        } else {
            helper_->file.CertificateFile = const_cast<char*>(config.cert_file.c_str());
            helper_->file.PrivateKeyFile = const_cast<char*>(config.key_file.c_str());
            helper_->cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
            helper_->cred.CertificateFile = &helper_->file;
        }

        const auto load_status = api_->ConfigurationLoadCredential(handle_, &helper_->cred);
        if (QUIC_FAILED(load_status)) {
            throw std::runtime_error("ConfigurationLoadCredential(server) failed: " + StatusToHex(load_status));
        }
    }

    ~Configuration() {
        if (handle_ != nullptr) {
            api_->ConfigurationClose(handle_);
        }
    }

    HQUIC get() const {
        return handle_;
    }

  private:
    struct CredentialHelper {
        QUIC_CREDENTIAL_CONFIG cred{};
        QUIC_CERTIFICATE_FILE file{};
        QUIC_CERTIFICATE_FILE_PROTECTED file_protected{};
    };

    const MsQuicApi& api_;
    HQUIC handle_{nullptr};
    std::optional<CredentialHelper> helper_;
};

class Server {
  public:
    explicit Server(const AppConfig& config)
        : config_(config),
          api_(),
          registration_(api_, "msquic-loadtest-server"),
          configuration_(api_, registration_.get(), config_, false),
          stats_printer_("server", stats_, config_.stats_interval_ms) {}

    void Run() {
        StartListeners();
        stats_printer_.Start();

        std::cout << "server listening on " << config_.bind << " ports ";
        for (uint32_t i = 0; i < config_.server_count; ++i) {
            if (i != 0) {
                std::cout << ",";
            }
            std::cout << (config_.base_port + i);
        }
        std::cout << std::endl;

        while (!g_stop_requested.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        for (auto& listener : listeners_) {
            if (listener->handle != nullptr) {
                api_->ListenerClose(listener->handle);
                listener->handle = nullptr;
            }
        }
        listeners_.clear();
        stats_printer_.Stop();
        PrintSummary();
    }

  private:
    struct ServerStreamContext {
        explicit ServerStreamContext(Server& owner) : owner(owner) {}
        Server& owner;
        std::mutex mutex;
        std::vector<uint8_t> receive_buffer;
        uint32_t pending_sends{0};
        bool peer_finished{false};
        bool shutdown_started{false};
    };

    struct ListenerHandle {
        explicit ListenerHandle(HQUIC value) : handle(value) {}
        ~ListenerHandle() = default;
        HQUIC handle{nullptr};
    };

    static QUIC_STATUS QUIC_API ListenerCallback(
        HQUIC,
        void* context,
        QUIC_LISTENER_EVENT* event) {
        return static_cast<Server*>(context)->OnListenerEvent(event);
    }

    static QUIC_STATUS QUIC_API ConnectionCallback(
        HQUIC connection,
        void* context,
        QUIC_CONNECTION_EVENT* event) {
        return static_cast<Server*>(context)->OnConnectionEvent(connection, event);
    }

    static QUIC_STATUS QUIC_API StreamCallback(
        HQUIC stream,
        void* context,
        QUIC_STREAM_EVENT* event) {
        auto* stream_context = static_cast<ServerStreamContext*>(context);
        return stream_context->owner.OnStreamEvent(stream, stream_context, event);
    }

    void StartListeners() {
        listeners_.reserve(config_.server_count);

        for (uint32_t i = 0; i < config_.server_count; ++i) {
            HQUIC listener = nullptr;
            const auto open_status = api_->ListenerOpen(registration_.get(), ListenerCallback, this, &listener);
            if (QUIC_FAILED(open_status)) {
                throw std::runtime_error("ListenerOpen failed: " + StatusToHex(open_status));
            }

            QUIC_ADDR address{};
            const bool parsed =
                QuicAddr4FromString(config_.bind.c_str(), &address) ||
                QuicAddr6FromString(config_.bind.c_str(), &address);
            if (!parsed) {
                api_->ListenerClose(listener);
                throw std::runtime_error("failed to parse bind address: " + config_.bind);
            }
            QuicAddrSetPort(&address, static_cast<uint16_t>(config_.base_port + i));

            QUIC_BUFFER alpn{
                static_cast<uint32_t>(config_.alpn.size()),
                reinterpret_cast<uint8_t*>(const_cast<char*>(config_.alpn.data()))
            };

            const auto start_status = api_->ListenerStart(listener, &alpn, 1, &address);
            if (QUIC_FAILED(start_status)) {
                api_->ListenerClose(listener);
                throw std::runtime_error("ListenerStart failed: " + StatusToHex(start_status));
            }

            listeners_.push_back(std::make_unique<ListenerHandle>(listener));
        }
    }

    QUIC_STATUS OnListenerEvent(QUIC_LISTENER_EVENT* event) {
        if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) {
            return QUIC_STATUS_SUCCESS;
        }

        api_->SetCallbackHandler(event->NEW_CONNECTION.Connection, reinterpret_cast<void*>(ConnectionCallback), this);
        return api_->ConnectionSetConfiguration(event->NEW_CONNECTION.Connection, configuration_.get());
    }

    QUIC_STATUS OnConnectionEvent(HQUIC connection, QUIC_CONNECTION_EVENT* event) {
        switch (event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            std::cerr << "server connection established" << std::endl;
            break;
        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
            std::cerr << "server peer stream started" << std::endl;
            auto* stream_ctx = new ServerStreamContext(*this);
            api_->SetCallbackHandler(event->PEER_STREAM_STARTED.Stream, reinterpret_cast<void*>(StreamCallback), stream_ctx);
            break;
        }
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            std::cerr << "server transport shutdown: "
                      << StatusToHex(event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status) << std::endl;
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            std::cerr << "server peer shutdown: "
                      << event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode << std::endl;
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            api_->ConnectionClose(connection);
            break;
        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    void MaybeShutdownStream(HQUIC stream, ServerStreamContext* context) {
        if (context->peer_finished && context->pending_sends == 0 && !context->shutdown_started) {
            context->shutdown_started = true;
            api_->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        }
    }

    QUIC_STATUS OnStreamEvent(HQUIC stream, ServerStreamContext* context, QUIC_STREAM_EVENT* event) {
        switch (event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE: {
            std::vector<std::unique_ptr<SendBuffer>> sends;
            {
                std::lock_guard<std::mutex> lock(context->mutex);
                for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
                    const auto* buffer = &event->RECEIVE.Buffers[i];
                    context->receive_buffer.insert(
                        context->receive_buffer.end(),
                        buffer->Buffer,
                        buffer->Buffer + buffer->Length);
                }

                while (context->receive_buffer.size() >= config_.message_size) {
                    auto send = std::make_unique<SendBuffer>(config_.message_size);
                    std::memcpy(send->storage.data(), context->receive_buffer.data(), config_.message_size);
                    context->receive_buffer.erase(
                        context->receive_buffer.begin(),
                        context->receive_buffer.begin() + static_cast<std::ptrdiff_t>(config_.message_size));
                    ++context->pending_sends;
                    sends.push_back(std::move(send));
                }

                if ((event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0) {
                    context->peer_finished = true;
                }
            }

            for (auto& send : sends) {
                auto* raw_send = send.release();
                const auto status = api_->StreamSend(
                    stream,
                    &raw_send->quic_buffer,
                    1,
                    QUIC_SEND_FLAG_NONE,
                    raw_send);
                if (QUIC_FAILED(status)) {
                    delete raw_send;
                    return status;
                }
                stats_.AddReceived(config_.message_size);
                stats_.AddSent(config_.message_size);
            }

            {
                std::lock_guard<std::mutex> lock(context->mutex);
                MaybeShutdownStream(stream, context);
            }
            break;
        }
        case QUIC_STREAM_EVENT_SEND_COMPLETE: {
            delete static_cast<SendBuffer*>(event->SEND_COMPLETE.ClientContext);
            std::lock_guard<std::mutex> lock(context->mutex);
            if (context->pending_sends > 0) {
                --context->pending_sends;
            }
            MaybeShutdownStream(stream, context);
            break;
        }
        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
            std::lock_guard<std::mutex> lock(context->mutex);
            context->peer_finished = true;
            MaybeShutdownStream(stream, context);
            break;
        }
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            api_->StreamClose(stream);
            delete context;
            break;
        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    void PrintSummary() {
        const auto snapshot = stats_.SnapshotNow();
        std::cout << "server summary: "
                  << "tx_messages=" << snapshot.sent_messages
                  << " rx_messages=" << snapshot.recv_messages
                  << " latency_ms(p50/p75/p99)="
                  << FormatLatencySummary(snapshot.latency)
                  << std::endl;
    }

    AppConfig config_;
    MsQuicApi api_;
    Registration registration_;
    Configuration configuration_;
    Stats stats_;
    StatsPrinter stats_printer_;
    std::vector<std::unique_ptr<ListenerHandle>> listeners_;
};

class Client {
  public:
    explicit Client(const AppConfig& config)
        : config_(config),
          api_(),
          registration_(api_, "msquic-loadtest-client"),
          configuration_(api_, registration_.get(), config_, true),
          stats_printer_("client", stats_, config_.stats_interval_ms),
          paced_(config.send_pps > 0),
          pacing_interval_ns_(paced_
                                  ? (1'000'000'000.0 * static_cast<double>(config.client_count)) /
                                        static_cast<double>(config.send_pps)
                                  : 0.0) {}

    ~Client() {
        StopPacer();
    }

    void Run() {
        deadline_ = Clock::now() + std::chrono::seconds(config_.duration_sec);
        drain_deadline_ = deadline_ + std::chrono::milliseconds(config_.drain_timeout_ms);

        StartPacer();
        StartConnections();
        stats_printer_.Start();

        while (!g_stop_requested.load(std::memory_order_relaxed)) {
            if (Clock::now() >= deadline_ && !stop_sending_.exchange(true, std::memory_order_relaxed)) {
                for (auto& connection : connections_) {
                    RequestStopSending(*connection);
                }
            }

            if (Clock::now() >= drain_deadline_) {
                break;
            }

            std::unique_lock<std::mutex> lock(done_mutex_);
            if (done_cv_.wait_for(lock, std::chrono::milliseconds(200), [this]() {
                    return active_connections_.load(std::memory_order_relaxed) == 0;
                })) {
                break;
            }
        }

        for (auto& connection : connections_) {
            ForceShutdown(*connection);
        }

        WaitForConnectionsToClose();
        StopPacer();
        stats_printer_.Stop();
        PrintSummary();
    }

  private:
    struct ConnectionContext;

    struct StreamContext {
        explicit StreamContext(ConnectionContext& owner) : owner(owner) {}
        ConnectionContext& owner;
        std::mutex mutex;
        std::vector<uint8_t> receive_buffer;
        uint64_t next_sequence{0};
        uint64_t echoed_messages{0};
        double next_send_time_ns{0.0};
        bool stream_started{false};
        bool shutdown_started{false};
    };

    struct ConnectionContext {
        ConnectionContext(Client& owner, uint32_t index, uint16_t port)
            : owner(owner), index(index), port(port) {}

        Client& owner;
        uint32_t index;
        uint16_t port;
        HQUIC connection{nullptr};
        HQUIC stream{nullptr};
        std::mutex mutex;
        std::unique_ptr<StreamContext> stream_ctx;
        bool connected{false};
        bool closed{false};
    };

    static QUIC_STATUS QUIC_API ConnectionCallback(
        HQUIC connection,
        void* context,
        QUIC_CONNECTION_EVENT* event) {
        return static_cast<ConnectionContext*>(context)->owner.OnConnectionEvent(
            *static_cast<ConnectionContext*>(context),
            connection,
            event);
    }

    static QUIC_STATUS QUIC_API StreamCallback(
        HQUIC stream,
        void* context,
        QUIC_STREAM_EVENT* event) {
        return static_cast<StreamContext*>(context)->owner.owner.OnStreamEvent(
            static_cast<StreamContext*>(context)->owner,
            stream,
            static_cast<StreamContext*>(context),
            event);
    }

    void StartConnections() {
        connections_.reserve(config_.client_count);
        active_connections_.store(config_.client_count, std::memory_order_relaxed);

        for (uint32_t i = 0; i < config_.client_count; ++i) {
            const auto port = static_cast<uint16_t>(config_.base_port + (i % config_.server_count));
            auto connection = std::make_unique<ConnectionContext>(*this, i, port);

            const auto open_status =
                api_->ConnectionOpen(registration_.get(), ConnectionCallback, connection.get(), &connection->connection);
            if (QUIC_FAILED(open_status)) {
                throw std::runtime_error("ConnectionOpen failed: " + StatusToHex(open_status));
            }

            const auto start_status = api_->ConnectionStart(
                connection->connection,
                configuration_.get(),
                QUIC_ADDRESS_FAMILY_UNSPEC,
                config_.target.c_str(),
                port);
            if (QUIC_FAILED(start_status)) {
                api_->ConnectionClose(connection->connection);
                throw std::runtime_error("ConnectionStart failed: " + StatusToHex(start_status));
            }

            connections_.push_back(std::move(connection));
        }
    }

    void WaitForConnectionsToClose() {
        std::unique_lock<std::mutex> lock(done_mutex_);
        done_cv_.wait_for(lock, std::chrono::seconds(5), [this]() {
            return active_connections_.load(std::memory_order_relaxed) == 0;
        });
    }

    bool ShouldSendOnConnection(const ConnectionContext& connection) const {
        return config_.send_server_index < 0 ||
               (connection.index % config_.server_count) == static_cast<uint32_t>(config_.send_server_index);
    }

    void StartPacer() {
        if (!paced_) {
            return;
        }
        pacer_stop_.store(false, std::memory_order_relaxed);
        pacer_thread_ = std::thread([this]() { PacerLoop(); });
    }

    void StopPacer() {
        pacer_stop_.store(true, std::memory_order_relaxed);
        if (pacer_thread_.joinable()) {
            pacer_thread_.join();
        }
    }

    void PacerLoop() {
        while (!pacer_stop_.load(std::memory_order_relaxed) && !g_stop_requested.load(std::memory_order_relaxed)) {
            for (auto& connection : connections_) {
                std::lock_guard<std::mutex> lock(connection->mutex);
                if (connection->stream_ctx != nullptr && connection->stream != nullptr) {
                    PumpSends(*connection, connection->stream, *connection->stream_ctx);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void StartStream(ConnectionContext& connection) {
        std::lock_guard<std::mutex> lock(connection.mutex);
        if (connection.stream != nullptr) {
            return;
        }

        connection.stream_ctx = std::make_unique<StreamContext>(connection);
        HQUIC stream = nullptr;
        const auto open_status =
            api_->StreamOpen(connection.connection, QUIC_STREAM_OPEN_FLAG_NONE, StreamCallback, connection.stream_ctx.get(), &stream);
        if (QUIC_FAILED(open_status)) {
            std::cerr << "StreamOpen failed: " << StatusToHex(open_status) << std::endl;
            api_->ConnectionShutdown(connection.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 1);
            return;
        }

        connection.stream = stream;
        const auto start_status = api_->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
        if (QUIC_FAILED(start_status)) {
            connection.stream = nullptr;
            api_->StreamClose(stream);
            std::cerr << "StreamStart failed: " << StatusToHex(start_status) << std::endl;
            api_->ConnectionShutdown(connection.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 1);
            return;
        }
    }

    void PumpSends(ConnectionContext& connection, HQUIC stream, StreamContext& stream_ctx) {
        std::lock_guard<std::mutex> lock(stream_ctx.mutex);
        if (!stream_ctx.stream_started || stream_ctx.shutdown_started) {
            return;
        }

        while (ShouldSendOnConnection(connection) &&
               !stop_sending_.load(std::memory_order_relaxed) &&
               (stream_ctx.next_sequence - stream_ctx.echoed_messages) < config_.max_inflight) {
            if (paced_) {
                const double now_ns = static_cast<double>(NowNs());
                if (stream_ctx.next_send_time_ns == 0.0) {
                    stream_ctx.next_send_time_ns = now_ns;
                }
                if (stream_ctx.next_send_time_ns > now_ns) {
                    break;
                }
                stream_ctx.next_send_time_ns += pacing_interval_ns_;
                if (stream_ctx.next_send_time_ns + (pacing_interval_ns_ * 4.0) < now_ns) {
                    stream_ctx.next_send_time_ns = now_ns;
                }
            }

            auto send = std::make_unique<SendBuffer>(config_.message_size);

            MessageHeader header{};
            header.magic = kMessageMagic;
            header.sequence = stream_ctx.next_sequence++;
            header.send_timestamp_ns = NowNs();
            std::memcpy(send->storage.data(), &header, sizeof(header));

            for (uint32_t i = sizeof(header); i < config_.message_size; ++i) {
                send->storage[i] = static_cast<uint8_t>(header.sequence + i);
            }

            auto* raw_send = send.release();
            const auto status = api_->StreamSend(stream, &raw_send->quic_buffer, 1, QUIC_SEND_FLAG_NONE, raw_send);
            if (QUIC_FAILED(status)) {
                delete raw_send;
                std::cerr << "StreamSend failed: " << StatusToHex(status) << std::endl;
                api_->ConnectionShutdown(connection.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 1);
                return;
            }
            stats_.AddSent(config_.message_size);
        }

        if (stop_sending_.load(std::memory_order_relaxed) &&
            !stream_ctx.shutdown_started &&
            stream_ctx.next_sequence == stream_ctx.echoed_messages) {
            stream_ctx.shutdown_started = true;
            api_->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        }
    }

    void RequestStopSending(ConnectionContext& connection) {
        std::lock_guard<std::mutex> lock(connection.mutex);
        if (connection.stream_ctx != nullptr && connection.stream != nullptr) {
            PumpSends(connection, connection.stream, *connection.stream_ctx);
        } else if (connection.connection != nullptr) {
            api_->ConnectionShutdown(connection.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        }
    }

    void ForceShutdown(ConnectionContext& connection) {
        std::lock_guard<std::mutex> lock(connection.mutex);
        if (!connection.closed && connection.connection != nullptr) {
            api_->ConnectionShutdown(connection.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        }
    }

    QUIC_STATUS OnConnectionEvent(ConnectionContext& connection, HQUIC quic_connection, QUIC_CONNECTION_EVENT* event) {
        switch (event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            std::cerr << "client connection " << connection.index << " established" << std::endl;
            connection.connected = true;
            StartStream(connection);
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            std::cerr << "client connection " << connection.index
                      << " transport shutdown: "
                      << StatusToHex(event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status) << std::endl;
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            std::cerr << "client connection " << connection.index
                      << " peer shutdown: "
                      << event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode << std::endl;
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            if (!connection.closed) {
                connection.closed = true;
                api_->ConnectionClose(quic_connection);
                active_connections_.fetch_sub(1, std::memory_order_relaxed);
                done_cv_.notify_all();
            }
            break;
        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    QUIC_STATUS OnStreamEvent(
        ConnectionContext& connection,
        HQUIC stream,
        StreamContext* stream_ctx,
        QUIC_STREAM_EVENT* event) {
        switch (event->Type) {
        case QUIC_STREAM_EVENT_START_COMPLETE: {
            if (QUIC_FAILED(event->START_COMPLETE.Status)) {
                std::cerr << "Stream start completion failed: "
                          << StatusToHex(event->START_COMPLETE.Status) << std::endl;
                api_->ConnectionShutdown(connection.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 1);
                break;
            }
            {
                std::lock_guard<std::mutex> lock(stream_ctx->mutex);
                stream_ctx->stream_started = true;
            }
            if (!paced_) {
                PumpSends(connection, stream, *stream_ctx);
            }
            break;
        }
        case QUIC_STREAM_EVENT_RECEIVE: {
            {
                std::lock_guard<std::mutex> lock(stream_ctx->mutex);
                for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
                    const auto* buffer = &event->RECEIVE.Buffers[i];
                    stream_ctx->receive_buffer.insert(
                        stream_ctx->receive_buffer.end(),
                        buffer->Buffer,
                        buffer->Buffer + buffer->Length);
                }

                while (stream_ctx->receive_buffer.size() >= config_.message_size) {
                    MessageHeader header{};
                    std::memcpy(&header, stream_ctx->receive_buffer.data(), sizeof(header));
                    if (header.magic != kMessageMagic) {
                        std::cerr << "invalid echoed frame magic on client " << connection.index << std::endl;
                        api_->ConnectionShutdown(connection.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 1);
                        return QUIC_STATUS_PROTOCOL_ERROR;
                    }

                    const auto latency_ns = NowNs() - header.send_timestamp_ns;
                    stats_.AddReceived(config_.message_size);
                    stats_.AddLatencyNs(latency_ns);
                    ++stream_ctx->echoed_messages;

                    stream_ctx->receive_buffer.erase(
                        stream_ctx->receive_buffer.begin(),
                        stream_ctx->receive_buffer.begin() + static_cast<std::ptrdiff_t>(config_.message_size));
                }
            }

            if (!paced_) {
                PumpSends(connection, stream, *stream_ctx);
            }
            break;
        }
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            delete static_cast<SendBuffer*>(event->SEND_COMPLETE.ClientContext);
            break;
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            api_->StreamClose(stream);
            api_->ConnectionShutdown(connection.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            break;
        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    void PrintSummary() {
        const auto snapshot = stats_.SnapshotNow();
        std::cout << "client summary: "
                  << "sent_messages=" << snapshot.sent_messages
                  << " echoed_messages=" << snapshot.recv_messages
                  << " sent_bytes=" << snapshot.sent_bytes
                  << " echoed_bytes=" << snapshot.recv_bytes
                  << " latency_ms(p50/p75/p99)="
                  << FormatLatencySummary(snapshot.latency)
                  << std::endl;
    }

    AppConfig config_;
    MsQuicApi api_;
    Registration registration_;
    Configuration configuration_;
    Stats stats_;
    StatsPrinter stats_printer_;
    std::vector<std::unique_ptr<ConnectionContext>> connections_;
    std::atomic<uint32_t> active_connections_{0};
    std::mutex done_mutex_;
    std::condition_variable done_cv_;
    std::atomic<bool> stop_sending_{false};
    const bool paced_{false};
    const double pacing_interval_ns_{0.0};
    std::atomic<bool> pacer_stop_{false};
    std::thread pacer_thread_;
    Clock::time_point deadline_{};
    Clock::time_point drain_deadline_{};
};

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    try {
        const auto args = ParseArgs(argc, argv);
        if (args.values.count("help") != 0U) {
            PrintUsage();
            return 0;
        }

        const auto config = LoadConfig(args);
        if (config.protocol == Protocol::Sctp) {
            if (config.mode == "server") {
                SctpServer server(config);
                server.Run();
            } else {
                SctpClient client(config);
                client.Run();
            }
        } else {
            if (config.mode == "server") {
                Server server(config);
                server.Run();
            } else {
                Client client(config);
                client.Run();
            }
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        PrintUsage();
        return 1;
    }
}
