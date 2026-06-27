#include "msquic_backend.h"

namespace proto_test {

#ifdef HAVE_MSQUIC
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
            settings.PeerBidiStreamCount = config.stream_count;
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
          trace_(config, "amf"),
          stats_printer_("server", stats_, config_.stats_interval_ms) {}

    void Run() {
        StartListeners();
        if (!config_.qualcomm_method) {
            stats_printer_.Start();
        }

        std::cout << "server listening on " << config_.bind << " ports ";
        for (uint32_t i = 0; i < config_.server_count; ++i) {
            if (i != 0) {
                std::cout << ",";
            }
            std::cout << (config_.base_port + i);
        }
        std::cout << std::endl;

        while (!g_stop_requested.load(std::memory_order_relaxed) &&
               !(config_.qualcomm_method &&
                 qualcomm_final_receives_.load(std::memory_order_relaxed) >= config_.message_count)) {
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
            struct QualcommAction {
                std::unique_ptr<SendBuffer> send;
                uint64_t sequence{0};
                uint8_t hop_in{0};
                uint8_t hop_out{0};
                uint64_t entry_realtime_ns{0};
                uint64_t entry_mono_ns{0};
            };
            std::vector<QualcommAction> qualcomm_actions;
            {
                std::lock_guard<std::mutex> lock(context->mutex);
                for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
                    const auto* buffer = &event->RECEIVE.Buffers[i];
                    context->receive_buffer.insert(
                        context->receive_buffer.end(),
                        buffer->Buffer,
                        buffer->Buffer + buffer->Length);
                }

                while (true) {
                    const auto frame_size = TryPeekFrameSize(context->receive_buffer, config_.message_size);
                    if (!frame_size.has_value()) {
                        break;
                    }
                    auto send = std::make_unique<SendBuffer>(*frame_size);
                    std::memcpy(send->storage.data(), context->receive_buffer.data(), *frame_size);
                    context->receive_buffer.erase(
                        context->receive_buffer.begin(),
                        context->receive_buffer.begin() + static_cast<std::ptrdiff_t>(*frame_size));
                    if (config_.qualcomm_method) {
                        const uint64_t entry_realtime_ns = RealtimeNs();
                        const uint64_t entry_mono_ns = NowNs();
                        MessageHeader header{};
                        std::memcpy(&header, send->storage.data(), sizeof(header));
                        const uint8_t hop_in = QualcommHop(send->storage);
                        if (hop_in == kQualcommHopInitial) {
                            SetQualcommHop(send->storage, kQualcommHopReflected);
                            ++context->pending_sends;
                            qualcomm_actions.push_back(QualcommAction{
                                std::move(send),
                                header.sequence,
                                hop_in,
                                kQualcommHopReflected,
                                entry_realtime_ns,
                                entry_mono_ns,
                            });
                        } else {
                            qualcomm_actions.push_back(QualcommAction{
                                std::move(send),
                                header.sequence,
                                hop_in,
                                hop_in,
                                entry_realtime_ns,
                                entry_mono_ns,
                            });
                        }
                        continue;
                    }
                    ++context->pending_sends;
                    sends.push_back(std::move(send));
                }

                if ((event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0) {
                    context->peer_finished = true;
                }
            }

            for (auto& action : qualcomm_actions) {
                const uint64_t exit_mono_ns = NowNs();
                const uint64_t exit_realtime_ns = RealtimeNs();
                trace_.Log(
                    action.hop_out == kQualcommHopReflected ? "amf_turnaround" : "amf_final_receive",
                    action.sequence,
                    action.send == nullptr ? 0 : action.send->quic_buffer.Length,
                    action.hop_in,
                    action.hop_out,
                    action.entry_realtime_ns,
                    exit_realtime_ns,
                    action.entry_mono_ns,
                    exit_mono_ns);
                if (action.send == nullptr) {
                    continue;
                }
                stats_.AddReceived(action.send->quic_buffer.Length);
                if (action.hop_out != kQualcommHopReflected) {
                    qualcomm_final_receives_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                MessageHeader header{};
                std::memcpy(&header, action.send->storage.data(), sizeof(header));
                header.send_timestamp_ns = exit_mono_ns;
                std::memcpy(action.send->storage.data(), &header, sizeof(header));
                auto* raw_send = action.send.release();
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
                stats_.AddSent(raw_send->quic_buffer.Length);
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
                stats_.AddReceived(raw_send->quic_buffer.Length);
                stats_.AddSent(raw_send->quic_buffer.Length);
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
    QualcommTraceWriter trace_;
    StatsPrinter stats_printer_;
    std::vector<std::unique_ptr<ListenerHandle>> listeners_;
    std::atomic<uint64_t> qualcomm_final_receives_{0};
};

class Client {
  public:
    explicit Client(const AppConfig& config)
        : config_(config),
          api_(),
          registration_(api_, "msquic-loadtest-client"),
          configuration_(api_, registration_.get(), config_, true),
          trace_(config, "ran"),
          stream_metrics_(config.stream_count),
          stats_printer_("client", stats_, config_.stats_interval_ms),
          pacing_mode_(DeterminePacingMode(config)),
          paced_(pacing_mode_ != PacingMode::Unlimited || HasStreamProfilePacing(config)),
          active_send_connection_count_(ActiveSendConnectionCount(config)),
          pacing_interval_ns_(ComputePacingIntervalNs(config, pacing_mode_, active_send_connection_count_)) {}

    ~Client() {
        StopPacer();
    }

    void Run() {
        StartConnections();
        if (config_.qualcomm_method) {
            while (!g_stop_requested.load(std::memory_order_relaxed) && !QualcommDone()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            for (auto& connection : connections_) {
                ForceShutdown(*connection);
            }
            WaitForConnectionsToClose();
            PrintSummary();
            return;
        }
        deadline_ = Clock::now() + std::chrono::seconds(config_.duration_sec);
        drain_deadline_ = deadline_ + std::chrono::milliseconds(config_.drain_timeout_ms);

        StartPacer();
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
    enum class PacingMode {
        Unlimited,
        TotalPps,
        PerClientPps,
    };

    static PacingMode DeterminePacingMode(const AppConfig& config) {
        if (config.send_pps_per_client > 0) {
            return PacingMode::PerClientPps;
        }
        if (config.send_pps > 0) {
            return PacingMode::TotalPps;
        }
        return PacingMode::Unlimited;
    }

    static double ComputePacingIntervalNs(
        const AppConfig& config,
        PacingMode pacing_mode,
        uint32_t active_send_connection_count) {
        switch (pacing_mode) {
        case PacingMode::Unlimited:
            return 0.0;
        case PacingMode::TotalPps:
            return config.send_pps == 0 || active_send_connection_count == 0
                       ? 0.0
                       : (1'000'000'000.0 * static_cast<double>(active_send_connection_count)) /
                             static_cast<double>(config.send_pps);
        case PacingMode::PerClientPps:
            return config.send_pps_per_client == 0 || active_send_connection_count == 0
                       ? 0.0
                       : 1'000'000'000.0 / static_cast<double>(config.send_pps_per_client);
        }
        return 0.0;
    }

    struct ConnectionContext;

    struct StreamContext {
        StreamContext(ConnectionContext& owner, uint32_t ordinal) : owner(owner), ordinal(ordinal) {}
        ConnectionContext& owner;
        const uint32_t ordinal;
        AppConfig::StreamProfile profile;
        HQUIC stream{nullptr};
        std::mutex mutex;
        std::vector<uint8_t> receive_buffer;
        uint64_t next_sequence{0};
        uint64_t echoed_messages{0};
        uint64_t final_messages_sent{0};
        double next_send_time_ns{0.0};
        bool stream_started{false};
        bool shutdown_started{false};
        bool closed{false};
    };

    struct ConnectionContext {
        ConnectionContext(Client& owner, uint32_t index, uint16_t port)
            : owner(owner), index(index), port(port) {}

        Client& owner;
        uint32_t index;
        uint16_t port;
        HQUIC connection{nullptr};
        std::mutex mutex;
        std::vector<std::unique_ptr<StreamContext>> stream_contexts;
        double next_send_time_ns{0.0};
        uint32_t next_send_stream_ordinal{0};
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
        auto* stream_context = static_cast<StreamContext*>(context);
        return stream_context->owner.owner.OnStreamEvent(
            stream_context->owner,
            stream,
            stream_context,
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

    bool AllStreamsDrained(const ConnectionContext& connection) const {
        for (const auto& stream_ctx : connection.stream_contexts) {
            if (!stream_ctx->closed && stream_ctx->next_sequence != stream_ctx->echoed_messages) {
                return false;
            }
        }
        return true;
    }

    bool AllStreamsClosed(const ConnectionContext& connection) const {
        for (const auto& stream_ctx : connection.stream_contexts) {
            if (!stream_ctx->closed) {
                return false;
            }
        }
        return true;
    }

    bool QualcommDone() {
        if (!config_.qualcomm_method) {
            return false;
        }
        for (const auto& connection : connections_) {
            for (const auto& stream_ctx : connection->stream_contexts) {
                std::lock_guard<std::mutex> stream_lock(stream_ctx->mutex);
                if (stream_ctx->final_messages_sent >= config_.message_count) {
                    return true;
                }
            }
        }
        return false;
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
            bool did_work = false;
            double earliest_next_send_ns = std::numeric_limits<double>::max();
            for (auto& connection : connections_) {
                std::lock_guard<std::mutex> lock(connection->mutex);
                if (!connection->stream_contexts.empty()) {
                    if (PumpSends(*connection, 1) > 0) {
                        did_work = true;
                    }
                    if (connection->next_send_time_ns > 0.0) {
                        earliest_next_send_ns = std::min(earliest_next_send_ns, connection->next_send_time_ns);
                    }
                }
            }
            if (did_work || earliest_next_send_ns == std::numeric_limits<double>::max()) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }

            const double now_ns = static_cast<double>(NowNs());
            if (earliest_next_send_ns <= now_ns) {
                continue;
            }
            const auto sleep_ns = static_cast<int64_t>(std::min(earliest_next_send_ns - now_ns, 1'000'000.0));
            std::this_thread::sleep_for(std::chrono::nanoseconds(std::max<int64_t>(sleep_ns, 50'000)));
        }
    }

    void StartStreams(ConnectionContext& connection) {
        std::lock_guard<std::mutex> lock(connection.mutex);
        if (!connection.stream_contexts.empty()) {
            return;
        }
        connection.stream_contexts.reserve(config_.stream_count);
        for (uint32_t ordinal = 0; ordinal < config_.stream_count; ++ordinal) {
            auto stream_ctx = std::make_unique<StreamContext>(connection, ordinal);
            stream_ctx->profile = StreamProfileForOrdinal(config_, ordinal);
            HQUIC stream = nullptr;
            const auto open_status =
                api_->StreamOpen(connection.connection, QUIC_STREAM_OPEN_FLAG_NONE, StreamCallback, stream_ctx.get(), &stream);
            if (QUIC_FAILED(open_status)) {
                std::cerr << "StreamOpen failed: " << StatusToHex(open_status) << std::endl;
                api_->ConnectionShutdown(connection.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 1);
                return;
            }
            stream_ctx->stream = stream;
            const auto start_status = api_->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
            if (QUIC_FAILED(start_status)) {
                api_->StreamClose(stream);
                std::cerr << "StreamStart failed: " << StatusToHex(start_status) << std::endl;
                api_->ConnectionShutdown(connection.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 1);
                return;
            }
            connection.stream_contexts.push_back(std::move(stream_ctx));
        }
    }

    size_t PumpSends(ConnectionContext& connection, size_t max_messages = std::numeric_limits<size_t>::max()) {
        size_t send_count = 0;
        while (ShouldSendOnConnection(connection) &&
               send_count < max_messages &&
               !stop_sending_.load(std::memory_order_relaxed)) {
            if (paced_) {
                const double now_ns = static_cast<double>(NowNs());
                if (connection.next_send_time_ns == 0.0) {
                    connection.next_send_time_ns = now_ns;
                }
                if (connection.next_send_time_ns > now_ns) {
                    break;
                }
                if (pacing_interval_ns_ > 0.0) {
                    connection.next_send_time_ns += pacing_interval_ns_;
                    if (connection.next_send_time_ns + (pacing_interval_ns_ * 4.0) < now_ns) {
                        connection.next_send_time_ns = now_ns;
                    }
                }
            }

            StreamContext* selected_stream = nullptr;
            for (uint32_t attempt = 0; attempt < connection.stream_contexts.size(); ++attempt) {
                const uint32_t ordinal = (connection.next_send_stream_ordinal + attempt) % connection.stream_contexts.size();
                auto& candidate = connection.stream_contexts[ordinal];
                std::lock_guard<std::mutex> stream_lock(candidate->mutex);
                if (!candidate->stream_started || candidate->shutdown_started || candidate->closed) {
                    continue;
                }
                if ((candidate->next_sequence - candidate->echoed_messages) >= candidate->profile.max_inflight) {
                    continue;
                }
                if (config_.qualcomm_method && candidate->next_sequence >= config_.message_count) {
                    continue;
                }
                if (candidate->profile.send_pps > 0) {
                    const double now_ns = static_cast<double>(NowNs());
                    if (candidate->next_send_time_ns == 0.0) {
                        candidate->next_send_time_ns = now_ns;
                    }
                    if (candidate->next_send_time_ns > now_ns) {
                        continue;
                    }
                    const double interval_ns = 1'000'000'000.0 / static_cast<double>(candidate->profile.send_pps);
                    candidate->next_send_time_ns += interval_ns;
                    if (candidate->next_send_time_ns + (interval_ns * 4.0) < now_ns) {
                        candidate->next_send_time_ns = now_ns;
                    }
                }
                if (candidate->profile.name.empty()) {
                    continue;
                }
                selected_stream = candidate.get();
                connection.next_send_stream_ordinal = (ordinal + 1) % connection.stream_contexts.size();
                break;
            }
            if (selected_stream == nullptr) {
                break;
            }

            auto send = std::make_unique<SendBuffer>(selected_stream->profile.message_size);

            MessageHeader header{};
            header.magic = kMessageMagic;
            header.reserved = static_cast<uint32_t>(send->storage.size());
            {
                std::lock_guard<std::mutex> stream_lock(selected_stream->mutex);
                header.sequence = selected_stream->next_sequence++;
            }
            header.send_timestamp_ns = 0;
            std::memcpy(send->storage.data(), &header, sizeof(header));

            for (uint32_t i = sizeof(header); i < send->storage.size(); ++i) {
                send->storage[i] = static_cast<uint8_t>(header.sequence + i);
            }
            if (config_.qualcomm_method) {
                SetQualcommHop(send->storage, kQualcommHopInitial);
            }

            auto* raw_send = send.release();
            header.send_timestamp_ns = NowNs();
            std::memcpy(raw_send->storage.data(), &header, sizeof(header));
            const auto status = api_->StreamSend(selected_stream->stream, &raw_send->quic_buffer, 1, QUIC_SEND_FLAG_NONE, raw_send);
            if (QUIC_FAILED(status)) {
                delete raw_send;
                std::cerr << "StreamSend failed: " << StatusToHex(status) << std::endl;
                api_->ConnectionShutdown(connection.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 1);
                return send_count;
            }
            stats_.AddSent(raw_send->quic_buffer.Length);
            stream_metrics_[selected_stream->ordinal].AddSent(raw_send->quic_buffer.Length);
            ++send_count;
        }

        if (stop_sending_.load(std::memory_order_relaxed) && AllStreamsDrained(connection)) {
            for (auto& stream_ctx : connection.stream_contexts) {
                std::lock_guard<std::mutex> stream_lock(stream_ctx->mutex);
                if (!stream_ctx->shutdown_started && !stream_ctx->closed && stream_ctx->stream_started) {
                    stream_ctx->shutdown_started = true;
                    api_->StreamShutdown(stream_ctx->stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
                }
            }
        }
        return send_count;
    }

    void RequestStopSending(ConnectionContext& connection) {
        std::lock_guard<std::mutex> lock(connection.mutex);
        if (!connection.stream_contexts.empty()) {
            PumpSends(connection, 0);
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
            StartStreams(connection);
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
                std::lock_guard<std::mutex> connection_lock(connection.mutex);
                PumpSends(connection);
            }
            break;
        }
        case QUIC_STREAM_EVENT_RECEIVE: {
            struct FinalSend {
                std::unique_ptr<SendBuffer> send;
                uint64_t sequence{0};
                uint8_t hop_in{0};
                uint8_t hop_out{0};
                uint64_t entry_realtime_ns{0};
                uint64_t entry_mono_ns{0};
            };
            std::vector<FinalSend> final_sends;
            {
                std::lock_guard<std::mutex> lock(stream_ctx->mutex);
                for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
                    const auto* buffer = &event->RECEIVE.Buffers[i];
                    stream_ctx->receive_buffer.insert(
                        stream_ctx->receive_buffer.end(),
                        buffer->Buffer,
                        buffer->Buffer + buffer->Length);
                }

                while (true) {
                    const auto frame_size = TryPeekFrameSize(stream_ctx->receive_buffer, stream_ctx->profile.message_size);
                    if (!frame_size.has_value()) {
                        break;
                    }
                    MessageHeader header{};
                    std::memcpy(&header, stream_ctx->receive_buffer.data(), sizeof(header));
                    const uint32_t actual_frame_size = FrameSizeFromHeader(header, stream_ctx->profile.message_size);
                    if (config_.qualcomm_method) {
                        const uint64_t entry_realtime_ns = RealtimeNs();
                        const uint64_t entry_mono_ns = NowNs();
                        auto send = std::make_unique<SendBuffer>(actual_frame_size);
                        std::memcpy(send->storage.data(), stream_ctx->receive_buffer.data(), actual_frame_size);
                        const uint8_t hop_in = QualcommHop(send->storage);
                        stream_ctx->receive_buffer.erase(
                            stream_ctx->receive_buffer.begin(),
                            stream_ctx->receive_buffer.begin() + static_cast<std::ptrdiff_t>(actual_frame_size));
                        if (hop_in == kQualcommHopReflected) {
                            SetQualcommHop(send->storage, kQualcommHopFinal);
                            ++stream_ctx->echoed_messages;
                            ++stream_ctx->final_messages_sent;
                            stats_.AddReceived(actual_frame_size);
                            stream_metrics_[stream_ctx->ordinal].AddReceived(actual_frame_size, 0);
                            final_sends.push_back(FinalSend{
                                std::move(send),
                                header.sequence,
                                hop_in,
                                kQualcommHopFinal,
                                entry_realtime_ns,
                                entry_mono_ns,
                            });
                        }
                        if (stream_ctx->echoed_messages >= config_.message_count) {
                            stop_sending_.store(true, std::memory_order_relaxed);
                        }
                        continue;
                    }
                    const auto latency_ns = NowNs() - header.send_timestamp_ns;
                    stats_.AddReceived(actual_frame_size);
                    stats_.AddLatencyNs(latency_ns);
                    stream_metrics_[stream_ctx->ordinal].AddReceived(actual_frame_size, latency_ns);
                    ++stream_ctx->echoed_messages;

                    stream_ctx->receive_buffer.erase(
                        stream_ctx->receive_buffer.begin(),
                        stream_ctx->receive_buffer.begin() + static_cast<std::ptrdiff_t>(actual_frame_size));
                }
            }

            for (auto& final_send : final_sends) {
                const uint64_t exit_mono_ns = NowNs();
                const uint64_t exit_realtime_ns = RealtimeNs();
                trace_.Log(
                    "ran_turnaround",
                    final_send.sequence,
                    final_send.send == nullptr ? 0 : final_send.send->quic_buffer.Length,
                    final_send.hop_in,
                    final_send.hop_out,
                    final_send.entry_realtime_ns,
                    exit_realtime_ns,
                    final_send.entry_mono_ns,
                    exit_mono_ns);
                if (final_send.send == nullptr) {
                    continue;
                }
                MessageHeader header{};
                std::memcpy(&header, final_send.send->storage.data(), sizeof(header));
                header.send_timestamp_ns = exit_mono_ns;
                std::memcpy(final_send.send->storage.data(), &header, sizeof(header));
                auto* raw_send = final_send.send.release();
                const auto status = api_->StreamSend(stream, &raw_send->quic_buffer, 1, QUIC_SEND_FLAG_NONE, raw_send);
                if (QUIC_FAILED(status)) {
                    delete raw_send;
                    std::cerr << "StreamSend(final) failed: " << StatusToHex(status) << std::endl;
                    api_->ConnectionShutdown(connection.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 1);
                    break;
                }
                stats_.AddSent(raw_send->quic_buffer.Length);
                stream_metrics_[stream_ctx->ordinal].AddSent(raw_send->quic_buffer.Length);
            }

            if (!paced_) {
                std::lock_guard<std::mutex> connection_lock(connection.mutex);
                PumpSends(connection);
            }
            break;
        }
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            delete static_cast<SendBuffer*>(event->SEND_COMPLETE.ClientContext);
            break;
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            {
                std::lock_guard<std::mutex> lock(stream_ctx->mutex);
                stream_ctx->closed = true;
            }
            api_->StreamClose(stream);
            {
                std::lock_guard<std::mutex> connection_lock(connection.mutex);
                if (AllStreamsClosed(connection)) {
                    api_->ConnectionShutdown(connection.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
                }
            }
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
        for (uint32_t ordinal = 0; ordinal < config_.stream_count; ++ordinal) {
            const auto profile = StreamProfileForOrdinal(config_, ordinal);
            const auto stream_snapshot = stream_metrics_[ordinal].Snapshot();
            std::cout << "client stream summary:"
                      << " name=" << profile.name
                      << " stream_id=" << ordinal
                      << " sent_messages=" << stream_snapshot.sent_messages
                      << " echoed_messages=" << stream_snapshot.echoed_messages
                      << " sent_bytes=" << stream_snapshot.sent_bytes
                      << " echoed_bytes=" << stream_snapshot.echoed_bytes
                      << " latency_ms(p50/p75/p99)=" << FormatLatencySummary(stream_snapshot.latency)
                      << std::endl;
            std::cout << "client stream latency detail:"
                      << " name=" << profile.name
                      << " stream_id=" << ordinal
                      << " " << FormatLatencyDetailSummary(stream_snapshot.latency_detail)
                      << std::endl;
        }
    }

    AppConfig config_;
    MsQuicApi api_;
    Registration registration_;
    Configuration configuration_;
    Stats stats_;
    QualcommTraceWriter trace_;
    std::vector<StreamMetrics> stream_metrics_;
    StatsPrinter stats_printer_;
    std::vector<std::unique_ptr<ConnectionContext>> connections_;
    std::atomic<uint32_t> active_connections_{0};
    std::mutex done_mutex_;
    std::condition_variable done_cv_;
    std::atomic<bool> stop_sending_{false};
    const PacingMode pacing_mode_{PacingMode::Unlimited};
    const bool paced_{false};
    const uint32_t active_send_connection_count_{0};
    const double pacing_interval_ns_{0.0};
    std::atomic<bool> pacer_stop_{false};
    std::thread pacer_thread_;
    Clock::time_point deadline_{};
    Clock::time_point drain_deadline_{};
};
#endif

#ifdef HAVE_MSQUIC
void RunMsQuicServer(const AppConfig& config) {
    Server server(config);
    server.Run();
}

void RunMsQuicClient(const AppConfig& config) {
    Client client(config);
    client.Run();
}
#else
void RunMsQuicServer(const AppConfig&) {
    throw std::runtime_error("MSQuic backend is not built; install MSQuic or use --protocol=sctp");
}

void RunMsQuicClient(const AppConfig&) {
    throw std::runtime_error("MSQuic backend is not built; install MSQuic or use --protocol=sctp");
}
#endif

} // namespace proto_test
