#include "lsquic_backend.h"

namespace proto_test {

#ifdef HAVE_LSQUIC
class LsQuicEndpoint {
  public:
    explicit LsQuicEndpoint(const AppConfig& config, bool server)
        : config_(config),
          is_server_(server),
          trace_(config, server ? "amf" : "ran"),
          stats_printer_(server ? "server" : "client", stats_, config.stats_interval_ms) {
        if (!config_.qualcomm_method) {
            throw std::runtime_error("--protocol=lsquic is implemented only for --qualcomm-method=1");
        }
        if (config_.sctp_tls) {
            throw std::runtime_error("--protocol=lsquic does not use SCTP TLS options");
        }
        InitSsl();
        InitSocket();
        InitEngine();
    }

    ~LsQuicEndpoint() {
        if (engine_ != nullptr) {
            lsquic_engine_destroy(engine_);
        }
        if (ssl_ctx_ != nullptr) {
            SSL_CTX_free(ssl_ctx_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
        lsquic_global_cleanup();
    }

    void Run() {
        if (is_server_) {
            std::cout << "lsquic server listening on " << config_.bind << " port " << config_.base_port << std::endl;
        } else {
            StartClientConnection();
        }

        while (!g_stop_requested.load(std::memory_order_relaxed) && !Done()) {
            MaybeQueueDelayedInitial();
            PumpEngine();
            pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLIN;
            const int timeout_ms = PollTimeoutMs();
            const int rc = ::poll(&pfd, 1, timeout_ms);
            if (rc > 0 && (pfd.revents & POLLIN) != 0) {
                ReadPackets();
            } else if (rc < 0 && errno != EINTR) {
                throw std::runtime_error(SocketErrorString("poll(lsquic)"));
            }
            MaybeQueueDelayedInitial();
            PumpEngine();
        }

        if (conn_ != nullptr) {
            lsquic_conn_close(conn_);
            PumpEngine();
        }
        PrintSummary();
    }

  private:
    struct ConnCtx {
        LsQuicEndpoint* endpoint{nullptr};
        lsquic_conn_t* conn{nullptr};
    };

    struct PendingFrame {
        std::vector<uint8_t> frame;
        const char* event{nullptr};
        uint64_t sequence{0};
        uint8_t hop_in{0};
        uint8_t hop_out{0};
        uint64_t entry_realtime_ns{0};
        uint64_t entry_mono_ns{0};
        bool logged{false};
    };

    struct StreamCtx {
        LsQuicEndpoint* endpoint{nullptr};
        lsquic_stream_t* stream{nullptr};
        std::vector<uint8_t> receive_buffer;
        std::deque<PendingFrame> pending;
        uint64_t next_sequence{0};
        uint64_t echoed_messages{0};
        uint64_t final_messages_sent{0};
        uint64_t next_initial_ready_ns{0};
    };

    static lsquic_conn_ctx_t* OnNewConn(void* stream_if_ctx, lsquic_conn_t* conn) {
        auto* endpoint = static_cast<LsQuicEndpoint*>(stream_if_ctx);
        auto* ctx = new ConnCtx{endpoint, conn};
        endpoint->conn_ = conn;
        if (!endpoint->is_server_) {
            lsquic_conn_make_stream(conn);
        }
        return reinterpret_cast<lsquic_conn_ctx_t*>(ctx);
    }

    static void OnConnClosed(lsquic_conn_t* conn) {
        auto* ctx = reinterpret_cast<ConnCtx*>(lsquic_conn_get_ctx(conn));
        if (ctx != nullptr) {
            ctx->endpoint->conn_ = nullptr;
            lsquic_conn_set_ctx(conn, nullptr);
            delete ctx;
        }
    }

    static lsquic_stream_ctx_t* OnNewStream(void* stream_if_ctx, lsquic_stream_t* stream) {
        auto* endpoint = static_cast<LsQuicEndpoint*>(stream_if_ctx);
        auto* ctx = new StreamCtx{endpoint, stream};
        endpoint->stream_ = stream;
        endpoint->stream_ctx_ = ctx;
        if (endpoint->is_server_) {
            lsquic_stream_wantread(stream, 1);
        } else {
            endpoint->QueueInitial(*ctx);
            lsquic_stream_wantwrite(stream, 1);
        }
        return reinterpret_cast<lsquic_stream_ctx_t*>(ctx);
    }

    static void OnRead(lsquic_stream_t* stream, lsquic_stream_ctx_t* raw_ctx) {
        auto* ctx = reinterpret_cast<StreamCtx*>(raw_ctx);
        ctx->endpoint->HandleRead(stream, *ctx);
    }

    static void OnWrite(lsquic_stream_t* stream, lsquic_stream_ctx_t* raw_ctx) {
        auto* ctx = reinterpret_cast<StreamCtx*>(raw_ctx);
        ctx->endpoint->HandleWrite(stream, *ctx);
    }

    static void OnClose(lsquic_stream_t*, lsquic_stream_ctx_t* raw_ctx) {
        auto* ctx = reinterpret_cast<StreamCtx*>(raw_ctx);
        if (ctx != nullptr) {
            ctx->endpoint->stream_ = nullptr;
            ctx->endpoint->stream_ctx_ = nullptr;
            delete ctx;
        }
    }

    static int PacketsOut(void* packets_out_ctx, const lsquic_out_spec* out_spec, unsigned n_packets_out) {
        auto* endpoint = static_cast<LsQuicEndpoint*>(packets_out_ctx);
        unsigned sent = 0;
        for (; sent < n_packets_out; ++sent) {
            msghdr msg{};
            msg.msg_name = const_cast<sockaddr*>(out_spec[sent].dest_sa);
            msg.msg_namelen = SockaddrLen(*out_spec[sent].dest_sa);
            msg.msg_iov = out_spec[sent].iov;
            msg.msg_iovlen = out_spec[sent].iovlen;
            const ssize_t rc = ::sendmsg(endpoint->fd_, &msg, 0);
            if (rc < 0) {
                return sent == 0 ? -1 : static_cast<int>(sent);
            }
        }
        return static_cast<int>(sent);
    }

    static SSL_CTX* LookupCert(void* ctx, const sockaddr*, const char*) {
        return static_cast<LsQuicEndpoint*>(ctx)->ssl_ctx_;
    }

    static SSL_CTX* GetSslCtx(void* ctx, const sockaddr*) {
        return static_cast<LsQuicEndpoint*>(ctx)->ssl_ctx_;
    }

    static int SelectAlpn(SSL*, const unsigned char** out, unsigned char* outlen,
                          const unsigned char* in, unsigned int inlen, void* arg) {
        const auto* endpoint = static_cast<LsQuicEndpoint*>(arg);
        const std::string& alpn = endpoint->config_.alpn;
        const unsigned char* p = in;
        const unsigned char* end = in + inlen;
        while (p < end) {
            const unsigned int len = *p++;
            if (p + len <= end && len == alpn.size() && std::memcmp(p, alpn.data(), len) == 0) {
                *out = p;
                *outlen = static_cast<unsigned char>(len);
                return SSL_TLSEXT_ERR_OK;
            }
            p += len;
        }
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }

    static socklen_t SockaddrLen(const sockaddr& address) {
        return address.sa_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    }

    void InitSsl() {
        ssl_ctx_ = SSL_CTX_new(TLS_method());
        if (ssl_ctx_ == nullptr) {
            throw std::runtime_error(OpenSslErrorString("SSL_CTX_new"));
        }
        SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ssl_ctx_, TLS1_3_VERSION);
        SSL_CTX_set_default_verify_paths(ssl_ctx_);
        if (is_server_) {
            if (SSL_CTX_use_certificate_chain_file(ssl_ctx_, config_.cert_file.c_str()) != 1) {
                throw std::runtime_error(OpenSslErrorString("SSL_CTX_use_certificate_chain_file"));
            }
            if (SSL_CTX_use_PrivateKey_file(ssl_ctx_, config_.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
                throw std::runtime_error(OpenSslErrorString("SSL_CTX_use_PrivateKey_file"));
            }
            SSL_CTX_set_alpn_select_cb(ssl_ctx_, SelectAlpn, this);
        } else if (!config_.verify_peer) {
            SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);
        }
    }

    void InitSocket() {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) {
            throw std::runtime_error(SocketErrorString("socket(AF_INET, SOCK_DGRAM)"));
        }
        int reuse = 1;
        (void)::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags >= 0) {
            (void)::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        }

        sockaddr_in bind_address{};
        bind_address.sin_family = AF_INET;
        bind_address.sin_port = htons(is_server_ ? config_.base_port : 0);
        const std::string bind_host = is_server_ ? config_.bind : "0.0.0.0";
        if (::inet_pton(AF_INET, bind_host.c_str(), &bind_address.sin_addr) != 1) {
            throw std::runtime_error("LSQUIC currently supports IPv4 addresses only");
        }
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&bind_address), sizeof(bind_address)) != 0) {
            throw std::runtime_error(SocketErrorString("bind(lsquic)"));
        }

        local_len_ = sizeof(local_addr_);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&local_addr_), &local_len_) != 0) {
            throw std::runtime_error(SocketErrorString("getsockname(lsquic)"));
        }

        if (!is_server_) {
            const sockaddr_in peer = ResolveIpv4Address(config_.target, config_.base_port, "LSQUIC client");
            std::memcpy(&peer_addr_, &peer, sizeof(peer));
            peer_len_ = sizeof(peer);
        }
    }

    void InitEngine() {
        const int global_flags = is_server_ ? LSQUIC_GLOBAL_SERVER : LSQUIC_GLOBAL_CLIENT;
        if (lsquic_global_init(global_flags) != 0) {
            throw std::runtime_error("lsquic_global_init failed");
        }
        const unsigned engine_flags = is_server_ ? LSENG_SERVER : 0;
        lsquic_engine_init_settings(&settings_, engine_flags);
        settings_.es_pace_packets = 0;
        settings_.es_max_streams_in = 1;
        settings_.es_init_max_streams_bidi = 1;
        settings_.es_idle_conn_to = config_.idle_timeout_ms * 1000;
        settings_.es_scid_len = 8;

        api_.ea_settings = &settings_;
        api_.ea_stream_if = &stream_if_;
        api_.ea_stream_if_ctx = this;
        api_.ea_packets_out = PacketsOut;
        api_.ea_packets_out_ctx = this;
        api_.ea_alpn = config_.alpn.c_str();
        api_.ea_get_ssl_ctx = GetSslCtx;
        if (is_server_) {
            api_.ea_lookup_cert = LookupCert;
            api_.ea_cert_lu_ctx = this;
        }

        engine_ = lsquic_engine_new(engine_flags, &api_);
        if (engine_ == nullptr) {
            throw std::runtime_error("lsquic_engine_new failed");
        }
    }

    void StartClientConnection() {
        conn_ = lsquic_engine_connect(
            engine_,
            N_LSQVER,
            reinterpret_cast<sockaddr*>(&local_addr_),
            reinterpret_cast<sockaddr*>(&peer_addr_),
            this,
            nullptr,
            config_.target.c_str(),
            0,
            nullptr,
            0,
            nullptr,
            0);
        if (conn_ == nullptr) {
            throw std::runtime_error("lsquic_engine_connect failed");
        }
        std::cout << "lsquic client connecting to " << config_.target << ":" << config_.base_port << std::endl;
    }

    void PumpEngine() {
        lsquic_engine_process_conns(engine_);
        while (lsquic_engine_has_unsent_packets(engine_)) {
            lsquic_engine_send_unsent_packets(engine_);
        }
    }

    int PollTimeoutMs() {
        int diff_us = 0;
        if (lsquic_engine_earliest_adv_tick(engine_, &diff_us)) {
            if (diff_us <= 0) {
                return 0;
            }
            return std::max(1, std::min(20, diff_us / 1000));
        }
        return 20;
    }

    void ReadPackets() {
        while (true) {
            std::array<uint8_t, 4096> buffer{};
            sockaddr_storage peer{};
            socklen_t peer_len = sizeof(peer);
            const ssize_t bytes = ::recvfrom(
                fd_,
                buffer.data(),
                buffer.size(),
                0,
                reinterpret_cast<sockaddr*>(&peer),
                &peer_len);
            if (bytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    return;
                }
                throw std::runtime_error(SocketErrorString("recvfrom(lsquic)"));
            }
            if (is_server_ && peer_len_ == 0) {
                std::memcpy(&peer_addr_, &peer, peer_len);
                peer_len_ = peer_len;
            }
            if (lsquic_engine_packet_in(
                    engine_,
                    buffer.data(),
                    static_cast<size_t>(bytes),
                    reinterpret_cast<sockaddr*>(&local_addr_),
                    reinterpret_cast<sockaddr*>(&peer),
                    this,
                    0) < 0) {
                throw std::runtime_error("lsquic_engine_packet_in failed");
            }
        }
    }

    void HandleRead(lsquic_stream_t* stream, StreamCtx& ctx) {
        std::array<uint8_t, 4096> buffer{};
        while (true) {
            const ssize_t bytes = lsquic_stream_read(stream, buffer.data(), buffer.size());
            if (bytes > 0) {
                ctx.receive_buffer.insert(ctx.receive_buffer.end(), buffer.begin(), buffer.begin() + bytes);
                ProcessFrames(stream, ctx);
                continue;
            }
            if (bytes == 0) {
                return;
            }
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                break;
            }
            if ((is_server_ && server_final_receives_ >= config_.message_count) ||
                (!is_server_ && client_final_sent_all_) ||
                errno == 0) {
                return;
            }
            throw std::runtime_error("lsquic_stream_read failed");
        }
        lsquic_stream_wantread(stream, 1);
    }

    void ProcessFrames(lsquic_stream_t* stream, StreamCtx& ctx) {
        while (true) {
            const auto frame_size = TryPeekFrameSize(ctx.receive_buffer, config_.message_size);
            if (!frame_size.has_value()) {
                return;
            }
            std::vector<uint8_t> frame(*frame_size);
            std::memcpy(frame.data(), ctx.receive_buffer.data(), *frame_size);
            ctx.receive_buffer.erase(
                ctx.receive_buffer.begin(),
                ctx.receive_buffer.begin() + static_cast<std::ptrdiff_t>(*frame_size));

            MessageHeader header{};
            std::memcpy(&header, frame.data(), sizeof(header));
            const uint64_t entry_realtime_ns = RealtimeNs();
            const uint64_t entry_mono_ns = NowNs();
            const uint8_t hop_in = QualcommHop(frame);

            if (is_server_) {
                stats_.AddReceived(frame.size());
                if (hop_in == kQualcommHopInitial) {
                    SetQualcommHop(frame, kQualcommHopReflected);
                    ctx.pending.push_back(PendingFrame{
                        std::move(frame),
                        "amf_turnaround",
                        header.sequence,
                        hop_in,
                        kQualcommHopReflected,
                        entry_realtime_ns,
                        entry_mono_ns,
                        false,
                    });
                    lsquic_stream_wantwrite(stream, 1);
                } else if (hop_in == kQualcommHopFinal) {
                    trace_.Log(
                        "amf_final_receive",
                        header.sequence,
                        static_cast<uint32_t>(frame.size()),
                        hop_in,
                        hop_in,
                        entry_realtime_ns,
                        RealtimeNs(),
                        entry_mono_ns,
                        NowNs());
                    ++server_final_receives_;
                    if (server_final_receives_ >= config_.message_count && conn_ != nullptr) {
                        lsquic_conn_close(conn_);
                    }
                }
            } else if (hop_in == kQualcommHopReflected) {
                SetQualcommHop(frame, kQualcommHopFinal);
                ++ctx.echoed_messages;
                ++ctx.final_messages_sent;
                stats_.AddReceived(frame.size());
                ctx.pending.push_back(PendingFrame{
                    std::move(frame),
                    "ran_turnaround",
                    header.sequence,
                    hop_in,
                    kQualcommHopFinal,
                    entry_realtime_ns,
                    entry_mono_ns,
                    false,
                });
                lsquic_stream_wantwrite(stream, 1);
            }
        }
    }

    void HandleWrite(lsquic_stream_t* stream, StreamCtx& ctx) {
        while (!ctx.pending.empty()) {
            auto& pending = ctx.pending.front();
            if (!pending.logged && pending.event != nullptr) {
                const uint64_t exit_mono_ns = NowNs();
                const uint64_t exit_realtime_ns = RealtimeNs();
                trace_.Log(
                    pending.event,
                    pending.sequence,
                    static_cast<uint32_t>(pending.frame.size()),
                    pending.hop_in,
                    pending.hop_out,
                    pending.entry_realtime_ns,
                    exit_realtime_ns,
                    pending.entry_mono_ns,
                    exit_mono_ns);
                MessageHeader header{};
                std::memcpy(&header, pending.frame.data(), sizeof(header));
                header.send_timestamp_ns = exit_mono_ns;
                std::memcpy(pending.frame.data(), &header, sizeof(header));
                pending.logged = true;
            }
            const ssize_t written = lsquic_stream_write(stream, pending.frame.data(), pending.frame.size());
            if (written < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    break;
                }
                throw std::runtime_error("lsquic_stream_write failed");
            }
            if (static_cast<size_t>(written) != pending.frame.size()) {
                throw std::runtime_error("partial LSQUIC stream write in fast-test mode");
            }
            stats_.AddSent(pending.frame.size());
            const bool sent_final = !is_server_ && pending.hop_out == kQualcommHopFinal;
            ctx.pending.pop_front();
            if (sent_final) {
                if (ctx.final_messages_sent >= config_.message_count) {
                    client_final_sent_all_ = true;
                    client_final_sent_time_ = Clock::now();
                } else if (config_.qualcomm_gap_ms > 0) {
                    ctx.next_initial_ready_ns = NowNs() + (config_.qualcomm_gap_ms * 1'000'000ULL);
                } else {
                    QueueInitial(ctx);
                }
            }
        }
        lsquic_stream_flush(stream);
        lsquic_stream_wantwrite(stream, ctx.pending.empty() ? 0 : 1);
        lsquic_stream_wantread(stream, 1);
    }

    void MaybeQueueDelayedInitial() {
        if (is_server_ || stream_ == nullptr || stream_ctx_ == nullptr ||
            config_.qualcomm_gap_ms == 0 || stream_ctx_->next_initial_ready_ns == 0) {
            return;
        }
        if (NowNs() < stream_ctx_->next_initial_ready_ns) {
            return;
        }
        stream_ctx_->next_initial_ready_ns = 0;
        QueueInitial(*stream_ctx_);
        lsquic_stream_wantwrite(stream_, 1);
    }

    void QueueInitial(StreamCtx& ctx) {
        if (ctx.next_sequence >= config_.message_count) {
            return;
        }
        std::vector<uint8_t> frame(config_.message_size);
        MessageHeader header{};
        header.magic = kMessageMagic;
        header.reserved = static_cast<uint32_t>(frame.size());
        header.sequence = ctx.next_sequence++;
        header.send_timestamp_ns = NowNs();
        std::memcpy(frame.data(), &header, sizeof(header));
        for (uint32_t i = sizeof(header); i < frame.size(); ++i) {
            frame[i] = static_cast<uint8_t>(header.sequence + i);
        }
        SetQualcommHop(frame, kQualcommHopInitial);
        ctx.pending.push_back(PendingFrame{
            std::move(frame),
            nullptr,
            header.sequence,
            kQualcommHopInitial,
            kQualcommHopInitial,
            0,
            0,
            true,
        });
    }

    bool Done() const {
        if (is_server_) {
            return server_final_receives_ >= config_.message_count;
        }
        return client_final_sent_all_ &&
               Clock::now() - client_final_sent_time_ >= std::chrono::milliseconds(200);
    }

    void PrintSummary() {
        const auto snapshot = stats_.SnapshotNow();
        if (is_server_) {
            std::cout << "server summary: "
                      << "tx_messages=" << snapshot.sent_messages
                      << " rx_messages=" << snapshot.recv_messages
                      << " latency_ms(p50/p75/p99)=n/a/n/a/n/a"
                      << std::endl;
        } else {
            std::cout << "client summary: "
                      << "sent_messages=" << snapshot.sent_messages
                      << " echoed_messages=" << snapshot.recv_messages
                      << " sent_bytes=" << snapshot.sent_bytes
                      << " echoed_bytes=" << snapshot.recv_bytes
                      << " latency_ms(p50/p75/p99)=n/a/n/a/n/a"
                      << std::endl;
        }
    }

    const AppConfig& config_;
    bool is_server_{false};
    QualcommTraceWriter trace_;
    Stats stats_;
    StatsPrinter stats_printer_;
    int fd_{-1};
    sockaddr_storage local_addr_{};
    socklen_t local_len_{0};
    sockaddr_storage peer_addr_{};
    socklen_t peer_len_{0};
    SSL_CTX* ssl_ctx_{nullptr};
    lsquic_engine_settings settings_{};
    lsquic_engine_api api_{};
    lsquic_engine_t* engine_{nullptr};
    lsquic_conn_t* conn_{nullptr};
    lsquic_stream_t* stream_{nullptr};
    StreamCtx* stream_ctx_{nullptr};
    uint64_t server_final_receives_{0};
    bool client_final_sent_all_{false};
    Clock::time_point client_final_sent_time_{};
    static const lsquic_stream_if stream_if_;
};

const lsquic_stream_if LsQuicEndpoint::stream_if_ = {
    .on_new_conn = LsQuicEndpoint::OnNewConn,
    .on_goaway_received = nullptr,
    .on_conn_closed = LsQuicEndpoint::OnConnClosed,
    .on_new_stream = LsQuicEndpoint::OnNewStream,
    .on_read = LsQuicEndpoint::OnRead,
    .on_write = LsQuicEndpoint::OnWrite,
    .on_close = LsQuicEndpoint::OnClose,
};
#endif

#ifdef HAVE_LSQUIC
void RunLsQuicEndpoint(const AppConfig& config, bool server) {
    LsQuicEndpoint endpoint(config, server);
    endpoint.Run();
}
#else
void RunLsQuicEndpoint(const AppConfig&, bool) {
    throw std::runtime_error("LSQUIC backend is not built; set -DLSQUIC_ROOT and -DBORINGSSL_ROOT or use --protocol=sctp");
}
#endif

} // namespace proto_test
