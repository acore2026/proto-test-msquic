# Qualcomm-Method Single-Client Stack Timing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Qualcomm R3-261501-aligned benchmark mode for one client and one server that records per-host application entry/exit timestamps, supports a 30,000-message RAN-AMF-RAN-AMF ping-pong flow, and leaves the app-level pacing/duration scheduler bypassed.

**Architecture:** Keep the existing load generator and transports, but add a strict `--qualcomm-method=1` mode that forces one client, one server, one stream, `max_inflight=1`, no PPS pacing, no periodic stats, and message-count-driven completion. Add monotonic and realtime trace logging inside application entry/exit points, then add a post-processing script that combines app traces with packet-capture CSV exports to compute `(Tegress - Tingress) - (Ty - Tx)` for AMF and RAN roles.

**Tech Stack:** C++17, MSQuic C API, Linux SCTP sockets, OpenSSL for existing optional SCTP-DTLS, Bash helper scripts, Python 3 standard library, tcpdump/tshark or Wireshark for packet timestamp export.

---

## Scope And Constraints

- This mode aligns the measurement method with Qualcomm R3-261501, not the existing N2 scaling benchmark.
- This mode must run with `--server-count=1`, `--clients=1`, `--stream-count=1`, and `--max-inflight=1`.
- This mode must reject `--send-pps`, `--send-pps-per-client`, and `--stream-profile`.
- This mode must not start the app's pacing thread.
- This mode must not use duration-based sending as the stop condition; `--message-count` controls completion.
- This mode must not start periodic `StatsPrinter` output by default, because it adds timing and CPU noise.
- This mode bypasses the load generator's scheduler/pacer. It cannot bypass Linux kernel scheduling, NIC queues, interrupt moderation, QUIC/SCTP internals, or host OS process scheduling. Two-host test instructions will document CPU pinning and real-time priority as optional external controls.
- Qualcomm's paper used LSQUIC for QUIC. This project uses MSQuic. The output must report implementation names clearly: `sctp-linux` and `msquic`, not generic `QUIC` when comparing to Qualcomm.
- Qualcomm's plain SCTP measurement used no TLS/DTLS. The aligned comparison should run `--protocol=sctp` without `--sctp-tls=1`, and `--protocol=msquic` for QUIC. `sctp-dtls` may be left supported by the code path, but the documentation must mark it as out of Qualcomm scope.

## Measurement Model

Qualcomm's reported values are per-host stack turnarounds:

```text
NF1 / AMF stack time = (T3 - T2) - (AMF_app_exit - AMF_app_entry)
NF2 / RAN stack time = (T5 - T4) - (RAN_app_exit - RAN_app_entry)
```

The new mode implements this packet flow:

```text
RAN client sends initial request, hop=0       -> T1
AMF server receives initial request          -> T2, AMF_app_entry
AMF server reflects response, hop=1          -> AMF_app_exit, T3
RAN client receives reflected response       -> T4, RAN_app_entry
RAN client re-sends it as final packet hop=2 -> RAN_app_exit, T5
AMF server receives final packet             -> T6
```

The application trace CSV supplies `AMF_app_entry`, `AMF_app_exit`, `RAN_app_entry`, and `RAN_app_exit` in realtime nanoseconds. Packet capture supplies T1-T6. The post-processor pairs packet timestamps around the app timestamps on the same host, so host-to-host clock sync is not required for stack-time calculations.

## File Structure

- Modify `src/main.cpp`
  - Add Qualcomm-mode CLI fields and validation.
  - Add `RealtimeNs()`, hop helpers, trace writer, and trace records.
  - Add Qualcomm-mode send/receive behavior for SCTP/common transport path.
  - Add Qualcomm-mode send/receive behavior for MSQuic path.
  - Bypass app-level scheduling and periodic stats in Qualcomm mode.
- Create `benchmarks/qualcomm-method/README.md`
  - Document two-host setup, commands, capture filters, trace files, pcap export, and interpretation.
- Create `benchmarks/qualcomm-method/export-pcap-csv.sh`
  - Convert pcap files to normalized CSV using `tshark`.
- Create `benchmarks/qualcomm-method/compute-stack-times.py`
  - Combine app trace CSV and packet CSV into AMF/RAN stack time summaries.
- Create `benchmarks/qualcomm-method/test-compute-stack-times.py`
  - Unit tests for pairing and stack-time calculation using synthetic traces.
- Optionally modify `README.md`
  - Add a short pointer to `benchmarks/qualcomm-method/README.md`.

---

### Task 1: Add Qualcomm Mode Configuration And Validation

**Files:**
- Modify: `src/main.cpp:243-455`

- [ ] **Step 1: Extend usage text**

Add these lines under common options in `PrintUsage()` after `--stats-interval-ms`:

```cpp
        << "  --qualcomm-method=1        Enable single-client stack timing trace mode\n"
        << "  --message-count=N          Messages to send in Qualcomm mode, default 30000\n"
        << "  --trace-file=FILE          App timestamp CSV for Qualcomm mode\n\n"
```

Expected behavior:
- `msquic-loadtest server --help` prints the new options.
- `msquic-loadtest client --help` prints the new options.

- [ ] **Step 2: Add fields to `AppConfig`**

Add these fields at the end of `AppConfig`:

```cpp
    bool qualcomm_method{false};
    uint64_t message_count{30000};
    std::string trace_file;
```

- [ ] **Step 3: Parse Qualcomm mode before dependent defaults**

At the start of `LoadConfig`, after `config.mode = args.mode;`, add:

```cpp
    config.qualcomm_method = GetBool(args, "qualcomm-method", false);
```

Then change the `message-size`, `server-count`, `stream-count`, and `stats-interval-ms` defaults to use Qualcomm defaults when enabled:

```cpp
    config.server_count = GetNumber<uint32_t>(args, "server-count", 1);
    config.stream_count = GetNumber<uint32_t>(args, "stream-count", 1);
    config.message_size = GetNumber<uint32_t>(args, "message-size", config.qualcomm_method ? 50U : 1024U);
    config.idle_timeout_ms = GetNumber<uint64_t>(args, "idle-timeout-ms", 30000);
    config.stats_interval_ms = GetNumber<uint64_t>(args, "stats-interval-ms", config.qualcomm_method ? 0U : 1000U);
```

Add parsing for message count and trace file after `ca_file`:

```cpp
    config.message_count = GetNumber<uint64_t>(args, "message-count", 30000);
    config.trace_file = GetString(args, "trace-file", "");
```

- [ ] **Step 4: Add strict validation**

After the existing SCTP-DTLS multi-stream validation block, add:

```cpp
    if (config.qualcomm_method) {
        if (config.server_count != 1) {
            throw std::runtime_error("--qualcomm-method=1 requires --server-count=1");
        }
        if (config.stream_count != 1) {
            throw std::runtime_error("--qualcomm-method=1 requires --stream-count=1");
        }
        if (config.message_size != 50) {
            throw std::runtime_error("--qualcomm-method=1 requires --message-size=50 for R3-261501 alignment");
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
```

Inside the client-specific block, after `verify_peer`, add:

```cpp
        if (config.qualcomm_method) {
            if (config.client_count != 1) {
                throw std::runtime_error("--qualcomm-method=1 requires --clients=1");
            }
            if (config.max_inflight != 1) {
                throw std::runtime_error("--qualcomm-method=1 requires --max-inflight=1 to avoid scheduler overlap");
            }
            if (config.send_pps != 0 || config.send_pps_per_client != 0) {
                throw std::runtime_error("--qualcomm-method=1 bypasses pacing; do not set --send-pps or --send-pps-per-client");
            }
        }
```

- [ ] **Step 5: Run build**

Run:

```bash
cmake --build build-multistream -j
```

Expected:
- Build succeeds if the local MSQuic runtime is available.
- If local runtime is unavailable, build the Docker image later in Task 8.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "feat: add Qualcomm method config"
```

---

### Task 2: Add Trace Clock, Hop Helpers, And CSV Writer

**Files:**
- Modify: `src/main.cpp:73-82`
- Modify: `src/main.cpp:480-530`

- [ ] **Step 1: Add realtime clock helper**

After `NowNs()`, add:

```cpp
uint64_t RealtimeNs() {
    timespec ts{};
    if (::clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        throw std::runtime_error(SocketErrorString("clock_gettime(CLOCK_REALTIME)"));
    }
    return (static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL) + static_cast<uint64_t>(ts.tv_nsec);
}
```

Because `SocketErrorString` is currently declared later, either move `SocketErrorString` above `RealtimeNs()` or use direct `std::strerror(errno)` in this helper:

```cpp
throw std::runtime_error(std::string("clock_gettime(CLOCK_REALTIME) failed: ") + std::strerror(errno));
```

Use the direct `std::strerror(errno)` version to keep the diff small.

- [ ] **Step 2: Add Qualcomm hop constants and helpers**

After `StreamProfileForOrdinal`, add:

```cpp
constexpr size_t kQualcommHopOffset = sizeof(MessageHeader);
constexpr uint8_t kQualcommHopInitial = 0;
constexpr uint8_t kQualcommHopReflected = 1;
constexpr uint8_t kQualcommHopFinal = 2;

void SetQualcommHop(std::vector<uint8_t>& frame, uint8_t hop) {
    if (frame.size() <= kQualcommHopOffset) {
        throw std::runtime_error("Qualcomm method frame too small for hop marker");
    }
    frame[kQualcommHopOffset] = hop;
}

uint8_t QualcommHopFromFrame(const uint8_t* data, size_t length) {
    if (length <= kQualcommHopOffset) {
        throw std::runtime_error("Qualcomm method frame too small for hop marker");
    }
    return data[kQualcommHopOffset];
}
```

- [ ] **Step 3: Add trace record and writer**

After `SendBuffer`, add:

```cpp
struct QualcommTraceRecord {
    std::string event;
    std::string role;
    std::string protocol;
    uint64_t sequence{0};
    uint32_t message_size{0};
    uint8_t hop_in{0};
    uint8_t hop_out{0};
    uint64_t app_entry_realtime_ns{0};
    uint64_t app_exit_realtime_ns{0};
    uint64_t app_entry_mono_ns{0};
    uint64_t app_exit_mono_ns{0};
};

class QualcommTraceWriter {
  public:
    QualcommTraceWriter() = default;

    explicit QualcommTraceWriter(const AppConfig& config, std::string role)
        : enabled_(config.qualcomm_method),
          role_(std::move(role)),
          protocol_(ProtocolName(config.protocol)) {
        if (!enabled_) {
            return;
        }
        output_.open(config.trace_file, std::ios::out | std::ios::trunc);
        if (!output_) {
            throw std::runtime_error("failed to open --trace-file=" + config.trace_file);
        }
        output_
            << "event,role,protocol,sequence,message_size,hop_in,hop_out,"
            << "app_entry_realtime_ns,app_exit_realtime_ns,"
            << "app_entry_mono_ns,app_exit_mono_ns\n";
    }

    bool enabled() const {
        return enabled_;
    }

    const std::string& role() const {
        return role_;
    }

    const std::string& protocol() const {
        return protocol_;
    }

    void Write(const QualcommTraceRecord& record) {
        if (!enabled_) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        output_ << record.event << ","
                << record.role << ","
                << record.protocol << ","
                << record.sequence << ","
                << record.message_size << ","
                << static_cast<unsigned>(record.hop_in) << ","
                << static_cast<unsigned>(record.hop_out) << ","
                << record.app_entry_realtime_ns << ","
                << record.app_exit_realtime_ns << ","
                << record.app_entry_mono_ns << ","
                << record.app_exit_mono_ns << "\n";
    }

  private:
    bool enabled_{false};
    std::string role_;
    std::string protocol_;
    std::ofstream output_;
    std::mutex mutex_;
};
```

Add `#include <fstream>` near the other includes.

- [ ] **Step 4: Run build**

```bash
cmake --build build-multistream -j
```

Expected:
- Compiler accepts the new writer and helpers.

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "feat: add Qualcomm trace writer"
```

---

### Task 3: Instrument SCTP/Common Server Turnaround

**Files:**
- Modify: `src/main.cpp:1496-1569`
- Modify: `src/main.cpp:2457-2502`

- [ ] **Step 1: Add pending send metadata**

After `LoadServerConnectionState`, add:

```cpp
struct QualcommPendingSend {
    uint16_t stream_id{0};
    std::vector<uint8_t> frame;
    uint64_t sequence{0};
    uint32_t message_size{0};
    uint8_t hop_in{0};
    uint64_t app_entry_realtime_ns{0};
    uint64_t app_entry_mono_ns{0};
};
```

- [ ] **Step 2: Add trace writer to `LoadServerController`**

Change the constructor initializer:

```cpp
    LoadServerController(const AppConfig& config, Stats& stats)
        : config_(config), stats_(stats), trace_(config, "amf") {}
```

Add field:

```cpp
    QualcommTraceWriter trace_;
```

- [ ] **Step 3: Change `OnData` to trace AMF processing**

In `LoadServerController::OnData`, replace `std::vector<std::vector<uint8_t>> sends;` with:

```cpp
        std::vector<QualcommPendingSend> qualcomm_sends;
        std::vector<std::pair<uint16_t, std::vector<uint8_t>>> regular_sends;
```

Inside the frame extraction loop, replace `sends.push_back(std::move(frame));` with:

```cpp
                if (config_.qualcomm_method) {
                    const uint64_t entry_rt = RealtimeNs();
                    const uint64_t entry_mono = NowNs();
                    MessageHeader header{};
                    std::memcpy(&header, frame.data(), sizeof(header));
                    const uint8_t hop = QualcommHopFromFrame(frame.data(), frame.size());
                    if (hop == kQualcommHopInitial) {
                        SetQualcommHop(frame, kQualcommHopReflected);
                        qualcomm_sends.push_back(QualcommPendingSend{
                            stream_id,
                            std::move(frame),
                            header.sequence,
                            *frame_size,
                            hop,
                            entry_rt,
                            entry_mono,
                        });
                    } else if (hop == kQualcommHopFinal) {
                        const uint64_t exit_rt = RealtimeNs();
                        const uint64_t exit_mono = NowNs();
                        trace_.Write(QualcommTraceRecord{
                            "final_receive",
                            trace_.role(),
                            trace_.protocol(),
                            header.sequence,
                            *frame_size,
                            hop,
                            hop,
                            entry_rt,
                            exit_rt,
                            entry_mono,
                            exit_mono,
                        });
                    } else {
                        throw std::runtime_error("unexpected Qualcomm hop at AMF server");
                    }
                } else {
                    regular_sends.push_back({stream_id, std::move(frame)});
                }
```

Replace the send loop with:

```cpp
        for (auto& send : regular_sends) {
            connection->SendCopy(send.second.data(), send.second.size(), send.first);
            stats_.AddReceived(send.second.size());
            stats_.AddSent(send.second.size());
        }

        for (auto& pending : qualcomm_sends) {
            const uint64_t exit_rt = RealtimeNs();
            const uint64_t exit_mono = NowNs();
            trace_.Write(QualcommTraceRecord{
                "turnaround",
                trace_.role(),
                trace_.protocol(),
                pending.sequence,
                pending.message_size,
                pending.hop_in,
                kQualcommHopReflected,
                pending.app_entry_realtime_ns,
                exit_rt,
                pending.app_entry_mono_ns,
                exit_mono,
            });
            connection->SendCopy(pending.frame.data(), pending.frame.size(), pending.stream_id);
            stats_.AddReceived(pending.frame.size());
            stats_.AddSent(pending.frame.size());
        }
```

- [ ] **Step 4: Bypass periodic server stats in Qualcomm mode**

In `SctpServer::Run()`, wrap `stats_printer_.Start()`:

```cpp
        if (!config_.qualcomm_method && config_.stats_interval_ms > 0) {
            stats_printer_.Start();
        }
```

Wrap `stats_printer_.Stop()`:

```cpp
        if (!config_.qualcomm_method && config_.stats_interval_ms > 0) {
            stats_printer_.Stop();
        }
```

- [ ] **Step 5: Run build**

```bash
cmake --build build-multistream -j
```

Expected:
- Build succeeds.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "feat: trace Qualcomm SCTP server turnaround"
```

---

### Task 4: Implement SCTP/Common Client Ping-Pong And Scheduler Bypass

**Files:**
- Modify: `src/main.cpp:1571-2045`
- Modify: `src/main.cpp:2504-2560`

- [ ] **Step 1: Extend client state**

Add fields to `LoadClientStreamState`:

```cpp
    uint64_t qualcomm_reflected_messages{0};
    uint64_t qualcomm_final_sent_messages{0};
```

Add field to `LoadClientController`:

```cpp
    QualcommTraceWriter trace_;
```

Change constructor initializer:

```cpp
          pacing_interval_ns_(ComputePacingIntervalNs(config, pacing_mode_, active_send_connection_count_)),
          trace_(config, "ran") {}
```

- [ ] **Step 2: Bypass pacer in Qualcomm mode**

At the start of `LoadClientController::StartPacer()`, add:

```cpp
        if (config_.qualcomm_method) {
            return;
        }
```

At the start of `PacerLoop()`, add:

```cpp
        if (config_.qualcomm_method) {
            return;
        }
```

- [ ] **Step 3: Add Qualcomm completion helper**

In the private section of `LoadClientController`, add:

```cpp
    bool QualcommComplete(const LoadClientConnectionState& state) const {
        if (!config_.qualcomm_method) {
            return false;
        }
        const uint16_t stream_id = StreamIdFromOrdinal(0);
        const auto it = state.streams.find(stream_id);
        if (it == state.streams.end()) {
            return false;
        }
        return it->second.qualcomm_reflected_messages >= config_.message_count &&
               it->second.qualcomm_final_sent_messages >= config_.message_count;
    }
```

- [ ] **Step 4: Add method to send a Qualcomm final packet**

In the private section of `LoadClientController`, add:

```cpp
    void SendQualcommFinal(
        const std::shared_ptr<ITransportConnection>& connection,
        uint16_t stream_id,
        std::vector<uint8_t> frame,
        uint64_t sequence,
        uint64_t app_entry_rt,
        uint64_t app_entry_mono) {
        SetQualcommHop(frame, kQualcommHopFinal);
        const uint64_t exit_rt = RealtimeNs();
        const uint64_t exit_mono = NowNs();
        trace_.Write(QualcommTraceRecord{
            "turnaround",
            trace_.role(),
            trace_.protocol(),
            sequence,
            static_cast<uint32_t>(frame.size()),
            kQualcommHopReflected,
            kQualcommHopFinal,
            app_entry_rt,
            exit_rt,
            app_entry_mono,
            exit_mono,
        });
        connection->SendCopy(frame.data(), frame.size(), stream_id);
        stats_.AddSent(frame.size());
        stream_metrics_[stream_id - config_.sctp_stream_id].AddSent(frame.size());
    }
```

- [ ] **Step 5: Change client receive handling for Qualcomm mode**

Inside `LoadClientController::OnData`, after copying `MessageHeader header`, branch before the normal latency update:

```cpp
                if (config_.qualcomm_method) {
                    const uint64_t app_entry_rt = RealtimeNs();
                    const uint64_t app_entry_mono = NowNs();
                    const uint8_t hop = QualcommHopFromFrame(
                        stream_state.receive_buffer.data(),
                        stream_state.receive_buffer.size());
                    if (hop != kQualcommHopReflected) {
                        throw std::runtime_error("RAN client expected reflected Qualcomm hop");
                    }
                    std::vector<uint8_t> final_frame(
                        stream_state.receive_buffer.begin(),
                        stream_state.receive_buffer.begin() + static_cast<std::ptrdiff_t>(actual_frame_size));
                    stream_state.receive_buffer.erase(
                        stream_state.receive_buffer.begin(),
                        stream_state.receive_buffer.begin() + static_cast<std::ptrdiff_t>(actual_frame_size));
                    ++stream_state.echoed_messages;
                    ++stream_state.qualcomm_reflected_messages;
                    const auto latency_ns = NowNs() - header.send_timestamp_ns;
                    stats_.AddReceived(actual_frame_size);
                    stats_.AddLatencyNs(latency_ns);
                    stream_metrics_[stream_id - config_.sctp_stream_id].AddReceived(actual_frame_size, latency_ns);
                    SendQualcommFinal(
                        connection,
                        stream_id,
                        std::move(final_frame),
                        header.sequence,
                        app_entry_rt,
                        app_entry_mono);
                    ++stream_state.qualcomm_final_sent_messages;
                    continue;
                }
```

Keep the existing non-Qualcomm path unchanged after this branch.

- [ ] **Step 6: Limit initial sends by message count**

In `PumpSends`, inside the stream selection loop before creating `frame`, add:

```cpp
                    if (config_.qualcomm_method && stream_state.next_sequence >= config_.message_count) {
                        continue;
                    }
```

After filling the payload, set the initial hop:

```cpp
                    if (config_.qualcomm_method) {
                        SetQualcommHop(frame, kQualcommHopInitial);
                    }
```

Change the close condition:

```cpp
            if (config_.qualcomm_method) {
                if (QualcommComplete(state) && !state.send_closed) {
                    state.send_closed = true;
                    should_close_send = true;
                }
            } else if (stop_sending_.load(std::memory_order_relaxed) &&
                AllStreamsDrained(state) &&
                !state.send_closed) {
                state.send_closed = true;
                should_close_send = true;
            }
```

- [ ] **Step 7: Send next initial only from event path**

At the end of `OnData`, keep:

```cpp
        if (!paced_) {
            PumpSends(connection->Id());
        }
```

Because Qualcomm mode rejects pacing and `max_inflight=1`, this call becomes event-driven one-at-a-time sending. No pacer thread or duration scheduler is involved.

- [ ] **Step 8: Change SCTP client run loop for message-count completion**

In `SctpClient::Run()`, branch after `transport_.Start()`:

```cpp
        if (config_.qualcomm_method) {
            controller_.StartPacer(); // returns immediately in Qualcomm mode
            while (!g_stop_requested.load(std::memory_order_relaxed)) {
                if (controller_.WaitUntilDone(config_.client_count, std::chrono::milliseconds(200))) {
                    break;
                }
            }
            controller_.ForceShutdownAll();
            transport_.Stop();
            controller_.StopPacer();
            PrintSummary();
            return;
        }
```

Do not start `stats_printer_` in this branch.

- [ ] **Step 9: Run build**

```bash
cmake --build build-multistream -j
```

Expected:
- Build succeeds.
- Qualcomm SCTP mode sends exactly `--message-count` initial requests, receives exactly that many reflected responses, and sends exactly that many final packets.

- [ ] **Step 10: Commit**

```bash
git add src/main.cpp
git commit -m "feat: add Qualcomm SCTP client ping-pong"
```

---

### Task 5: Instrument MSQuic Server Turnaround

**Files:**
- Modify: `src/main.cpp:2696-2948`

- [ ] **Step 1: Add trace writer to `Server`**

Add field:

```cpp
    QualcommTraceWriter trace_;
```

Change constructor initializer:

```cpp
          configuration_(api_, registration_.get(), config_, false),
          stats_printer_("server", stats_, config_.stats_interval_ms),
          trace_(config_, "amf") {}
```

- [ ] **Step 2: Bypass periodic server stats in Qualcomm mode**

In `Server::Run()`, wrap `stats_printer_.Start()` and `stats_printer_.Stop()` the same way as SCTP:

```cpp
        if (!config_.qualcomm_method && config_.stats_interval_ms > 0) {
            stats_printer_.Start();
        }
```

```cpp
        if (!config_.qualcomm_method && config_.stats_interval_ms > 0) {
            stats_printer_.Stop();
        }
```

- [ ] **Step 3: Add AMF trace in `Server::OnStreamEvent`**

In the frame extraction loop, after `auto send = std::make_unique<SendBuffer>(*frame_size);`, add:

```cpp
                    const uint64_t entry_rt = RealtimeNs();
                    const uint64_t entry_mono = NowNs();
                    MessageHeader header{};
                    std::memcpy(&header, context->receive_buffer.data(), sizeof(header));
                    const uint8_t hop = QualcommHopFromFrame(context->receive_buffer.data(), *frame_size);
```

For Qualcomm mode, handle initial and final hops:

```cpp
                    if (config_.qualcomm_method) {
                        if (hop == kQualcommHopInitial) {
                            std::memcpy(send->storage.data(), context->receive_buffer.data(), *frame_size);
                            SetQualcommHop(send->storage, kQualcommHopReflected);
                            context->receive_buffer.erase(
                                context->receive_buffer.begin(),
                                context->receive_buffer.begin() + static_cast<std::ptrdiff_t>(*frame_size));
                            ++context->pending_sends;
                            const uint64_t exit_rt = RealtimeNs();
                            const uint64_t exit_mono = NowNs();
                            trace_.Write(QualcommTraceRecord{
                                "turnaround",
                                trace_.role(),
                                trace_.protocol(),
                                header.sequence,
                                *frame_size,
                                hop,
                                kQualcommHopReflected,
                                entry_rt,
                                exit_rt,
                                entry_mono,
                                exit_mono,
                            });
                            sends.push_back(std::move(send));
                        } else if (hop == kQualcommHopFinal) {
                            context->receive_buffer.erase(
                                context->receive_buffer.begin(),
                                context->receive_buffer.begin() + static_cast<std::ptrdiff_t>(*frame_size));
                            const uint64_t exit_rt = RealtimeNs();
                            const uint64_t exit_mono = NowNs();
                            trace_.Write(QualcommTraceRecord{
                                "final_receive",
                                trace_.role(),
                                trace_.protocol(),
                                header.sequence,
                                *frame_size,
                                hop,
                                hop,
                                entry_rt,
                                exit_rt,
                                entry_mono,
                                exit_mono,
                            });
                        } else {
                            return QUIC_STATUS_INVALID_PARAMETER;
                        }
                        continue;
                    }
```

Keep the existing non-Qualcomm copy/erase/send behavior after this branch.

- [ ] **Step 4: Verify exit timestamp placement**

The `exit_rt` timestamp must be captured immediately before the app hands data to the transport. If review shows the code logs exit too early, move trace writing from the extraction loop to the send loop immediately before `api_->StreamSend(...)`. The accepted placement is:

```cpp
const uint64_t exit_rt = RealtimeNs();
const auto status = api_->StreamSend(...);
```

If moving to the send loop, store a small metadata struct beside the `SendBuffer` so each send has the correct entry timestamp and sequence.

- [ ] **Step 5: Run build**

```bash
cmake --build build-multistream -j
```

Expected:
- Build succeeds.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "feat: trace Qualcomm MSQuic server turnaround"
```

---

### Task 6: Implement MSQuic Client Ping-Pong And Scheduler Bypass

**Files:**
- Modify: `src/main.cpp:2950-3509`

- [ ] **Step 1: Add trace writer and counters**

Add field to `Client`:

```cpp
    QualcommTraceWriter trace_;
```

Change constructor initializer:

```cpp
          pacing_interval_ns_(ComputePacingIntervalNs(config, pacing_mode_, active_send_connection_count_)),
          trace_(config_, "ran") {}
```

Add fields to `StreamContext`:

```cpp
        uint64_t qualcomm_reflected_messages{0};
        uint64_t qualcomm_final_send_completions{0};
```

- [ ] **Step 2: Bypass pacer in Qualcomm mode**

At the start of `Client::StartPacer()` and `Client::PacerLoop()`, add:

```cpp
        if (config_.qualcomm_method) {
            return;
        }
```

- [ ] **Step 3: Change MSQuic client run loop**

In `Client::Run()`, branch after `StartConnections()`:

```cpp
        if (config_.qualcomm_method) {
            while (!g_stop_requested.load(std::memory_order_relaxed)) {
                std::unique_lock<std::mutex> lock(done_mutex_);
                if (done_cv_.wait_for(lock, std::chrono::milliseconds(200), [this]() {
                        return active_connections_.load(std::memory_order_relaxed) == 0;
                    })) {
                    break;
                }
            }
            WaitForConnectionsToClose();
            PrintSummary();
            return;
        }
```

Do not call `StartPacer()` or `stats_printer_.Start()` in this branch.

- [ ] **Step 4: Add Qualcomm final send helper**

In `Client`, add:

```cpp
    bool SendQualcommFinal(ConnectionContext& connection, StreamContext& stream_ctx, std::vector<uint8_t> frame) {
        MessageHeader header{};
        std::memcpy(&header, frame.data(), sizeof(header));
        SetQualcommHop(frame, kQualcommHopFinal);
        const uint64_t exit_rt = RealtimeNs();
        const uint64_t exit_mono = NowNs();
        trace_.Write(QualcommTraceRecord{
            "turnaround",
            trace_.role(),
            trace_.protocol(),
            header.sequence,
            static_cast<uint32_t>(frame.size()),
            kQualcommHopReflected,
            kQualcommHopFinal,
            stream_ctx.pending_app_entry_realtime_ns,
            exit_rt,
            stream_ctx.pending_app_entry_mono_ns,
            exit_mono,
        });
        auto send = std::make_unique<SendBuffer>(frame.size());
        std::memcpy(send->storage.data(), frame.data(), frame.size());
        auto* raw_send = send.release();
        const auto status = api_->StreamSend(
            stream_ctx.stream,
            &raw_send->quic_buffer,
            1,
            QUIC_SEND_FLAG_NONE,
            raw_send);
        if (QUIC_FAILED(status)) {
            delete raw_send;
            std::cerr << "Qualcomm final StreamSend failed: " << StatusToHex(status) << std::endl;
            api_->ConnectionShutdown(connection.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 1);
            return false;
        }
        stats_.AddSent(raw_send->quic_buffer.Length);
        stream_metrics_[stream_ctx.ordinal].AddSent(raw_send->quic_buffer.Length);
        return true;
    }
```

Before using this helper, add these fields to `StreamContext`:

```cpp
        uint64_t pending_app_entry_realtime_ns{0};
        uint64_t pending_app_entry_mono_ns{0};
```

- [ ] **Step 5: Limit initial sends by message count and mark initial hop**

In `Client::PumpSends`, before selecting a stream as eligible, add:

```cpp
                if (config_.qualcomm_method && candidate->next_sequence >= config_.message_count) {
                    continue;
                }
```

After filling the frame payload and before `StreamSend`, add:

```cpp
            if (config_.qualcomm_method) {
                SetQualcommHop(raw_send->storage, kQualcommHopInitial);
            }
```

Change stream shutdown logic:

```cpp
        if (config_.qualcomm_method) {
            for (auto& stream_ctx : connection.stream_contexts) {
                std::lock_guard<std::mutex> stream_lock(stream_ctx->mutex);
                if (!stream_ctx->shutdown_started &&
                    !stream_ctx->closed &&
                    stream_ctx->stream_started &&
                    stream_ctx->qualcomm_reflected_messages >= config_.message_count &&
                    stream_ctx->qualcomm_final_send_completions >= config_.message_count) {
                    stream_ctx->shutdown_started = true;
                    api_->StreamShutdown(stream_ctx->stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
                }
            }
        } else if (stop_sending_.load(std::memory_order_relaxed) && AllStreamsDrained(connection)) {
            for (auto& stream_ctx : connection.stream_contexts) {
                std::lock_guard<std::mutex> stream_lock(stream_ctx->mutex);
                if (!stream_ctx->shutdown_started && !stream_ctx->closed && stream_ctx->stream_started) {
                    stream_ctx->shutdown_started = true;
                    api_->StreamShutdown(stream_ctx->stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
                }
            }
        }
```

- [ ] **Step 6: Handle reflected hop in `OnStreamEvent`**

Inside `QUIC_STREAM_EVENT_RECEIVE`, after reading `MessageHeader header` and `actual_frame_size`, add:

```cpp
                    if (config_.qualcomm_method) {
                        const uint8_t hop = QualcommHopFromFrame(
                            stream_ctx->receive_buffer.data(),
                            stream_ctx->receive_buffer.size());
                        if (hop != kQualcommHopReflected) {
                            return QUIC_STATUS_INVALID_PARAMETER;
                        }
                        const uint64_t app_entry_rt = RealtimeNs();
                        const uint64_t app_entry_mono = NowNs();
                        std::vector<uint8_t> final_frame(
                            stream_ctx->receive_buffer.begin(),
                            stream_ctx->receive_buffer.begin() + static_cast<std::ptrdiff_t>(actual_frame_size));
                        stream_ctx->pending_app_entry_realtime_ns = app_entry_rt;
                        stream_ctx->pending_app_entry_mono_ns = app_entry_mono;
                        const auto latency_ns = NowNs() - header.send_timestamp_ns;
                        stats_.AddReceived(actual_frame_size);
                        stats_.AddLatencyNs(latency_ns);
                        stream_metrics_[stream_ctx->ordinal].AddReceived(actual_frame_size, latency_ns);
                        ++stream_ctx->echoed_messages;
                        ++stream_ctx->qualcomm_reflected_messages;
                        stream_ctx->receive_buffer.erase(
                            stream_ctx->receive_buffer.begin(),
                            stream_ctx->receive_buffer.begin() + static_cast<std::ptrdiff_t>(actual_frame_size));
                        if (!SendQualcommFinal(connection, *stream_ctx, std::move(final_frame))) {
                            return QUIC_STATUS_INTERNAL_ERROR;
                        }
                        break;
                    }
```

Keep the existing non-Qualcomm receive path unchanged.

- [ ] **Step 7: Count Qualcomm final send completions**

In `QUIC_STREAM_EVENT_SEND_COMPLETE`, before deleting the send buffer, inspect the hop marker:

```cpp
            if (config_.qualcomm_method && event->SEND_COMPLETE.ClientContext != nullptr) {
                const auto* completed = static_cast<SendBuffer*>(event->SEND_COMPLETE.ClientContext);
                if (completed->storage.size() > kQualcommHopOffset &&
                    completed->storage[kQualcommHopOffset] == kQualcommHopFinal) {
                    std::lock_guard<std::mutex> lock(stream_ctx->mutex);
                    ++stream_ctx->qualcomm_final_send_completions;
                }
            }
```

Then keep:

```cpp
            delete static_cast<SendBuffer*>(event->SEND_COMPLETE.ClientContext);
```

- [ ] **Step 8: Pump next initial after final completion**

After incrementing `qualcomm_final_send_completions`, call:

```cpp
                    std::lock_guard<std::mutex> connection_lock(connection.mutex);
                    PumpSends(connection);
```

If `PumpSends(connection)` is called while holding `stream_ctx->mutex`, release the stream lock first to avoid lock inversion. The accepted order is connection mutex first, then stream mutex inside `PumpSends`.

- [ ] **Step 9: Run build**

```bash
cmake --build build-multistream -j
```

Expected:
- Build succeeds.
- Qualcomm MSQuic mode sends exactly `--message-count` initial requests, receives exactly that many reflected responses, sends exactly that many final packets, and shuts down gracefully after final send completions.

- [ ] **Step 10: Commit**

```bash
git add src/main.cpp
git commit -m "feat: add Qualcomm MSQuic client ping-pong"
```

---

### Task 7: Add Packet Capture Export Script

**Files:**
- Create: `benchmarks/qualcomm-method/export-pcap-csv.sh`

- [ ] **Step 1: Add script**

Create `benchmarks/qualcomm-method/export-pcap-csv.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 2 ]]; then
  echo "usage: $0 INPUT.pcap OUTPUT.csv" >&2
  exit 2
fi

input_pcap="$1"
output_csv="$2"

if ! command -v tshark >/dev/null 2>&1; then
  echo "tshark not found in PATH" >&2
  exit 127
fi

tshark -r "${input_pcap}" -T fields \
  -E header=y \
  -E separator=, \
  -E quote=d \
  -e frame.number \
  -e frame.time_epoch \
  -e frame.len \
  -e ip.src \
  -e ip.dst \
  -e ipv6.src \
  -e ipv6.dst \
  -e _ws.col.Protocol \
  -e tcp.srcport \
  -e tcp.dstport \
  -e udp.srcport \
  -e udp.dstport \
  -e sctp.srcport \
  -e sctp.dstport \
  > "${output_csv}"
```

- [ ] **Step 2: Make script executable**

```bash
chmod +x benchmarks/qualcomm-method/export-pcap-csv.sh
```

- [ ] **Step 3: Commit**

```bash
git add benchmarks/qualcomm-method/export-pcap-csv.sh
git commit -m "feat: add Qualcomm pcap CSV export"
```

---

### Task 8: Add Stack-Time Post-Processor

**Files:**
- Create: `benchmarks/qualcomm-method/compute-stack-times.py`
- Create: `benchmarks/qualcomm-method/test-compute-stack-times.py`

- [ ] **Step 1: Add post-processor**

Create `benchmarks/qualcomm-method/compute-stack-times.py`:

```python
#!/usr/bin/env python3
import argparse
import csv
import math
from dataclasses import dataclass
from statistics import mean, stdev


@dataclass(frozen=True)
class TraceRow:
    event: str
    role: str
    protocol: str
    sequence: int
    message_size: int
    hop_in: int
    hop_out: int
    app_entry_ns: int
    app_exit_ns: int


@dataclass(frozen=True)
class PacketRow:
    frame_number: int
    time_ns: int
    frame_len: int
    src: str
    dst: str
    protocol: str
    srcport: str
    dstport: str


def parse_epoch_ns(value: str) -> int:
    whole, dot, frac = value.partition(".")
    frac = (frac + "000000000")[:9] if dot else "000000000"
    return int(whole) * 1_000_000_000 + int(frac)


def read_trace(path: str, role: str, event: str) -> list[TraceRow]:
    rows: list[TraceRow] = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if row["role"] != role or row["event"] != event:
                continue
            rows.append(TraceRow(
                event=row["event"],
                role=row["role"],
                protocol=row["protocol"],
                sequence=int(row["sequence"]),
                message_size=int(row["message_size"]),
                hop_in=int(row["hop_in"]),
                hop_out=int(row["hop_out"]),
                app_entry_ns=int(row["app_entry_realtime_ns"]),
                app_exit_ns=int(row["app_exit_realtime_ns"]),
            ))
    return rows


def read_packets(path: str, local_ip: str, peer_ip: str, port: str) -> tuple[list[PacketRow], list[PacketRow]]:
    inbound: list[PacketRow] = []
    outbound: list[PacketRow] = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            src = row.get("ip.src") or row.get("ipv6.src") or ""
            dst = row.get("ip.dst") or row.get("ipv6.dst") or ""
            srcport = row.get("tcp.srcport") or row.get("udp.srcport") or row.get("sctp.srcport") or ""
            dstport = row.get("tcp.dstport") or row.get("udp.dstport") or row.get("sctp.dstport") or ""
            if port not in (srcport, dstport):
                continue
            packet = PacketRow(
                frame_number=int(row["frame.number"]),
                time_ns=parse_epoch_ns(row["frame.time_epoch"]),
                frame_len=int(row["frame.len"]),
                src=src,
                dst=dst,
                protocol=row.get("_ws.col.Protocol", ""),
                srcport=srcport,
                dstport=dstport,
            )
            if src == peer_ip and dst == local_ip:
                inbound.append(packet)
            elif src == local_ip and dst == peer_ip:
                outbound.append(packet)
    return inbound, outbound


def latest_before_or_at(packets: list[PacketRow], timestamp_ns: int, window_ns: int) -> PacketRow:
    candidates = [p for p in packets if timestamp_ns - window_ns <= p.time_ns <= timestamp_ns]
    if not candidates:
        raise ValueError(f"no inbound packet within {window_ns} ns before app entry {timestamp_ns}")
    return max(candidates, key=lambda p: p.time_ns)


def first_after_or_at(packets: list[PacketRow], timestamp_ns: int, window_ns: int) -> PacketRow:
    candidates = [p for p in packets if timestamp_ns <= p.time_ns <= timestamp_ns + window_ns]
    if not candidates:
        raise ValueError(f"no outbound packet within {window_ns} ns after app exit {timestamp_ns}")
    return min(candidates, key=lambda p: p.time_ns)


def compute_samples(
    traces: list[TraceRow],
    inbound: list[PacketRow],
    outbound: list[PacketRow],
    window_us: int,
) -> list[float]:
    window_ns = window_us * 1000
    samples_us: list[float] = []
    for trace in traces:
        t_ingress = latest_before_or_at(inbound, trace.app_entry_ns, window_ns)
        t_egress = first_after_or_at(outbound, trace.app_exit_ns, window_ns)
        host_turnaround_ns = t_egress.time_ns - t_ingress.time_ns
        app_processing_ns = trace.app_exit_ns - trace.app_entry_ns
        stack_ns = host_turnaround_ns - app_processing_ns
        samples_us.append(stack_ns / 1000.0)
    return samples_us


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return math.nan
    sorted_values = sorted(values)
    index = round((len(sorted_values) - 1) * pct)
    return sorted_values[index]


def print_summary(label: str, samples_us: list[float]) -> None:
    if not samples_us:
        print(f"{label}: no samples")
        return
    sd = stdev(samples_us) if len(samples_us) > 1 else 0.0
    print(
        f"{label}: count={len(samples_us)} "
        f"mean_us={mean(samples_us):.3f} sd_us={sd:.3f} "
        f"p50_us={percentile(samples_us, 0.50):.3f} "
        f"p95_us={percentile(samples_us, 0.95):.3f} "
        f"p99_us={percentile(samples_us, 0.99):.3f}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--amf-trace", required=True)
    parser.add_argument("--ran-trace", required=True)
    parser.add_argument("--amf-packets", required=True)
    parser.add_argument("--ran-packets", required=True)
    parser.add_argument("--amf-ip", required=True)
    parser.add_argument("--ran-ip", required=True)
    parser.add_argument("--port", default="15443")
    parser.add_argument("--pair-window-us", type=int, default=5000)
    args = parser.parse_args()

    amf_traces = read_trace(args.amf_trace, role="amf", event="turnaround")
    ran_traces = read_trace(args.ran_trace, role="ran", event="turnaround")
    amf_in, amf_out = read_packets(args.amf_packets, args.amf_ip, args.ran_ip, args.port)
    ran_in, ran_out = read_packets(args.ran_packets, args.ran_ip, args.amf_ip, args.port)

    amf_samples = compute_samples(amf_traces, amf_in, amf_out, args.pair_window_us)
    ran_samples = compute_samples(ran_traces, ran_in, ran_out, args.pair_window_us)
    print_summary("AMF/NF1", amf_samples)
    print_summary("RAN/NF2", ran_samples)
    print_summary("average", amf_samples + ran_samples)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2: Add synthetic test**

Create `benchmarks/qualcomm-method/test-compute-stack-times.py`:

```python
#!/usr/bin/env python3
import importlib.util
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("compute-stack-times.py")
spec = importlib.util.spec_from_file_location("compute_stack_times", MODULE_PATH)
mod = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(mod)


def test_compute_samples_subtracts_app_processing():
    trace = mod.TraceRow(
        event="turnaround",
        role="amf",
        protocol="sctp",
        sequence=7,
        message_size=50,
        hop_in=0,
        hop_out=1,
        app_entry_ns=1_000_100_000,
        app_exit_ns=1_000_130_000,
    )
    inbound = [
        mod.PacketRow(1, 1_000_090_000, 98, "10.0.0.2", "10.0.0.1", "SCTP", "50000", "15443"),
    ]
    outbound = [
        mod.PacketRow(2, 1_000_170_000, 98, "10.0.0.1", "10.0.0.2", "SCTP", "15443", "50000"),
    ]
    samples = mod.compute_samples([trace], inbound, outbound, window_us=5000)
    assert samples == [50.0]


def test_parse_epoch_ns_preserves_nanoseconds():
    assert mod.parse_epoch_ns("1710000000.123456789") == 1_710_000_000_123_456_789
    assert mod.parse_epoch_ns("1710000000.1") == 1_710_000_000_100_000_000
```

- [ ] **Step 3: Run test**

```bash
python3 benchmarks/qualcomm-method/test-compute-stack-times.py
```

Expected:
- Exit code `0`.

- [ ] **Step 4: Make scripts executable**

```bash
chmod +x benchmarks/qualcomm-method/compute-stack-times.py benchmarks/qualcomm-method/test-compute-stack-times.py
```

- [ ] **Step 5: Commit**

```bash
git add benchmarks/qualcomm-method/compute-stack-times.py benchmarks/qualcomm-method/test-compute-stack-times.py
git commit -m "feat: compute Qualcomm stack timing"
```

---

### Task 9: Add Qualcomm Method Documentation

**Files:**
- Create: `benchmarks/qualcomm-method/README.md`
- Modify: `README.md`

- [ ] **Step 1: Add benchmark README**

Create `benchmarks/qualcomm-method/README.md`:

```markdown
# Qualcomm-Method Stack Timing Benchmark

This benchmark mode aligns this project with the measurement style described in
3GPP R3-261501:

```text
stack_processing = (packet_egress - packet_ingress) - (app_exit - app_entry)
```

It is intentionally separate from the N2 scaling benchmark. It measures a
single client and a single server with one stream and one in-flight message.

## What This Measures

- AMF/NF1 stack turnaround:
  `(T3 - T2) - (AMF_app_exit - AMF_app_entry)`
- RAN/NF2 stack turnaround:
  `(T5 - T4) - (RAN_app_exit - RAN_app_entry)`

The app writes realtime nanosecond timestamps to CSV. Packet timestamps come
from pcap files captured on the host NICs.

## What This Does Not Bypass

`--qualcomm-method=1` bypasses this load generator's pacing thread, PPS
scheduler, duration-based send loop, and periodic stats printer. It does not
bypass Linux scheduling, NIC queues, interrupt moderation, SCTP internals,
QUIC internals, or encryption costs.

## Recommended Host Setup

Use two physical Linux hosts on the same Ethernet LAN:

- RAN host: runs the client
- AMF host: runs the server

Use the same kernel family and CPU governor on both hosts. For cleaner data,
pin the process and capture tool to isolated CPUs if available.

## AMF Host

Start capture:

```bash
sudo tcpdump -i eth0 -w amf.pcap 'port 15443'
```

Run SCTP server:

```bash
./msquic-loadtest server \
  --qualcomm-method=1 \
  --protocol=sctp \
  --bind=0.0.0.0 \
  --base-port=15443 \
  --server-count=1 \
  --stream-count=1 \
  --message-size=50 \
  --trace-file=amf-sctp-trace.csv
```

Run MSQuic server:

```bash
./msquic-loadtest server \
  --qualcomm-method=1 \
  --protocol=msquic \
  --cert=certs/server.crt \
  --key=certs/server.key \
  --bind=0.0.0.0 \
  --base-port=15443 \
  --server-count=1 \
  --stream-count=1 \
  --message-size=50 \
  --trace-file=amf-msquic-trace.csv
```

## RAN Host

Start capture:

```bash
sudo tcpdump -i eth0 -w ran.pcap 'port 15443'
```

Run SCTP client:

```bash
./msquic-loadtest client \
  --qualcomm-method=1 \
  --protocol=sctp \
  --target=AMF_HOST_IP \
  --base-port=15443 \
  --server-count=1 \
  --clients=1 \
  --stream-count=1 \
  --message-size=50 \
  --max-inflight=1 \
  --message-count=30000 \
  --trace-file=ran-sctp-trace.csv
```

Run MSQuic client:

```bash
./msquic-loadtest client \
  --qualcomm-method=1 \
  --protocol=msquic \
  --target=AMF_HOST_IP \
  --base-port=15443 \
  --server-count=1 \
  --clients=1 \
  --stream-count=1 \
  --message-size=50 \
  --max-inflight=1 \
  --message-count=30000 \
  --trace-file=ran-msquic-trace.csv
```

## Export Packet CSV

```bash
./benchmarks/qualcomm-method/export-pcap-csv.sh amf.pcap amf-packets.csv
./benchmarks/qualcomm-method/export-pcap-csv.sh ran.pcap ran-packets.csv
```

## Compute Stack Times

```bash
./benchmarks/qualcomm-method/compute-stack-times.py \
  --amf-trace amf-sctp-trace.csv \
  --ran-trace ran-sctp-trace.csv \
  --amf-packets amf-packets.csv \
  --ran-packets ran-packets.csv \
  --amf-ip AMF_HOST_IP \
  --ran-ip RAN_HOST_IP \
  --port 15443
```

The output reports `mean_us`, `sd_us`, `p50_us`, `p95_us`, and `p99_us` for
AMF/NF1, RAN/NF2, and the combined average.

## Interpretation Rules

- Compare SCTP and MSQuic only after both complete all 30,000 messages.
- Report implementation names: Linux SCTP and MSQuic.
- Keep this result separate from the N2 scaling benchmark. This benchmark
  isolates per-host stack turnaround under a single-flow condition.
- For QUIC, packet payload is encrypted. The post-processor pairs packet
  timestamps around application entry and exit times; use one in-flight message
  to avoid ambiguous pairing.
```

- [ ] **Step 2: Add root README pointer**

Add this section near the benchmark helpers in `README.md`:

```markdown
### Qualcomm-Method Stack Timing

For a single-client/single-server measurement aligned with R3-261501, use
`benchmarks/qualcomm-method/README.md`. That mode uses `--qualcomm-method=1`,
`--message-size=50`, `--message-count=30000`, and packet-capture post-processing
to compute per-host stack processing time.
```

- [ ] **Step 3: Commit**

```bash
git add benchmarks/qualcomm-method/README.md README.md
git commit -m "docs: document Qualcomm method benchmark"
```

---

### Task 10: Add Runtime Verification Scripts

**Files:**
- Create: `benchmarks/qualcomm-method/smoke-local.sh`

- [ ] **Step 1: Add smoke script**

Create `benchmarks/qualcomm-method/smoke-local.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

BIN="${BIN:-./build-multistream/msquic-loadtest}"
BASE_PORT="${BASE_PORT:-18443}"
OUT_DIR="${OUT_DIR:-/tmp/msquic-loadtest-qualcomm-smoke}"

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

"${BIN}" server \
  --qualcomm-method=1 \
  --protocol=sctp \
  --bind=127.0.0.1 \
  --base-port="${BASE_PORT}" \
  --server-count=1 \
  --stream-count=1 \
  --message-size=50 \
  --trace-file="${OUT_DIR}/amf-trace.csv" \
  >"${OUT_DIR}/server.log" 2>&1 &

server_pid="$!"
cleanup() {
  kill "${server_pid}" >/dev/null 2>&1 || true
  wait "${server_pid}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

sleep 1

"${BIN}" client \
  --qualcomm-method=1 \
  --protocol=sctp \
  --target=127.0.0.1 \
  --base-port="${BASE_PORT}" \
  --server-count=1 \
  --clients=1 \
  --stream-count=1 \
  --message-size=50 \
  --max-inflight=1 \
  --message-count=10 \
  --trace-file="${OUT_DIR}/ran-trace.csv" \
  >"${OUT_DIR}/client.log" 2>&1

amf_turnarounds="$(awk -F, '$1 == "turnaround" { count++ } END { print count + 0 }' "${OUT_DIR}/amf-trace.csv")"
ran_turnarounds="$(awk -F, '$1 == "turnaround" { count++ } END { print count + 0 }' "${OUT_DIR}/ran-trace.csv")"
amf_finals="$(awk -F, '$1 == "final_receive" { count++ } END { print count + 0 }' "${OUT_DIR}/amf-trace.csv")"

if [[ "${amf_turnarounds}" != "10" ]]; then
  echo "expected 10 AMF turnarounds, got ${amf_turnarounds}" >&2
  exit 1
fi
if [[ "${ran_turnarounds}" != "10" ]]; then
  echo "expected 10 RAN turnarounds, got ${ran_turnarounds}" >&2
  exit 1
fi
if [[ "${amf_finals}" != "10" ]]; then
  echo "expected 10 AMF final receives, got ${amf_finals}" >&2
  exit 1
fi

echo "smoke passed: ${OUT_DIR}"
```

- [ ] **Step 2: Make executable**

```bash
chmod +x benchmarks/qualcomm-method/smoke-local.sh
```

- [ ] **Step 3: Run smoke test**

Run:

```bash
benchmarks/qualcomm-method/smoke-local.sh
```

Expected:
- `smoke passed: /tmp/msquic-loadtest-qualcomm-smoke`

If local `libmsquic.so.2` is unavailable, run the equivalent smoke in the Docker image after rebuilding the image:

```bash
./scripts/docker-build-image.sh
```

Then run a container-based smoke command that starts server and client containers on `--network host`.

- [ ] **Step 4: Commit**

```bash
git add benchmarks/qualcomm-method/smoke-local.sh
git commit -m "test: add Qualcomm mode smoke test"
```

---

### Task 11: End-To-End Verification

**Files:**
- No new files.

- [ ] **Step 1: Build locally or build Docker image**

Preferred local command:

```bash
cmake --build build-multistream -j
```

Docker command:

```bash
./scripts/docker-build-image.sh
```

Expected:
- Binary builds successfully.

- [ ] **Step 2: Verify parser rejects scheduler flags**

Run:

```bash
./build-multistream/msquic-loadtest client \
  --qualcomm-method=1 \
  --protocol=sctp \
  --target=127.0.0.1 \
  --clients=1 \
  --server-count=1 \
  --stream-count=1 \
  --message-size=50 \
  --max-inflight=1 \
  --send-pps=10 \
  --trace-file=/tmp/ran.csv
```

Expected:

```text
--qualcomm-method=1 bypasses pacing; do not set --send-pps or --send-pps-per-client
```

- [ ] **Step 3: Verify parser rejects multiple clients**

Run:

```bash
./build-multistream/msquic-loadtest client \
  --qualcomm-method=1 \
  --protocol=sctp \
  --target=127.0.0.1 \
  --clients=2 \
  --server-count=1 \
  --stream-count=1 \
  --message-size=50 \
  --max-inflight=1 \
  --trace-file=/tmp/ran.csv
```

Expected:

```text
--qualcomm-method=1 requires --clients=1
```

- [ ] **Step 4: Run post-processor unit test**

```bash
python3 benchmarks/qualcomm-method/test-compute-stack-times.py
```

Expected:
- Exit code `0`.

- [ ] **Step 5: Run smoke test**

```bash
benchmarks/qualcomm-method/smoke-local.sh
```

Expected:
- Exit code `0`.
- Trace files contain 10 AMF turnarounds, 10 RAN turnarounds, and 10 AMF final receives.

- [ ] **Step 6: Run two-host 30,000 message test**

Run the commands documented in `benchmarks/qualcomm-method/README.md` on two hosts.

Expected:
- Client summary shows `sent_messages` at least `60000` in Qualcomm mode for SCTP/common transport because it counts 30,000 initial sends plus 30,000 final sends.
- Client `echoed_messages` equals `30000`.
- AMF trace has 30,000 `turnaround` records and 30,000 `final_receive` records.
- RAN trace has 30,000 `turnaround` records.
- Post-processor prints AMF/NF1, RAN/NF2, and average stack-time summaries.

- [ ] **Step 7: Commit verification docs if adjusted**

If verification discovers command corrections, update `benchmarks/qualcomm-method/README.md` and commit:

```bash
git add benchmarks/qualcomm-method/README.md
git commit -m "docs: refine Qualcomm verification steps"
```

---

## Self-Review

**Spec coverage**
- Single client and single server: Task 1 validation requires one server, one client, one stream.
- Qualcomm message count: Task 1 adds `--message-count`, Task 4 and Task 6 enforce message-count completion.
- Qualcomm packet flow: Task 4 and Task 6 add RAN re-echo final hop; Task 3 and Task 5 add AMF final receive handling.
- Measurement alignment: Task 2 adds realtime trace fields, Task 8 computes `(Tegress - Tingress) - (Ty - Tx)`.
- Scheduling bypass: Task 1 rejects PPS scheduler flags; Task 4 and Task 6 bypass pacer and duration-driven send loops; Task 3 and Task 5 bypass periodic server stats in Qualcomm mode.
- Two-host workflow: Task 9 documents host commands, capture, export, and compute steps.

**Placeholder scan**
- This plan contains no unresolved implementation slots and no deferred test step.

**Type consistency**
- `AppConfig::qualcomm_method`, `AppConfig::message_count`, and `AppConfig::trace_file` are introduced in Task 1 and used consistently later.
- Hop constants are introduced in Task 2 and used consistently as `kQualcommHopInitial`, `kQualcommHopReflected`, and `kQualcommHopFinal`.
- Trace records use realtime nanoseconds for pcap alignment and monotonic nanoseconds for debugging.

**Known design risks**
- QUIC payload is encrypted, so pcap cannot directly expose the app sequence. The post-processor intentionally pairs nearest packets around app entry/exit timestamps and requires `max_inflight=1`.
- `clock_gettime(CLOCK_REALTIME)` and pcap timestamps must use the same host clock. Per-host stack calculations do not require cross-host synchronization.
- Existing `src/main.cpp` is large. This plan keeps changes localized rather than splitting the file, because a structural refactor would add risk unrelated to the measurement mode.
