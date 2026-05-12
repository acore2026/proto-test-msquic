#!/usr/bin/env python3
import subprocess
import time
import re
import matplotlib.pyplot as plt
import os
import signal
import psutil
import socket
import ssl
import threading
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
NUM_TESTS = 10
DATA_SIZE = 64 * 1024
TEST_DURATION = 3

SCTP_PORT = 4434
SCTP_TLS_PORT = 4435
QUIC_PORT = 4433

def generate_cert():
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    import datetime
    
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = issuer = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    cert = x509.CertificateBuilder().subject_name(subject).issuer_name(issuer).public_key(
        key.public_key()
    ).serial_number(x509.random_serial_number()).not_valid_before(
        datetime.datetime.utcnow()
    ).not_valid_after(
        datetime.datetime.utcnow() + datetime.timedelta(days=365)
    ).sign(key, hashes.SHA256())
    
    cert_path = "/tmp/sctp_cert.pem"
    key_path = "/tmp/sctp_key.pem"
    
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    with open(key_path, "wb") as f:
        f.write(key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.TraditionalOpenSSL,
            encryption_algorithm=serialization.NoEncryption()
        ))
    
    return cert_path, key_path

CERT_PATH, KEY_PATH = generate_cert()

def monitor_process(pid, duration):
    cpu_samples = []
    mem_samples = []
    try:
        p = psutil.Process(pid)
        start_time = time.time()
        while time.time() - start_time < duration:
            try:
                cpu = p.cpu_percent(interval=0.1)
                mem = p.memory_info().rss / 1024 / 1024
                cpu_samples.append(cpu)
                mem_samples.append(mem)
            except psutil.NoSuchProcess:
                break
    except psutil.NoSuchProcess:
        pass
    
    avg_cpu = sum(cpu_samples) / len(cpu_samples) if cpu_samples else 0
    avg_mem = sum(mem_samples) / len(mem_samples) if mem_samples else 0
    peak_cpu = max(cpu_samples) if cpu_samples else 0
    peak_mem = max(mem_samples) if mem_samples else 0
    
    return avg_cpu, avg_mem, peak_cpu, peak_mem

def run_sctp_plain_server():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_SCTP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, DATA_SIZE * 256)
    sock.bind(('0.0.0.0', SCTP_PORT))
    sock.listen(5)
    return sock

def run_sctp_tls_server():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_SCTP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, DATA_SIZE * 256)
    sock.bind(('0.0.0.0', SCTP_TLS_PORT))
    sock.listen(5)
    
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(CERT_PATH, KEY_PATH)
    
    return sock, context

def test_sctp_connection_plain():
    server_sock = run_sctp_plain_server()
    server_sock.settimeout(10)
    
    server_thread_ready = threading.Event()
    connection_time = 0
    server_cpu_data = {"avg": 0, "peak": 0}
    
    def server_handler():
        server_thread_ready.set()
        start_accept = time.time()
        try:
            conn, addr = server_sock.accept()
            connection_time = time.time() - start_accept
            
            p = psutil.Process(os.getpid())
            cpu_samples = []
            
            total_received = 0
            recv_start = time.time()
            
            while time.time() - recv_start < TEST_DURATION:
                data = conn.recv(DATA_SIZE)
                if not data:
                    break
                total_received += len(data)
                cpu_samples.append(p.cpu_percent(interval=0.05))
            
            conn.close()
            server_cpu_data["avg"] = sum(cpu_samples) / len(cpu_samples) if cpu_samples else 0
            server_cpu_data["peak"] = max(cpu_samples) if cpu_samples else 0
            server_cpu_data["connection_time"] = connection_time
            server_cpu_data["total_received"] = total_received
            
        except Exception as e:
            server_cpu_data["error"] = str(e)
        finally:
            server_sock.close()
    
    server_thread = threading.Thread(target=server_handler)
    server_thread.start()
    
    server_thread_ready.wait()
    time.sleep(0.1)
    
    client_start = time.time()
    client_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_SCTP)
    client_sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, DATA_SIZE * 256)
    client_sock.connect(('127.0.0.1', SCTP_PORT))
    client_connection_time = time.time() - client_start
    
    client_cpu_samples = []
    p = psutil.Process(os.getpid())
    data = b'\x00' * DATA_SIZE
    total_sent = 0
    
    send_start = time.time()
    while time.time() - send_start < TEST_DURATION:
        try:
            sent = client_sock.send(data)
            total_sent += sent
            client_cpu_samples.append(p.cpu_percent(interval=0.05))
        except Exception:
            break
    
    client_sock.close()
    
    server_thread.join(timeout=5)
    
    client_avg_cpu = sum(client_cpu_samples) / len(client_cpu_samples) if client_cpu_samples else 0
    client_peak_cpu = max(client_cpu_samples) if client_cpu_samples else 0
    
    return {
        "connection_time": (server_cpu_data.get("connection_time", 0) + client_connection_time) / 2,
        "client_avg_cpu": client_avg_cpu,
        "client_peak_cpu": client_peak_cpu,
        "server_avg_cpu": server_cpu_data.get("avg", 0),
        "server_peak_cpu": server_cpu_data.get("peak", 0),
        "total_data": server_cpu_data.get("total_received", 0)
    }

def test_sctp_connection_tls():
    server_sock, context = run_sctp_tls_server()
    server_sock.settimeout(10)
    
    server_thread_ready = threading.Event()
    
    def server_handler():
        server_thread_ready.set()
        start_accept = time.time()
        try:
            conn, addr = server_sock.accept()
            ssl_conn = context.wrap_socket(conn, server_side=True)
            connection_time = time.time() - start_accept
            
            p = psutil.Process(os.getpid())
            cpu_samples = []
            
            total_received = 0
            recv_start = time.time()
            
            while time.time() - recv_start < TEST_DURATION:
                data = ssl_conn.recv(DATA_SIZE)
                if not data:
                    break
                total_received += len(data)
                cpu_samples.append(p.cpu_percent(interval=0.05))
            
            ssl_conn.close()
            server_sock.close()
            
            server_cpu_data = {
                "avg": sum(cpu_samples) / len(cpu_samples) if cpu_samples else 0,
                "peak": max(cpu_samples) if cpu_samples else 0,
                "connection_time": connection_time,
                "total_received": total_received
            }
            
            global sctp_tls_result
            sctp_tls_result = server_cpu_data
            
        except Exception as e:
            global sctp_tls_result
            sctp_tls_result = {"error": str(e)}
    
    global sctp_tls_result
    sctp_tls_result = {}
    
    server_thread = threading.Thread(target=server_handler)
    server_thread.start()
    
    server_thread_ready.wait()
    time.sleep(0.1)
    
    client_start = time.time()
    client_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_SCTP)
    client_sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, DATA_SIZE * 256)
    client_sock.connect(('127.0.0.1', SCTP_TLS_PORT))
    
    client_context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    client_context.check_hostname = False
    client_context.verify_mode = ssl.CERT_NONE
    
    ssl_client_sock = client_context.wrap_socket(client_sock, server_hostname="localhost")
    client_connection_time = time.time() - client_start
    
    client_cpu_samples = []
    p = psutil.Process(os.getpid())
    data = b'\x00' * DATA_SIZE
    total_sent = 0
    
    send_start = time.time()
    while time.time() - send_start < TEST_DURATION:
        try:
            sent = ssl_client_sock.send(data)
            total_sent += sent
            client_cpu_samples.append(p.cpu_percent(interval=0.05))
        except Exception:
            break
    
    ssl_client_sock.close()
    
    server_thread.join(timeout=5)
    
    client_avg_cpu = sum(client_cpu_samples) / len(client_cpu_samples) if client_cpu_samples else 0
    client_peak_cpu = max(client_cpu_samples) if client_cpu_samples else 0
    
    return {
        "connection_time": (sctp_tls_result.get("connection_time", 0) + client_connection_time) / 2,
        "client_avg_cpu": client_avg_cpu,
        "client_peak_cpu": client_peak_cpu,
        "server_avg_cpu": sctp_tls_result.get("avg", 0),
        "server_peak_cpu": sctp_tls_result.get("peak", 0),
        "total_data": sctp_tls_result.get("total_received", 0)
    }

def test_quic_connection():
    proc = subprocess.Popen(
        ["python3", "-u", f"{SCRIPT_DIR}/quic_bench.py", "server", "--duration", str(TEST_DURATION)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        preexec_fn=os.setsid
    )
    
    server_pid = proc.pid
    time.sleep(1)
    
    monitor_thread = threading.Thread(target=lambda: monitor_process(server_pid, TEST_DURATION + 3))
    monitor_results = {"avg_cpu": 0, "peak_cpu": 0}
    
    def monitor_wrapper():
        avg_cpu, avg_mem, peak_cpu, peak_mem = monitor_process(server_pid, TEST_DURATION + 3)
        monitor_results["avg_cpu"] = avg_cpu
        monitor_results["peak_cpu"] = peak_cpu
        monitor_results["avg_mem"] = avg_mem
    
    monitor_thread = threading.Thread(target=monitor_wrapper)
    monitor_thread.start()
    
    client_start = time.time()
    
    import asyncio
    from aioquic.asyncio import connect
    from aioquic.quic.configuration import QuicConfiguration
    
    async def quic_client():
        configuration = QuicConfiguration(is_client=True)
        configuration.verify_mode = ssl.CERT_NONE
        
        connection_times = []
        client_cpu_samples = []
        p = psutil.Process(os.getpid())
        
        async with connect("127.0.0.1", QUIC_PORT, configuration=configuration) as client:
            conn_time = time.time() - client_start
            
            data = b'\x00' * DATA_SIZE
            send_start = time.time()
            
            while time.time() - send_start < TEST_DURATION:
                client._quic.send_stream_data(0, data)
                client_cpu_samples.append(p.cpu_percent(interval=0.05))
                await asyncio.sleep(0.002)
            
            client._quic.send_stream_data(0, b'', end_stream=True)
            await asyncio.sleep(1)
            
            return conn_time, sum(client_cpu_samples) / len(client_cpu_samples) if client_cpu_samples else 0, max(client_cpu_samples) if client_cpu_samples else 0
    
    client_connection_time, client_avg_cpu, client_peak_cpu = asyncio.run(quic_client())
    
    try:
        proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    
    monitor_thread.join(timeout=5)
    
    return {
        "connection_time": client_connection_time,
        "client_avg_cpu": client_avg_cpu,
        "client_peak_cpu": client_peak_cpu,
        "server_avg_cpu": monitor_results["avg_cpu"],
        "server_peak_cpu": monitor_results["peak_cpu"]
    }

def main():
    print("=" * 70)
    print("Testing SCTP vs QUIC: Connection Setup and Encryption Overhead")
    print(f"Tests per scenario: {NUM_TESTS}, Duration: {TEST_DURATION}s each")
    print("=" * 70)
    
    results = {
        "sctp_plain": {"connection": [], "client_cpu": [], "server_cpu": [], "client_peak": [], "server_peak": []},
        "sctp_tls": {"connection": [], "client_cpu": [], "server_cpu": [], "client_peak": [], "server_peak": []},
        "quic": {"connection": [], "client_cpu": [], "server_cpu": [], "client_peak": [], "server_peak": []}
    }
    
    for i in range(NUM_TESTS):
        print(f"\n--- Test {i+1}/{NUM_TESTS} ---")
        
        print("[SCTP Plain] Testing...")
        try:
            r = test_sctp_connection_plain()
            results["sctp_plain"]["connection"].append(r["connection_time"])
            results["sctp_plain"]["client_cpu"].append(r["client_avg_cpu"])
            results["sctp_plain"]["server_cpu"].append(r["server_avg_cpu"])
            results["sctp_plain"]["client_peak"].append(r["client_peak_cpu"])
            results["sctp_plain"]["server_peak"].append(r["server_peak_cpu"])
            print(f"  Connection: {r['connection_time']*1000:.2f}ms, CPU: client={r['client_avg_cpu']:.1f}%, server={r['server_avg_cpu']:.1f}%")
        except Exception as e:
            print(f"  Error: {e}")
        
        time.sleep(1)
        
        print("[SCTP+TLS] Testing...")
        try:
            r = test_sctp_connection_tls()
            results["sctp_tls"]["connection"].append(r["connection_time"])
            results["sctp_tls"]["client_cpu"].append(r["client_avg_cpu"])
            results["sctp_tls"]["server_cpu"].append(r["server_avg_cpu"])
            results["sctp_tls"]["client_peak"].append(r["client_peak_cpu"])
            results["sctp_tls"]["server_peak"].append(r["server_peak_cpu"])
            print(f"  Connection: {r['connection_time']*1000:.2f}ms, CPU: client={r['client_avg_cpu']:.1f}%, server={r['server_avg_cpu']:.1f}%")
        except Exception as e:
            print(f"  Error: {e}")
        
        time.sleep(1)
        
        print("[QUIC] Testing...")
        try:
            r = test_quic_connection()
            results["quic"]["connection"].append(r["connection_time"])
            results["quic"]["client_cpu"].append(r["client_avg_cpu"])
            results["quic"]["server_cpu"].append(r["server_avg_cpu"])
            results["quic"]["client_peak"].append(r["client_peak_cpu"])
            results["quic"]["server_peak"].append(r["server_peak_cpu"])
            print(f"  Connection: {r['connection_time']*1000:.2f}ms, CPU: client={r['client_avg_cpu']:.1f}%, server={r['server_avg_cpu']:.1f}%")
        except Exception as e:
            print(f"  Error: {e}")
        
        time.sleep(1)
    
    print("\n" + "=" * 70)
    print("Calculating averages...")
    print("=" * 70)
    
    avg_results = {}
    for proto in ["sctp_plain", "sctp_tls", "quic"]:
        avg_results[proto] = {
            "connection_ms": sum(results[proto]["connection"]) / len(results[proto]["connection"]) * 1000 if results[proto]["connection"] else 0,
            "client_cpu": sum(results[proto]["client_cpu"]) / len(results[proto]["client_cpu"]) if results[proto]["client_cpu"] else 0,
            "server_cpu": sum(results[proto]["server_cpu"]) / len(results[proto]["server_cpu"]) if results[proto]["server_cpu"] else 0,
            "client_peak": sum(results[proto]["client_peak"]) / len(results[proto]["client_peak"]) if results[proto]["client_peak"] else 0,
            "server_peak": sum(results[proto]["server_peak"]) / len(results[proto]["server_peak"]) if results[proto]["server_peak"] else 0
        }
    
    print("\n" + "=" * 70)
    print("FINAL SUMMARY (Average)")
    print("=" * 70)
    print(f"{'Protocol':<15} {'Connection(ms)':>15} {'Client CPU%':>12} {'Server CPU%':>12} {'Client Peak':>12} {'Server Peak':>12}")
    print("-" * 85)
    for proto, label in [("sctp_plain", "SCTP Plain"), ("sctp_tls", "SCTP+TLS"), ("quic", "QUIC")]:
        r = avg_results[proto]
        print(f"{label:<15} {r['connection_ms']:>15.2f} {r['client_cpu']:>12.1f} {r['server_cpu']:>12.1f} {r['client_peak']:>12.1f} {r['server_peak']:>12.1f}")
    
    print("\n" + "=" * 70)
    print("TLS Overhead Analysis")
    print("=" * 70)
    if results["sctp_plain"]["connection"] and results["sctp_tls"]["connection"]:
        tls_conn_overhead = avg_results["sctp_tls"]["connection_ms"] - avg_results["sctp_plain"]["connection_ms"]
        tls_client_cpu_overhead = avg_results["sctp_tls"]["client_cpu"] - avg_results["sctp_plain"]["client_cpu"]
        tls_server_cpu_overhead = avg_results["sctp_tls"]["server_cpu"] - avg_results["sctp_plain"]["server_cpu"]
        print(f"SCTP TLS Connection Overhead: {tls_conn_overhead:.2f}ms")
        print(f"SCTP TLS Client CPU Overhead: {tls_client_cpu_overhead:.1f}%")
        print(f"SCTP TLS Server CPU Overhead: {tls_server_cpu_overhead:.1f}%")
    
    print("\n" + "=" * 70)
    print("QUIC vs SCTP+TLS Comparison")
    print("=" * 70)
    if results["quic"]["connection"] and results["sctp_tls"]["connection"]:
        quic_conn_diff = avg_results["quic"]["connection_ms"] - avg_results["sctp_tls"]["connection_ms"]
        quic_cpu_diff = avg_results["quic"]["client_cpu"] - avg_results["sctp_tls"]["client_cpu"]
        print(f"QUIC Connection vs SCTP+TLS: {quic_conn_diff:.2f}ms")
        print(f"QUIC Client CPU vs SCTP+TLS: {quic_cpu_diff:.1f}%")
    
    print("\n" + "=" * 70)
    print("Generating charts...")
    print("=" * 70)
    
    tests = list(range(1, NUM_TESTS + 1))
    
    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    
    ax1 = axes[0, 0]
    ax1.plot(tests, [x*1000 for x in results["sctp_plain"]["connection"]], 'b-o', label='SCTP Plain', linewidth=2)
    ax1.plot(tests, [x*1000 for x in results["sctp_tls"]["connection"]], 'g-s', label='SCTP+TLS', linewidth=2)
    ax1.plot(tests, [x*1000 for x in results["quic"]["connection"]], 'r-^', label='QUIC', linewidth=2)
    ax1.axhline(y=avg_results["sctp_plain"]["connection_ms"], color='b', linestyle='--', alpha=0.5)
    ax1.axhline(y=avg_results["sctp_tls"]["connection_ms"], color='g', linestyle='--', alpha=0.5)
    ax1.axhline(y=avg_results["quic"]["connection_ms"], color='r', linestyle='--', alpha=0.5)
    ax1.set_xlabel('Test Number', fontsize=12)
    ax1.set_ylabel('Connection Time (ms)', fontsize=12)
    ax1.set_title('Connection Establishment Time', fontsize=14, fontweight='bold')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    
    ax2 = axes[0, 1]
    ax2.plot(tests, results["sctp_plain"]["client_cpu"], 'b-o', label='SCTP Plain', linewidth=2)
    ax2.plot(tests, results["sctp_tls"]["client_cpu"], 'g-s', label='SCTP+TLS', linewidth=2)
    ax2.plot(tests, results["quic"]["client_cpu"], 'r-^', label='QUIC', linewidth=2)
    ax2.set_xlabel('Test Number', fontsize=12)
    ax2.set_ylabel('CPU Usage (%)', fontsize=12)
    ax2.set_title('Client Average CPU Usage', fontsize=14, fontweight='bold')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    
    ax3 = axes[1, 0]
    ax3.plot(tests, results["sctp_plain"]["server_cpu"], 'b-o', label='SCTP Plain', linewidth=2)
    ax3.plot(tests, results["sctp_tls"]["server_cpu"], 'g-s', label='SCTP+TLS', linewidth=2)
    ax3.plot(tests, results["quic"]["server_cpu"], 'r-^', label='QUIC', linewidth=2)
    ax3.set_xlabel('Test Number', fontsize=12)
    ax3.set_ylabel('CPU Usage (%)', fontsize=12)
    ax3.set_title('Server Average CPU Usage', fontsize=14, fontweight='bold')
    ax3.legend()
    ax3.grid(True, alpha=0.3)
    
    ax4 = axes[1, 1]
    protocols = ['SCTP Plain', 'SCTP+TLS', 'QUIC']
    x_bar = range(len(protocols))
    width = 0.35
    ax4.bar([i - width/2 for i in x_bar], [avg_results["sctp_plain"]["connection_ms"], avg_results["sctp_tls"]["connection_ms"], avg_results["quic"]["connection_ms"]], width, label='Connection (ms)', color='steelblue')
    ax4.bar([i + width/2 for i in x_bar], [avg_results["sctp_plain"]["client_cpu"], avg_results["sctp_tls"]["client_cpu"], avg_results["quic"]["client_cpu"]], width, label='Client CPU (%)', color='indianred')
    ax4.set_xlabel('Protocol', fontsize=12)
    ax4.set_ylabel('Value', fontsize=12)
    ax4.set_title('Average Performance Comparison', fontsize=14, fontweight='bold')
    ax4.set_xticks(x_bar)
    ax4.set_xticklabels(protocols)
    ax4.legend()
    ax4.grid(True, alpha=0.3, axis='y')
    
    plt.suptitle('SCTP vs QUIC: Connection Setup and Encryption Overhead\n(10 tests average)', 
                 fontsize=16, fontweight='bold')
    plt.tight_layout()
    
    output_path = f"{SCRIPT_DIR}/results/connection_encryption_comparison.png"
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    print(f"\nChart saved to: {output_path}")
    
    with open(f"{SCRIPT_DIR}/results/connection_encryption_data.txt", "w") as f:
        f.write("Protocol,Connection_ms,Client_CPU_pct,Server_CPU_pct,Client_Peak_CPU,Server_Peak_CPU\n")
        for proto, label in [("sctp_plain", "SCTP_Plain"), ("sctp_tls", "SCTP_TLS"), ("quic", "QUIC")]:
            r = avg_results[proto]
            f.write(f"{label},{r['connection_ms']:.2f},{r['client_cpu']:.1f},{r['server_cpu']:.1f},{r['client_peak']:.1f},{r['server_peak']:.1f}\n")
    
    print(f"Data saved to: {SCRIPT_DIR}/results/connection_encryption_data.txt")

if __name__ == "__main__":
    main()