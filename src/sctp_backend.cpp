#include "sctp_backend.h"

namespace proto_test {

class SctpConnection : public ITransportConnection, public std::enable_shared_from_this<SctpConnection> {
  public:
    struct PendingSend {
        uint16_t stream_id;
        std::vector<uint8_t> data;
    };

    SctpConnection(int fd, uint32_t id, uint16_t default_stream_id, ITransportEventHandler& handler, SSL* ssl)
        : fd_(fd), id_(id), default_stream_id_(default_stream_id), handler_(handler), ssl_(ssl) {
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
        if (ssl_ == nullptr) {
            send_thread_ = std::thread([self = shared_from_this()]() { self->SendLoop(); });
        }
        recv_thread_ = std::thread([self = shared_from_this()]() { self->ReceiveLoop(); });
    }

    int Fd() const {
        return fd_.load(std::memory_order_relaxed);
    }

    bool UsesExternalPolling() const {
        return ssl_ != nullptr;
    }

    bool HandlePollEvents(short revents) {
        if (closed_.load(std::memory_order_relaxed)) {
            return false;
        }
        if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            Close();
            handler_.OnClosed(id_);
            return false;
        }

        while (!closed_.load(std::memory_order_relaxed) && !g_stop_requested.load(std::memory_order_relaxed)) {
            size_t read = 0;
            int ok = 0;
            int ssl_error = 0;
            {
                std::lock_guard<std::mutex> lock(ssl_mutex_);
                ok = SslReadCompat(ssl_, poll_buffer_.data(), poll_buffer_.size(), &read);
                if (ok != 1) {
                    ssl_error = SSL_get_error(ssl_, ok);
                }
            }

            if (ok == 1) {
                try {
                    handler_.OnData(shared_from_this(), default_stream_id_, poll_buffer_.data(), read);
                } catch (const std::exception& ex) {
                    handler_.OnTransportError(id_, ex.what());
                    Close();
                    handler_.OnClosed(id_);
                    return false;
                }
                continue;
            }

            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                return true;
            }
            if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                try {
                    handler_.OnPeerClosed(shared_from_this());
                } catch (const std::exception& ex) {
                    handler_.OnTransportError(id_, ex.what());
                }
                Close();
                handler_.OnClosed(id_);
                return false;
            }
            if (ssl_error == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;
            }

            handler_.OnTransportError(id_, OpenSslErrorString("SSL_read_ex"));
            Close();
            handler_.OnClosed(id_);
            return false;
        }

        return !closed_.load(std::memory_order_relaxed);
    }

    void Join() {
        if (recv_thread_.joinable()) {
            recv_thread_.join();
        }
        if (send_thread_.joinable()) {
            send_thread_.join();
        }
    }

    void SendCopy(const uint8_t* data, size_t length, uint16_t stream_id) override {
        if (closed_.load(std::memory_order_relaxed)) {
            return;
        }

        if (ssl_ != nullptr) {
            size_t offset = 0;
            while (offset < length) {
                size_t written = 0;
                int ok = 0;
                int ssl_error = 0;
                {
                    std::lock_guard<std::mutex> lock(ssl_mutex_);
                    ok = SslWriteCompat(ssl_, data + offset, length - offset, &written);
                    if (ok != 1) {
                        ssl_error = SSL_get_error(ssl_, ok);
                    }
                }
                if (ok == 1) {
                    offset += written;
                    continue;
                }
                if (closed_.load(std::memory_order_relaxed)) {
                    return;
                }
                if (ssl_error == SSL_ERROR_WANT_READ) {
                    WaitForSocketReady(POLLIN);
                    continue;
                }
                if (ssl_error == SSL_ERROR_WANT_WRITE) {
                    WaitForSocketReady(POLLOUT);
                    continue;
                }
                throw std::runtime_error(OpenSslErrorString("SSL_write_ex"));
            }
            return;
        }

        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            pending_sends_.push_back(PendingSend{stream_id, std::vector<uint8_t>(data, data + length)});
        }
        send_cv_.notify_one();
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
            std::lock_guard<std::mutex> lock(ssl_mutex_);
            SSL_shutdown(ssl_);
        }
        if (ssl_ == nullptr) {
            send_close_requested_.store(true, std::memory_order_relaxed);
            send_cv_.notify_one();
            return;
        }
        if (::shutdown(fd_, SHUT_WR) != 0 && errno != ENOTCONN && errno != EBADF) {
            throw std::runtime_error(SocketErrorString("shutdown(SHUT_WR)"));
        }
    }

    void Close() override {
        if (closed_.exchange(true, std::memory_order_relaxed)) {
            return;
        }
        send_cv_.notify_all();
        ::shutdown(fd_, SHUT_RDWR);
        CloseFd();
    }

  private:
    void SendLoop() {
        while (!closed_.load(std::memory_order_relaxed) && !g_stop_requested.load(std::memory_order_relaxed)) {
            PendingSend pending{};
            bool have_pending = false;
            bool should_close_send = false;

            {
                std::unique_lock<std::mutex> lock(send_mutex_);
                send_cv_.wait(lock, [&]() {
                    return closed_.load(std::memory_order_relaxed) ||
                           !pending_sends_.empty() ||
                           send_close_requested_.load(std::memory_order_relaxed);
                });
                if (closed_.load(std::memory_order_relaxed)) {
                    return;
                }
                if (!pending_sends_.empty()) {
                    pending = std::move(pending_sends_.front());
                    pending_sends_.pop_front();
                    have_pending = true;
                } else if (send_close_requested_.load(std::memory_order_relaxed)) {
                    should_close_send = true;
                }
            }

            if (have_pending) {
                try {
                    SendBlocking(pending.data.data(), pending.data.size(), pending.stream_id);
                } catch (const std::exception& ex) {
                    handler_.OnTransportError(id_, ex.what());
                    Close();
                    handler_.OnClosed(id_);
                    return;
                }
                continue;
            }

            if (should_close_send) {
                if (::shutdown(fd_, SHUT_WR) != 0 && errno != ENOTCONN && errno != EBADF) {
                    handler_.OnTransportError(id_, SocketErrorString("shutdown(SHUT_WR)"));
                    Close();
                    handler_.OnClosed(id_);
                }
                return;
            }
        }
    }

    void SendBlocking(const uint8_t* data, size_t length, uint16_t stream_id) {
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
            sndinfo->snd_sid = stream_id;
            msg.msg_controllen = cmsg->cmsg_len;

            const ssize_t written = sendmsg(fd_, &msg, 0);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(SocketErrorString("sendmsg"));
            }
            offset += static_cast<size_t>(written);
        }
    }

    void WaitForSocketReady(short events) {
        const int fd = fd_.load(std::memory_order_relaxed);
        if (fd < 0 || closed_.load(std::memory_order_relaxed)) {
            return;
        }

        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = events;
        while (!closed_.load(std::memory_order_relaxed) && !g_stop_requested.load(std::memory_order_relaxed)) {
            const int rc = ::poll(&pfd, 1, -1);
            if (rc > 0) {
                return;
            }
            if (rc == 0) {
                continue;
            }
            if (errno != EINTR) {
                return;
            }
        }
    }

    void ReceiveLoop() {
        std::vector<uint8_t> buffer(64 * 1024);

        while (!closed_.load(std::memory_order_relaxed) && !g_stop_requested.load(std::memory_order_relaxed)) {
            ssize_t bytes = -1;
            if (ssl_ != nullptr) {
                size_t read = 0;
                int ok = 0;
                int ssl_error = 0;
                {
                    std::lock_guard<std::mutex> lock(ssl_mutex_);
                    ok = SslReadCompat(ssl_, buffer.data(), buffer.size(), &read);
                    if (ok != 1) {
                        ssl_error = SSL_get_error(ssl_, ok);
                    }
                }
                if (ok == 1) {
                    bytes = static_cast<ssize_t>(read);
                } else {
                    if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                        bytes = 0;
                    } else if (!closed_.load(std::memory_order_relaxed) && ssl_error == SSL_ERROR_WANT_READ) {
                        WaitForSocketReady(POLLIN);
                        continue;
                    } else if (!closed_.load(std::memory_order_relaxed) && ssl_error == SSL_ERROR_WANT_WRITE) {
                        WaitForSocketReady(POLLOUT);
                        continue;
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
                struct msghdr msg {};
                struct iovec iov {};
                iov.iov_base = buffer.data();
                iov.iov_len = buffer.size();
                char control[CMSG_SPACE(sizeof(sctp_rcvinfo))] {};
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;
                msg.msg_control = control;
                msg.msg_controllen = sizeof(control);
                bytes = recvmsg(fd_, &msg, 0);
                if (bytes > 0) {
                    uint16_t stream_id = default_stream_id_;
                    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                        if (cmsg->cmsg_level == IPPROTO_SCTP && cmsg->cmsg_type == SCTP_RCVINFO) {
                            const auto* rcvinfo = reinterpret_cast<const sctp_rcvinfo*>(CMSG_DATA(cmsg));
                            stream_id = rcvinfo->rcv_sid;
                            break;
                        }
                    }
                    try {
                        handler_.OnData(shared_from_this(), stream_id, buffer.data(), static_cast<size_t>(bytes));
                    } catch (const std::exception& ex) {
                        handler_.OnTransportError(id_, ex.what());
                        break;
                    }
                    continue;
                }
            }
            if (bytes > 0) {
                try {
                    handler_.OnData(shared_from_this(), default_stream_id_, buffer.data(), static_cast<size_t>(bytes));
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
    const uint16_t default_stream_id_;
    ITransportEventHandler& handler_;
    std::atomic<bool> closed_{false};
    std::atomic<bool> send_closed_{false};
    std::thread recv_thread_;
    std::thread send_thread_;
    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::deque<PendingSend> pending_sends_;
    std::atomic<bool> send_close_requested_{false};
    std::mutex ssl_mutex_;
    std::vector<uint8_t> poll_buffer_{std::vector<uint8_t>(64 * 1024)};
    SSL* ssl_{nullptr};
};

struct LoadServerConnectionState {
    std::shared_ptr<ITransportConnection> connection;
    std::map<uint16_t, std::vector<uint8_t>> receive_buffers;
    bool peer_closed{false};
};

class LoadServerController : public ITransportEventHandler {
  public:
    LoadServerController(const AppConfig& config, Stats& stats)
        : config_(config), stats_(stats), trace_(config, "amf") {}

    void OnConnected(const std::shared_ptr<ITransportConnection>& connection) override {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_[connection->Id()].connection = connection;
        std::cerr << ProtocolName(config_.protocol) << " server connection " << connection->Id() << " established" << std::endl;
    }

    void OnData(
        const std::shared_ptr<ITransportConnection>& connection,
        uint16_t stream_id,
        const uint8_t* data,
        size_t length) override {
        struct QualcommAction {
            std::vector<uint8_t> frame;
            uint16_t stream_id{0};
            uint64_t sequence{0};
            uint8_t hop_in{0};
            uint8_t hop_out{0};
            uint64_t entry_realtime_ns{0};
            uint64_t entry_mono_ns{0};
            bool send{false};
        };
        std::vector<std::vector<uint8_t>> sends;
        std::vector<QualcommAction> qualcomm_actions;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& state = connections_[connection->Id()];
            state.connection = connection;
            auto& receive_buffer = state.receive_buffers[stream_id];
            receive_buffer.insert(receive_buffer.end(), data, data + length);
            while (true) {
                const auto frame_size = TryPeekFrameSize(receive_buffer, config_.message_size);
                if (!frame_size.has_value()) {
                    break;
                }
                std::vector<uint8_t> frame(*frame_size);
                std::memcpy(frame.data(), receive_buffer.data(), *frame_size);
                receive_buffer.erase(
                    receive_buffer.begin(),
                    receive_buffer.begin() + static_cast<std::ptrdiff_t>(*frame_size));
                if (config_.qualcomm_method) {
                    const uint64_t entry_realtime_ns = RealtimeNs();
                    const uint64_t entry_mono_ns = NowNs();
                    MessageHeader header{};
                    std::memcpy(&header, frame.data(), sizeof(header));
                    const uint8_t hop_in = QualcommHop(frame);
                    if (hop_in == kQualcommHopInitial) {
                        SetQualcommHop(frame, kQualcommHopReflected);
                        qualcomm_actions.push_back(QualcommAction{
                            std::move(frame),
                            stream_id,
                            header.sequence,
                            hop_in,
                            kQualcommHopReflected,
                            entry_realtime_ns,
                            entry_mono_ns,
                            true,
                        });
                    } else {
                        ++qualcomm_final_receives_;
                        qualcomm_actions.push_back(QualcommAction{
                            std::move(frame),
                            stream_id,
                            header.sequence,
                            hop_in,
                            hop_in,
                            entry_realtime_ns,
                            entry_mono_ns,
                            false,
                        });
                        done_cv_.notify_all();
                    }
                    continue;
                }
                sends.push_back(std::move(frame));
            }
        }

        for (auto& action : qualcomm_actions) {
            const uint64_t exit_mono_ns = NowNs();
            const uint64_t exit_realtime_ns = RealtimeNs();
            trace_.Log(
                action.send ? "amf_turnaround" : "amf_final_receive",
                action.sequence,
                static_cast<uint32_t>(action.frame.size()),
                action.hop_in,
                action.hop_out,
                action.entry_realtime_ns,
                exit_realtime_ns,
                action.entry_mono_ns,
                exit_mono_ns);
            stats_.AddReceived(action.frame.size());
            if (action.send) {
                MessageHeader header{};
                std::memcpy(&header, action.frame.data(), sizeof(header));
                header.send_timestamp_ns = exit_mono_ns;
                std::memcpy(action.frame.data(), &header, sizeof(header));
                connection->SendCopy(action.frame.data(), action.frame.size(), action.stream_id);
                stats_.AddSent(action.frame.size());
            }
        }

        for (const auto& frame : sends) {
            connection->SendCopy(frame.data(), frame.size(), stream_id);
            stats_.AddReceived(frame.size());
            stats_.AddSent(frame.size());
        }
    }

    bool QualcommDone() {
        if (!config_.qualcomm_method) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return qualcomm_final_receives_ >= config_.message_count;
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
    QualcommTraceWriter trace_;
    std::mutex mutex_;
    std::condition_variable done_cv_;
    std::map<uint32_t, LoadServerConnectionState> connections_;
    uint64_t qualcomm_final_receives_{0};
};

struct LoadClientStreamState {
    AppConfig::StreamProfile profile;
    std::vector<uint8_t> receive_buffer;
    uint64_t next_sequence{0};
    uint64_t echoed_messages{0};
    uint64_t final_messages_sent{0};
    double next_send_time_ns{0.0};
};

struct LoadClientConnectionState {
    std::shared_ptr<ITransportConnection> connection;
    std::map<uint16_t, LoadClientStreamState> streams;
    double next_send_time_ns{0.0};
    uint32_t next_send_stream_ordinal{0};
    bool connected{false};
    bool send_closed{false};
};

class LoadClientController : public ITransportEventHandler {
  public:
    LoadClientController(const AppConfig& config, Stats& stats)
        : config_(config),
          stats_(stats),
          trace_(config, "ran"),
          stream_metrics_(config.stream_count),
          pacing_mode_(DeterminePacingMode(config)),
          paced_(pacing_mode_ != PacingMode::Unlimited || HasStreamProfilePacing(config) ||
                 (config.qualcomm_method && config.qualcomm_gap_ms > 0)),
          active_send_connection_count_(ActiveSendConnectionCount(config)),
          pacing_interval_ns_(ComputePacingIntervalNs(config, pacing_mode_, active_send_connection_count_)) {}

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
            if (pacing_mode_ == PacingMode::PerClientPps && ShouldSendOnConnection(connection->Id())) {
                state.next_send_time_ns =
                    InitialPerClientSendTimeNs(connection->Id(), static_cast<double>(NowNs()));
            }
            for (uint32_t ordinal = 0; ordinal < config_.stream_count; ++ordinal) {
                const uint16_t stream_id = StreamIdFromOrdinal(ordinal);
                auto& stream_state = state.streams[stream_id];
                stream_state.profile = StreamProfileForOrdinal(config_, ordinal);
            }
        }
        std::cerr << ProtocolName(config_.protocol) << " client connection " << connection->Id() << " established" << std::endl;
        if (!paced_) {
            PumpSends(connection->Id());
        }
    }

    void OnData(
        const std::shared_ptr<ITransportConnection>& connection,
        uint16_t stream_id,
        const uint8_t* data,
        size_t length) override {
        struct FinalSend {
            std::vector<uint8_t> frame;
            uint16_t stream_id{0};
            uint64_t sequence{0};
            uint8_t hop_in{0};
            uint8_t hop_out{0};
            uint64_t entry_realtime_ns{0};
            uint64_t entry_mono_ns{0};
        };
        std::vector<FinalSend> final_sends;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& state = states_[connection->Id()];
            state.connection = connection;
            auto& stream_state = state.streams[stream_id];
            if (stream_state.profile.name.empty()) {
                stream_state.profile = StreamProfileForOrdinal(config_, stream_id - config_.sctp_stream_id);
            }
            stream_state.receive_buffer.insert(stream_state.receive_buffer.end(), data, data + length);

            while (true) {
                const auto frame_size = TryPeekFrameSize(stream_state.receive_buffer, stream_state.profile.message_size);
                if (!frame_size.has_value()) {
                    break;
                }
                MessageHeader header{};
                std::memcpy(&header, stream_state.receive_buffer.data(), sizeof(header));
                const uint32_t actual_frame_size = FrameSizeFromHeader(header, stream_state.profile.message_size);
                if (config_.qualcomm_method) {
                    const uint64_t entry_realtime_ns = RealtimeNs();
                    const uint64_t entry_mono_ns = NowNs();
                    std::vector<uint8_t> frame(actual_frame_size);
                    std::memcpy(frame.data(), stream_state.receive_buffer.data(), actual_frame_size);
                    const uint8_t hop_in = QualcommHop(frame);
                    stream_state.receive_buffer.erase(
                        stream_state.receive_buffer.begin(),
                        stream_state.receive_buffer.begin() + static_cast<std::ptrdiff_t>(actual_frame_size));
                    if (hop_in == kQualcommHopReflected) {
                        SetQualcommHop(frame, kQualcommHopFinal);
                        ++stream_state.echoed_messages;
                        ++stream_state.final_messages_sent;
                        stats_.AddReceived(actual_frame_size);
                        stream_metrics_[stream_id - config_.sctp_stream_id].AddReceived(actual_frame_size, 0);
                        final_sends.push_back(FinalSend{
                            std::move(frame),
                            stream_id,
                            header.sequence,
                            hop_in,
                            kQualcommHopFinal,
                            entry_realtime_ns,
                            entry_mono_ns,
                        });
                    }
                    if (stream_state.echoed_messages >= config_.message_count) {
                        stop_sending_.store(true, std::memory_order_relaxed);
                    }
                    continue;
                }
                stream_state.receive_buffer.erase(
                    stream_state.receive_buffer.begin(),
                    stream_state.receive_buffer.begin() + static_cast<std::ptrdiff_t>(actual_frame_size));
                ++stream_state.echoed_messages;
                const auto latency_ns = NowNs() - header.send_timestamp_ns;
                stats_.AddReceived(actual_frame_size);
                stats_.AddLatencyNs(latency_ns);
                stream_metrics_[stream_id - config_.sctp_stream_id].AddReceived(actual_frame_size, latency_ns);
            }
        }
        for (auto& final_send : final_sends) {
            const uint64_t exit_mono_ns = NowNs();
            const uint64_t exit_realtime_ns = RealtimeNs();
            trace_.Log(
                "ran_turnaround",
                final_send.sequence,
                static_cast<uint32_t>(final_send.frame.size()),
                final_send.hop_in,
                final_send.hop_out,
                final_send.entry_realtime_ns,
                exit_realtime_ns,
                final_send.entry_mono_ns,
                exit_mono_ns);
            MessageHeader header{};
            std::memcpy(&header, final_send.frame.data(), sizeof(header));
            header.send_timestamp_ns = exit_mono_ns;
            std::memcpy(final_send.frame.data(), &header, sizeof(header));
            connection->SendCopy(final_send.frame.data(), final_send.frame.size(), final_send.stream_id);
            stats_.AddSent(final_send.frame.size());
            stream_metrics_[final_send.stream_id - config_.sctp_stream_id].AddSent(final_send.frame.size());
            if (config_.qualcomm_method && config_.qualcomm_gap_ms > 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                auto& stream_state = states_[connection->Id()].streams[final_send.stream_id];
                stream_state.next_send_time_ns =
                    static_cast<double>(NowNs() + (config_.qualcomm_gap_ms * 1'000'000ULL));
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
                PumpSends(id, 0);
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

    bool QualcommDone() {
        if (!config_.qualcomm_method) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [_, state] : states_) {
            for (const auto& [__, stream_state] : state.streams) {
                if (stream_state.final_messages_sent >= config_.message_count) {
                    return true;
                }
            }
        }
        return false;
    }

    void PrintStreamSummaries(const char* prefix) const {
        for (uint32_t ordinal = 0; ordinal < config_.stream_count; ++ordinal) {
            const auto profile = StreamProfileForOrdinal(config_, ordinal);
            const auto snapshot = stream_metrics_[ordinal].Snapshot();
            std::cout << prefix
                      << " name=" << profile.name
                      << " stream_id=" << StreamIdFromOrdinal(ordinal)
                      << " sent_messages=" << snapshot.sent_messages
                      << " echoed_messages=" << snapshot.echoed_messages
                      << " sent_bytes=" << snapshot.sent_bytes
                      << " echoed_bytes=" << snapshot.echoed_bytes
                      << " latency_ms(p50/p75/p99)=" << FormatLatencySummary(snapshot.latency)
                      << std::endl;
            std::cout << "client stream latency detail:"
                      << " name=" << profile.name
                      << " stream_id=" << StreamIdFromOrdinal(ordinal)
                      << " " << FormatLatencyDetailSummary(snapshot.latency_detail)
                      << std::endl;
        }
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
            return config.send_pps == 0
                       ? 0.0
                       : 1'000'000'000.0 / static_cast<double>(config.send_pps);
        case PacingMode::PerClientPps:
            return config.send_pps_per_client == 0 || active_send_connection_count == 0
                       ? 0.0
                       : 1'000'000'000.0 / static_cast<double>(config.send_pps_per_client);
        }
        return 0.0;
    }

    double InitialPerClientSendTimeNs(uint32_t connection_id, double now_ns) const {
        if (pacing_mode_ != PacingMode::PerClientPps || active_send_connection_count_ <= 1 || pacing_interval_ns_ <= 0.0) {
            return now_ns;
        }

        uint32_t ordinal = 0;
        for (uint32_t id = 0; id < connection_id; ++id) {
            if (ShouldSendOnConnection(id)) {
                ++ordinal;
            }
        }

        const double phase_step_ns = pacing_interval_ns_ / static_cast<double>(active_send_connection_count_);
        return now_ns + (phase_step_ns * static_cast<double>(ordinal % active_send_connection_count_));
    }

    bool ShouldSendOnConnection(uint32_t connection_id) const {
        return config_.send_server_index < 0 ||
               (connection_id % config_.server_count) == static_cast<uint32_t>(config_.send_server_index);
    }

    uint16_t StreamIdFromOrdinal(uint32_t ordinal) const {
        return static_cast<uint16_t>(config_.sctp_stream_id + ordinal);
    }

    bool AllStreamsDrained(const LoadClientConnectionState& state) const {
        for (uint32_t ordinal = 0; ordinal < config_.stream_count; ++ordinal) {
            const uint16_t stream_id = StreamIdFromOrdinal(ordinal);
            const auto it = state.streams.find(stream_id);
            if (it != state.streams.end() && it->second.next_sequence != it->second.echoed_messages) {
                return false;
            }
        }
        return true;
    }

    void PacerLoop() {
        if (pacing_mode_ == PacingMode::TotalPps && global_next_send_time_ns_ == 0.0) {
            global_next_send_time_ns_ = static_cast<double>(NowNs());
        }

        while (!pacer_stop_.load(std::memory_order_relaxed) && !g_stop_requested.load(std::memory_order_relaxed)) {
            std::vector<uint32_t> ids;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ids.reserve(states_.size());
                for (const auto& [id, _] : states_) {
                    ids.push_back(id);
                }
            }
            bool did_work = false;

            if (stop_sending_.load(std::memory_order_relaxed)) {
                for (uint32_t id : ids) {
                    try {
                        PumpSends(id, 0);
                    } catch (const std::exception& ex) {
                        OnTransportError(id, ex.what());
                    }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }

            const double now_ns = static_cast<double>(NowNs());
            if (pacing_mode_ == PacingMode::TotalPps && global_next_send_time_ns_ > now_ns) {
                const auto sleep_ns = static_cast<int64_t>(std::min(global_next_send_time_ns_ - now_ns, 500'000.0));
                std::this_thread::sleep_for(std::chrono::nanoseconds(std::max<int64_t>(sleep_ns, 50'000)));
                continue;
            }

            if (!ids.empty()) {
                const size_t start = next_paced_connection_index_ % ids.size();
                for (size_t attempt = 0; attempt < ids.size(); ++attempt) {
                    const size_t idx = (start + attempt) % ids.size();
                    const uint32_t id = ids[idx];
                    try {
                        if (!MaySendPacedMessage(id, now_ns)) {
                            continue;
                        }
                        if (PumpSends(id, 1) > 0) {
                            did_work = true;
                            next_paced_connection_index_ = idx + 1;
                            if (pacing_mode_ == PacingMode::TotalPps) {
                                global_next_send_time_ns_ += pacing_interval_ns_;
                                if (global_next_send_time_ns_ + (pacing_interval_ns_ * 4.0) < now_ns) {
                                    global_next_send_time_ns_ = now_ns;
                                }
                                break;
                            }
                        }
                    } catch (const std::exception& ex) {
                        OnTransportError(id, ex.what());
                    }
                }
            }

            if (!did_work) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    }

    bool MaySendPacedMessage(uint32_t connection_id, double now_ns) {
        if (pacing_mode_ == PacingMode::Unlimited) {
            return true;
        }
        if (pacing_mode_ == PacingMode::TotalPps) {
            return true;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = states_.find(connection_id);
        if (it == states_.end()) {
            return false;
        }
        auto& state = it->second;
        if (!state.connected || state.connection == nullptr || state.send_closed || !ShouldSendOnConnection(connection_id)) {
            return false;
        }
        if (state.next_send_time_ns == 0.0) {
            state.next_send_time_ns = now_ns;
        }
        return state.next_send_time_ns <= now_ns;
    }

    size_t PumpSends(uint32_t connection_id, size_t max_messages = std::numeric_limits<size_t>::max()) {
        std::shared_ptr<ITransportConnection> connection;
        std::vector<std::pair<uint16_t, std::vector<uint8_t>>> sends;
        bool should_close_send = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = states_.find(connection_id);
            if (it == states_.end()) {
                return 0;
            }
            auto& state = it->second;
            if (!state.connected || state.connection == nullptr || state.send_closed) {
                return 0;
            }
            connection = state.connection;

            while (ShouldSendOnConnection(connection_id) &&
                   sends.size() < max_messages &&
                   !stop_sending_.load(std::memory_order_relaxed)) {
                if (pacing_mode_ == PacingMode::PerClientPps) {
                    const double now_ns = static_cast<double>(NowNs());
                    if (state.next_send_time_ns == 0.0) {
                        state.next_send_time_ns = now_ns;
                    }
                    if (state.next_send_time_ns > now_ns) {
                        break;
                    }
                    state.next_send_time_ns += pacing_interval_ns_;
                    if (state.next_send_time_ns + (pacing_interval_ns_ * 4.0) < now_ns) {
                        state.next_send_time_ns = now_ns;
                    }
                }
                bool selected_stream = false;
                for (uint32_t attempt = 0; attempt < config_.stream_count; ++attempt) {
                    const uint32_t ordinal = (state.next_send_stream_ordinal + attempt) % config_.stream_count;
                    const uint16_t stream_id = StreamIdFromOrdinal(ordinal);
                    auto& stream_state = state.streams[stream_id];
                    if (stream_state.profile.name.empty()) {
                        stream_state.profile = StreamProfileForOrdinal(config_, ordinal);
                    }
                    if ((stream_state.next_sequence - stream_state.echoed_messages) >= stream_state.profile.max_inflight) {
                        continue;
                    }
                    if (config_.qualcomm_method && stream_state.next_sequence >= config_.message_count) {
                        continue;
                    }
                    if (config_.qualcomm_method && config_.qualcomm_gap_ms > 0 &&
                        stream_state.next_send_time_ns > static_cast<double>(NowNs())) {
                        continue;
                    }
                    if (stream_state.profile.send_pps > 0) {
                        const double now_ns = static_cast<double>(NowNs());
                        if (stream_state.next_send_time_ns == 0.0) {
                            stream_state.next_send_time_ns = now_ns;
                        }
                        if (stream_state.next_send_time_ns > now_ns) {
                            continue;
                        }
                        const double interval_ns = 1'000'000'000.0 / static_cast<double>(stream_state.profile.send_pps);
                        stream_state.next_send_time_ns += interval_ns;
                        if (stream_state.next_send_time_ns + (interval_ns * 4.0) < now_ns) {
                            stream_state.next_send_time_ns = now_ns;
                        }
                    }
                    std::vector<uint8_t> frame(stream_state.profile.message_size);
                    MessageHeader header{};
                    header.magic = kMessageMagic;
                    header.reserved = static_cast<uint32_t>(frame.size());
                    header.sequence = stream_state.next_sequence++;
                    header.send_timestamp_ns = NowNs();
                    std::memcpy(frame.data(), &header, sizeof(header));
                    for (uint32_t i = sizeof(header); i < frame.size(); ++i) {
                        frame[i] = static_cast<uint8_t>(header.sequence + i);
                    }
                    if (config_.qualcomm_method) {
                        SetQualcommHop(frame, kQualcommHopInitial);
                    }
                    sends.push_back({stream_id, std::move(frame)});
                    state.next_send_stream_ordinal = (ordinal + 1) % config_.stream_count;
                    selected_stream = true;
                    break;
                }
                if (!selected_stream) {
                    break;
                }
            }

            if (stop_sending_.load(std::memory_order_relaxed) &&
                AllStreamsDrained(state) &&
                !state.send_closed) {
                state.send_closed = true;
                should_close_send = true;
            }
        }

        for (auto& send : sends) {
            auto& stream_id = send.first;
            auto& frame = send.second;
            try {
                MessageHeader header{};
                std::memcpy(&header, frame.data(), sizeof(header));
                header.send_timestamp_ns = NowNs();
                std::memcpy(frame.data(), &header, sizeof(header));
                connection->SendCopy(frame.data(), frame.size(), stream_id);
                stats_.AddSent(frame.size());
                stream_metrics_[stream_id - config_.sctp_stream_id].AddSent(frame.size());
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
        return sends.size();
    }

    const AppConfig& config_;
    Stats& stats_;
    QualcommTraceWriter trace_;
    std::mutex mutex_;
    std::condition_variable done_cv_;
    std::map<uint32_t, LoadClientConnectionState> states_;
    std::set<uint32_t> closed_connections_;
    std::vector<StreamMetrics> stream_metrics_;
    std::atomic<bool> stop_sending_{false};
    const PacingMode pacing_mode_{PacingMode::Unlimited};
    const bool paced_{false};
    const uint32_t active_send_connection_count_{0};
    const double pacing_interval_ns_{0.0};
    double global_next_send_time_ns_{0.0};
    size_t next_paced_connection_index_{0};
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
        if (config_.sctp_tls) {
            poll_thread_ = std::thread([this]() { PollLoop(); });
        }

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

        WakeAcceptLoops();

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

        if (poll_thread_.joinable()) {
            poll_thread_.join();
        }

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
        int buffer_size = 4 * 1024 * 1024;
        if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SO_SNDBUF)"));
        }
        if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SO_RCVBUF)"));
        }
        int fragment_interleave = 2;
        if (::setsockopt(fd, IPPROTO_SCTP, SCTP_FRAGMENT_INTERLEAVE, &fragment_interleave, sizeof(fragment_interleave)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SCTP_FRAGMENT_INTERLEAVE)"));
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
        sctp_initmsg initmsg{};
        initmsg.sinit_num_ostreams = static_cast<uint16_t>(config_.sctp_stream_id + config_.stream_count);
        initmsg.sinit_max_instreams = static_cast<uint16_t>(config_.sctp_stream_id + config_.stream_count);
        if (::setsockopt(fd, IPPROTO_SCTP, SCTP_INITMSG, &initmsg, sizeof(initmsg)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SCTP_INITMSG)"));
        }
        int recvrcvinfo = 1;
        if (::setsockopt(fd, IPPROTO_SCTP, SCTP_RECVRCVINFO, &recvrcvinfo, sizeof(recvrcvinfo)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SCTP_RECVRCVINFO)"));
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
            if (stopped_.load(std::memory_order_relaxed)) {
                ::close(accepted_fd);
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
                if (connection->UsesExternalPolling()) {
                    WakePollLoop();
                } else {
                    connection->StartReceiveLoop();
                }
            } catch (const std::exception& ex) {
                ::close(accepted_fd);
                handler_.OnTransportError(std::numeric_limits<uint32_t>::max(), ex.what());
            }
        }
    }

    void WakeAcceptLoops() const {
        for (uint32_t i = 0; i < config_.server_count; ++i) {
            const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
            if (fd < 0) {
                continue;
            }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<uint16_t>(config_.base_port + i));
            const std::string host = config_.bind == "0.0.0.0" ? "127.0.0.1" : config_.bind;
            if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1) {
                (void)::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            }
            ::close(fd);
        }
    }

    void PollLoop() {
        while (!stopped_.load(std::memory_order_relaxed) && !g_stop_requested.load(std::memory_order_relaxed)) {
            std::vector<std::shared_ptr<SctpConnection>> connections;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [_, connection] : connections_) {
                    connections.push_back(connection);
                }
            }

            if (connections.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            std::vector<pollfd> pollfds;
            std::vector<std::shared_ptr<SctpConnection>> polled_connections;
            pollfds.reserve(connections.size());
            polled_connections.reserve(connections.size());
            for (const auto& connection : connections) {
                const int fd = connection->Fd();
                if (fd < 0) {
                    continue;
                }
                pollfd pfd{};
                pfd.fd = fd;
                pfd.events = POLLIN | POLLERR | POLLHUP;
                pollfds.push_back(pfd);
                polled_connections.push_back(connection);
            }

            const int rc = ::poll(pollfds.data(), pollfds.size(), 5);
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                handler_.OnTransportError(std::numeric_limits<uint32_t>::max(), SocketErrorString("poll"));
                return;
            }
            if (rc == 0) {
                continue;
            }

            for (size_t i = 0; i < pollfds.size(); ++i) {
                if (pollfds[i].revents == 0) {
                    continue;
                }
                polled_connections[i]->HandlePollEvents(pollfds[i].revents);
            }
        }
    }

    void WakePollLoop() {}

    const AppConfig& config_;
    ITransportEventHandler& handler_;
    std::atomic<bool> stopped_{false};
    std::atomic<uint32_t> next_connection_id_{0};
    std::mutex mutex_;
    std::map<uint32_t, std::shared_ptr<SctpConnection>> connections_;
    std::vector<int> listeners_;
    std::vector<std::thread> accept_threads_;
    std::thread poll_thread_;
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
            if (!connection->UsesExternalPolling()) {
                connection->StartReceiveLoop();
            }
        }
        if (config_.sctp_tls) {
            poll_thread_ = std::thread([this]() { PollLoop(); });
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
        if (poll_thread_.joinable()) {
            poll_thread_.join();
        }
        for (const auto& connection : connections) {
            connection->Join();
        }
    }

  private:
    void PollLoop() {
        while (!g_stop_requested.load(std::memory_order_relaxed)) {
            std::vector<std::shared_ptr<SctpConnection>> connections;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (connections_.empty()) {
                    return;
                }
                for (const auto& [_, connection] : connections_) {
                    connections.push_back(connection);
                }
            }

            std::vector<pollfd> pollfds;
            std::vector<std::shared_ptr<SctpConnection>> polled_connections;
            pollfds.reserve(connections.size());
            polled_connections.reserve(connections.size());
            for (const auto& connection : connections) {
                const int fd = connection->Fd();
                if (fd < 0) {
                    continue;
                }
                pollfd pfd{};
                pfd.fd = fd;
                pfd.events = POLLIN | POLLERR | POLLHUP;
                pollfds.push_back(pfd);
                polled_connections.push_back(connection);
            }
            if (pollfds.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            const int rc = ::poll(pollfds.data(), pollfds.size(), 5);
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                handler_.OnTransportError(std::numeric_limits<uint32_t>::max(), SocketErrorString("poll"));
                return;
            }
            if (rc == 0) {
                continue;
            }

            for (size_t i = 0; i < pollfds.size(); ++i) {
                if (pollfds[i].revents == 0) {
                    continue;
                }
                polled_connections[i]->HandlePollEvents(pollfds[i].revents);
            }
        }
    }

    void ConfigureSocket(int fd) const {
        int buffer_size = 4 * 1024 * 1024;
        if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SO_SNDBUF)"));
        }
        if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SO_RCVBUF)"));
        }
        int fragment_interleave = 2;
        if (::setsockopt(fd, IPPROTO_SCTP, SCTP_FRAGMENT_INTERLEAVE, &fragment_interleave, sizeof(fragment_interleave)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SCTP_FRAGMENT_INTERLEAVE)"));
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
        sctp_initmsg initmsg{};
        initmsg.sinit_num_ostreams = static_cast<uint16_t>(config_.sctp_stream_id + config_.stream_count);
        initmsg.sinit_max_instreams = static_cast<uint16_t>(config_.sctp_stream_id + config_.stream_count);
        if (::setsockopt(fd, IPPROTO_SCTP, SCTP_INITMSG, &initmsg, sizeof(initmsg)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SCTP_INITMSG)"));
        }
        int recvrcvinfo = 1;
        if (::setsockopt(fd, IPPROTO_SCTP, SCTP_RECVRCVINFO, &recvrcvinfo, sizeof(recvrcvinfo)) != 0) {
            throw std::runtime_error(SocketErrorString("setsockopt(SCTP_RECVRCVINFO)"));
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
    std::thread poll_thread_;
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
        if (!config_.qualcomm_method) {
            stats_printer_.Start();
        }

        std::cout << "sctp server listening on " << config_.bind << " ports ";
        for (uint32_t i = 0; i < config_.server_count; ++i) {
            if (i != 0) {
                std::cout << ",";
            }
            std::cout << (config_.base_port + i);
        }
        std::cout << std::endl;

        while (!g_stop_requested.load(std::memory_order_relaxed) &&
               !(config_.qualcomm_method && controller_.QualcommDone())) {
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
        transport_.Start();
        if (config_.qualcomm_method) {
            controller_.StartPacer();
            while (!g_stop_requested.load(std::memory_order_relaxed) && !controller_.QualcommDone()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            controller_.StopPacer();
            controller_.ForceShutdownAll();
            transport_.Stop();
            PrintSummary();
            return;
        }
        const auto deadline = Clock::now() + std::chrono::seconds(config_.duration_sec);
        const auto drain_deadline = deadline + std::chrono::milliseconds(config_.drain_timeout_ms);

        controller_.StartPacer();
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
        controller_.PrintStreamSummaries("client stream summary:");
    }

    const AppConfig& config_;
    Stats stats_;
    StatsPrinter stats_printer_;
    LoadClientController controller_;
    SctpClientTransport transport_;
    std::atomic<bool> stop_requested_{false};
};


void RunSctpServer(const AppConfig& config) {
    SctpServer server(config);
    server.Run();
}

void RunSctpClient(const AppConfig& config) {
    SctpClient client(config);
    client.Run();
}

} // namespace proto_test
