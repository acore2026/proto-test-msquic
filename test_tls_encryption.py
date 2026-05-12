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
import datetime

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

def measure_tls_handshake_server(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_SCTP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, DATA_SIZE * 256)
    sock.bind(('0.0.0.0', port))
    sock.listen(5)
    
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(CERT_PATH, KEY_PATH)
    
    sock.settimeout(10)
    
    results = {}
    
    try:
        accept_start = time.time()
        conn, addr = sock.accept()
        accept_time = time.time() - accept_start
        
        tls_start = time.time()
        ssl_conn = context.wrap_socket(conn, server_side=True)
        tls_time = time.time() - tls_start
        
        cpu_samples = []
        p = psutil.Process(os.getpid())
        
        recv_start = time.time()
        total_received = 0
        
        while time.time() - recv_start < TEST_DURATION:
            try:
                data = ssl_conn.recv(DATA_SIZE)
                if not data:
                    break
                total_received += len(data)
                cpu_samples.append(p.cpu_percent(interval=0.1))
            except Exception:
                break
        
        ssl_conn.close()
        
        results = {
            "accept_time": accept_time,
            "tls_handshake_time": tls_time,
            "avg_cpu": sum(cpu_samples) / len(cpu_samples) if cpu_samples else 0,
            "peak_cpu": max(cpu_samples) if cpu_samples else 0,
            "total_data": total_received
        }
        
    except Exception as e:
        results["error"] = str(e)
    finally:
        sock.close()
    
    return results

def test_sctp_plain_connection():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_SCTP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, DATA_SIZE * 256)
    sock.bind(('0.0.0.0', SCTP_PORT))
    sock.listen(5)
    sock.settimeout(10)
    
    server_results = {}
    
    def server_thread():
        try:
            accept_start = time.time()
            conn, addr = sock.accept()
            accept_time = time.time() - accept_start
            
            cpu_samples = []
            p = psutil.Process(os.getpid())
            
            recv_start = time.time()
            total_received = 0
            
            while time.time() - recv_start < TEST_DURATION:
                try:
                    data = conn.recv(DATA_SIZE)
                    if not data:
                        break
                    total_received += len(data)
                    cpu_samples.append(p.cpu_percent(interval=0.1))
                except Exception:
                    break
            
            conn.close()
            
            server_results["connection_time"] = accept_time
            server_results["avg_cpu"] = sum(cpu_samples) / len(cpu_samples) if cpu_samples else 0
            server_results["peak_cpu"] = max(cpu_samples) if cpu_samples else 0
            server_results["total_data"] = total_received
            
        except Exception as e:
            server_results["error"] = str(e)
        finally:
            sock.close()
    
    thread = threading.Thread(target=server_thread)
    thread.start()
    
    time.sleep(0.5)
    
    client_start = time.time()
    client_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_SCTP)
    client_sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, DATA_SIZE * 256)
    client_sock.connect(('127.0.0.1', SCTP_PORT))
    client_connection_time = time.time() - client_start
    
    cpu_samples = []
    p = psutil.Process(os.getpid())
    data = b'\x00' * DATA_SIZE
    total_sent = 0
    
    send_start = time.time()
    while time.time() - send_start < TEST_DURATION:
        try:
            sent = client_sock.send(data)
            total_sent += sent
            cpu_samples.append(p.cpu_percent(interval=0.1))
        except Exception:
            break
    
    client_sock.close()
    
    thread.join(timeout=5)
    
    return {
        "sctp_connection_ms": client_connection_time * 1000,
        "tls_handshake_ms": 0,
        "total_connection_ms": client_connection_time * 1000,
        "client_avg_cpu": sum(cpu_samples) / len(cpu_samples) if cpu_samples else 0,
        "client_peak_cpu": max(cpu_samples) if cpu_samples else 0,
        "server_avg_cpu": server_results.get("avg_cpu", 0),
        "server_peak_cpu": server_results.get("peak_cpu", 0)
    }

def test_sctp_tls_connection():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_SCTP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, DATA_SIZE * 256)
    sock.bind(('0.0.0.0', SCTP_TLS_PORT))
    sock.listen(5)
    sock.settimeout(10)
    
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(CERT_PATH, KEY_PATH)
    
    server_results = {}
    
    def server_thread():
        try:
            accept_start = time.time()
            conn, addr = sock.accept()
            accept_time = time.time() - accept_start
            
            tls_start = time.time()
            ssl_conn = context.wrap_socket(conn, server_side=True)
            tls_time = time.time() - tls_start
            
            cpu_samples = []
            p = psutil.Process(os.getpid())
            
            recv_start = time.time()
            total_received = 0
            
            while time.time() - recv_start < TEST_DURATION:
                try:
                    data = ssl_conn.recv(DATA_SIZE)
                    if not data:
                        break
                    total_received += len(data)
                    cpu_samples.append(p.cpu_percent(interval=0.1))
                except Exception:
                    break
            
            ssl_conn.close()
            
            server_results["sctp_connection_time"] = accept_time
            server_results["tls_handshake_time"] = tls_time
            server_results["avg_cpu"] = sum(cpu_samples) / len(cpu_samples) if cpu_samples else 0
            server_results["peak_cpu"] = max(cpu_samples) if cpu_samples else 0
            server_results["total_data"] = total_received
            
        except Exception as e:
            server_results["error"] = str(e)
        finally:
            sock.close()
    
    thread = threading.Thread(target=server_thread)
    thread.start()
    
    time.sleep(0.5)
    
    client_tcp_start = time.time()
    client_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_SCTP)
    client_sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, DATA_SIZE * 256)
    client_sock.connect(('127.0.0.1', SCTP_TLS_PORT))
    client_sctp_time = time.time() - client_tcp_start
    
    client_tls_start = time.time()
    client_context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    client_context.check_hostname = False
    client_context.verify_mode = ssl.CERT_NONE
    ssl_client_sock = client_context.wrap_socket(client_sock, server_hostname="localhost")
    client_tls_time = time.time() - client_tls_start
    
    client_total_time = time.time() - client_tcp_start
    
    cpu_samples = []
    p = psutil.Process(os.getpid())
    data = b'\x00' * DATA_SIZE
    total_sent = 0
    
    send_start = time.time()
    while time.time() - send_start < TEST_DURATION:
        try:
            sent = ssl_client_sock.send(data)
            total_sent += sent
            cpu_samples.append(p.cpu_percent(interval=0.1))
        except Exception:
            break
    
    ssl_client_sock.close()
    
    thread.join(timeout=5)
    
    return {
        "sctp_connection_ms": client_sctp_time * 1000,
        "tls_handshake_ms": client_tls_time * 1000,
        "total_connection_ms": client_total_time * 1000,
        "client_avg_cpu": sum(cpu_samples) / len(cpu_samples) if cpu_samples else 0,
        "client_peak_cpu": max(cpu_samples) if cpu_samples else 0,
        "server_avg_cpu": server_results.get("avg_cpu", 0),
        "server_peak_cpu": server_results.get("peak_cpu", 0)
    }

def test_quic_connection():
    server_proc = subprocess.Popen(
        ["python3", "-u", f"{SCRIPT_DIR}/quic_bench.py", "server", "--duration", str(TEST_DURATION)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        preexec_fn=os.setsid
    )
    
    server_pid = server_proc.pid
    server_cpu_samples = []
    
    def monitor_server():
        try:
            p = psutil.Process(server_pid)
            start_time = time.time()
            while time.time() - start_time < TEST_DURATION + 5:
                try:
                    cpu = p.cpu_percent(interval=0.1)
                    server_cpu_samples.append(cpu)
                except psutil.NoSuchProcess:
                    break
        except psutil.NoSuchProcess:
            pass
    
    monitor_thread = threading.Thread(target=monitor_server)
    monitor_thread.start()
    
    time.sleep(1)
    
    import asyncio
    from aioquic.asyncio import connect
    from aioquic.quic.configuration import QuicConfiguration
    
    async def quic_client():
        configuration = QuicConfiguration(is_client=True)
        configuration.verify_mode = ssl.CERT_NONE
        
        client_start = time.time()
        
        cpu_samples = []
        p = psutil.Process(os.getpid())
        
        async with connect("127.0.0.1", QUIC_PORT, configuration=configuration) as client:
            connection_time = time.time() - client_start
            
            data = b'\x00' * DATA_SIZE
            send_start = time.time()
            
            while time.time() - send_start < TEST_DURATION:
                client._quic.send_stream_data(0, data)
                cpu_samples.append(p.cpu_percent(interval=0.1))
                await asyncio.sleep(0.002)
            
            client._quic.send_stream_data(0, b'', end_stream=True)
            await asyncio.sleep(1)
            
            avg_cpu = sum(cpu_samples) / len(cpu_samples) if cpu_samples else 0
            peak_cpu = max(cpu_samples) if cpu_samples else 0
            
            return connection_time, avg_cpu, peak_cpu
    
    client_connection_time, client_avg_cpu, client_peak_cpu = asyncio.run(quic_client())
    
    try:
        server_proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(os.getpgid(server_proc.pid), signal.SIGKILL)
    
    monitor_thread.join(timeout=5)
    
    server_avg_cpu = sum(server_cpu_samples) / len(server_cpu_samples) if server_cpu_samples else 0
    server_peak_cpu = max(server_cpu_samples) if server_cpu_samples else 0
    
    return {
        "sctp_connection_ms": 0,
        "tls_handshake_ms": client_connection_time * 1000,
        "total_connection_ms": client_connection_time * 1000,
        "client_avg_cpu": client_avg_cpu,
        "client_peak_cpu": client_peak_cpu,
        "server_avg_cpu": server_avg_cpu,
        "server_peak_cpu": server_peak_cpu
    }

def main():
    print("=" * 70)
    print("Testing TLS/Encryption Handshake Time and CPU Consumption")
    print(f"Tests per scenario: {NUM_TESTS}, Duration: {TEST_DURATION}s each")
    print("=" * 70)
    
    results = {
        "sctp_plain": {
            "sctp_conn": [], "tls_handshake": [], "total_conn": [],
            "client_cpu": [], "server_cpu": [], "client_peak": [], "server_peak": []
        },
        "sctp_tls": {
            "sctp_conn": [], "tls_handshake": [], "total_conn": [],
            "client_cpu": [], "server_cpu": [], "client_peak": [], "server_peak": []
        },
        "quic": {
            "sctp_conn": [], "tls_handshake": [], "total_conn": [],
            "client_cpu": [], "server_cpu": [], "client_peak": [], "server_peak": []
        }
    }
    
    for i in range(NUM_TESTS):
        print(f"\n--- Test {i+1}/{NUM_TESTS} ---")
        
        print("[SCTP Plain] Testing connection time...")
        try:
            r = test_sctp_plain_connection()
            results["sctp_plain"]["sctp_conn"].append(r["sctp_connection_ms"])
            results["sctp_plain"]["tls_handshake"].append(0)
            results["sctp_plain"]["total_conn"].append(r["total_connection_ms"])
            results["sctp_plain"]["client_cpu"].append(r["client_avg_cpu"])
            results["sctp_plain"]["server_cpu"].append(r["server_avg_cpu"])
            results["sctp_plain"]["client_peak"].append(r["client_peak_cpu"])
            results["sctp_plain"]["server_peak"].append(r["server_peak_cpu"])
            print(f"  SCTP Connection: {r['sctp_connection_ms']:.1f}ms")
            print(f"  Client CPU: {r['client_avg_cpu']:.1f}%, Server CPU: {r['server_avg_cpu']:.1f}%")
        except Exception as e:
            print(f"  Error: {e}")
        
        time.sleep(2)
        
        print("[SCTP+TLS] Testing SCTP connection + TLS handshake...")
        try:
            r = test_sctp_tls_connection()
            results["sctp_tls"]["sctp_conn"].append(r["sctp_connection_ms"])
            results["sctp_tls"]["tls_handshake"].append(r["tls_handshake_ms"])
            results["sctp_tls"]["total_conn"].append(r["total_connection_ms"])
            results["sctp_tls"]["client_cpu"].append(r["client_avg_cpu"])
            results["sctp_tls"]["server_cpu"].append(r["server_avg_cpu"])
            results["sctp_tls"]["client_peak"].append(r["client_peak_cpu"])
            results["sctp_tls"]["server_peak"].append(r["server_peak_cpu"])
            print(f"  SCTP Connection: {r['sctp_connection_ms']:.1f}ms")
            print(f"  TLS Handshake: {r['tls_handshake_ms']:.1f}ms")
            print(f"  Total: {r['total_connection_ms']:.1f}ms")
            print(f"  Client CPU: {r['client_avg_cpu']:.1f}%, Server CPU: {r['server_avg_cpu']:.1f}%")
        except Exception as e:
            print(f"  Error: {e}")
        
        time.sleep(2)
        
        print("[QUIC] Testing QUIC connection (TLS 1.3 built-in)...")
        try:
            r = test_quic_connection()
            results["quic"]["sctp_conn"].append(0)
            results["quic"]["tls_handshake"].append(r["tls_handshake_ms"])
            results["quic"]["total_conn"].append(r["total_connection_ms"])
            results["quic"]["client_cpu"].append(r["client_avg_cpu"])
            results["quic"]["server_cpu"].append(r["server_avg_cpu"])
            results["quic"]["client_peak"].append(r["client_peak_cpu"])
            results["quic"]["server_peak"].append(r["server_peak_cpu"])
            print(f"  QUIC Connection: {r['tls_handshake_ms']:.1f}ms")
            print(f"  Client CPU: {r['client_avg_cpu']:.1f}%, Server CPU: {r['server_avg_cpu']:.1f}%")
        except Exception as e:
            print(f"  Error: {e}")
        
        time.sleep(2)
    
    print("\n" + "=" * 70)
    print("Calculating averages...")
    print("=" * 70)
    
    avg_results = {}
    for proto in ["sctp_plain", "sctp_tls", "quic"]:
        avg_results[proto] = {
            "sctp_conn": sum(results[proto]["sctp_conn"]) / len(results[proto]["sctp_conn"]) if results[proto]["sctp_conn"] else 0,
            "tls_handshake": sum(results[proto]["tls_handshake"]) / len(results[proto]["tls_handshake"]) if results[proto]["tls_handshake"] else 0,
            "total_conn": sum(results[proto]["total_conn"]) / len(results[proto]["total_conn"]) if results[proto]["total_conn"] else 0,
            "client_cpu": sum(results[proto]["client_cpu"]) / len(results[proto]["client_cpu"]) if results[proto]["client_cpu"] else 0,
            "server_cpu": sum(results[proto]["server_cpu"]) / len(results[proto]["server_cpu"]) if results[proto]["server_cpu"] else 0
        }
    
    print("\n" + "=" * 70)
    print("FINAL SUMMARY (Average)")
    print("=" * 70)
    print(f"{'Protocol':<15} {'SCTP Conn(ms)':>15} {'TLS Handshake(ms)':>18} {'Total Conn(ms)':>15} {'Client CPU%':>12} {'Server CPU%':>12}")
    print("-" * 95)
    for proto, label in [("sctp_plain", "SCTP Plain"), ("sctp_tls", "SCTP+TLS"), ("quic", "QUIC")]:
        r = avg_results[proto]
        print(f"{label:<15} {r['sctp_conn']:>15.1f} {r['tls_handshake']:>18.1f} {r['total_conn']:>15.1f} {r['client_cpu']:>12.1f} {r['server_cpu']:>12.1f}")
    
    print("\n" + "=" * 70)
    print("TLS Handshake Overhead Analysis")
    print("=" * 70)
    print(f"SCTP Connection (Plain):          {avg_results['sctp_plain']['sctp_conn']:.1f}ms")
    print(f"SCTP Connection (TLS added):      {avg_results['sctp_tls']['sctp_conn']:.1f}ms")
    print(f"SCTP TLS Handshake Time:          {avg_results['sctp_tls']['tls_handshake']:.1f}ms")
    print(f"SCTP+TLS Total Connection:        {avg_results['sctp_tls']['total_conn']:.1f}ms")
    
    print(f"\nQUIC (TLS 1.3 built-in):          {avg_results['quic']['tls_handshake']:.1f}ms")
    
    if avg_results['sctp_tls']['tls_handshake'] > 0 and avg_results['quic']['tls_handshake'] > 0:
        print(f"\nTLS Handshake Comparison:")
        print(f"  SCTP TLS (1.2/1.3):  {avg_results['sctp_tls']['tls_handshake']:.1f}ms")
        print(f"  QUIC TLS 1.3:        {avg_results['quic']['tls_handshake']:.1f}ms")
        print(f"  QUIC faster by:      {avg_results['sctp_tls']['tls_handshake'] - avg_results['quic']['tls_handshake']:.1f}ms")
    
    print("\n" + "=" * 70)
    print("Encryption CPU Overhead Analysis")
    print("=" * 70)
    print(f"SCTP Plain CPU:        Client {avg_results['sctp_plain']['client_cpu']:.1f}%, Server {avg_results['sctp_plain']['server_cpu']:.1f}%")
    print(f"SCTP+TLS CPU:          Client {avg_results['sctp_tls']['client_cpu']:.1f}%, Server {avg_results['sctp_tls']['server_cpu']:.1f}%")
    print(f"QUIC CPU:              Client {avg_results['quic']['client_cpu']:.1f}%, Server {avg_results['quic']['server_cpu']:.1f}%")
    
    tls_client_overhead = avg_results['sctp_tls']['client_cpu'] - avg_results['sctp_plain']['client_cpu']
    tls_server_overhead = avg_results['sctp_tls']['server_cpu'] - avg_results['sctp_plain']['server_cpu']
    
    print(f"\nSCTP TLS CPU Overhead:")
    print(f"  Client: +{tls_client_overhead:.1f}%")
    print(f"  Server: +{tls_server_overhead:.1f}%")
    
    print("\n" + "=" * 70)
    print("Generating charts...")
    print("=" * 70)
    
    tests = list(range(1, NUM_TESTS + 1))
    
    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    
    ax1 = axes[0, 0]
    ax1.plot(tests, results["sctp_plain"]["total_conn"], 'b-o', label='SCTP Plain (Connection)', linewidth=2, markersize=6)
    ax1.plot(tests, results["sctp_tls"]["tls_handshake"], 'g-s', label='SCTP+TLS (TLS Handshake)', linewidth=2, markersize=6)
    ax1.plot(tests, results["quic"]["tls_handshake"], 'r-^', label='QUIC (TLS 1.3 Handshake)', linewidth=2, markersize=6)
    ax1.axhline(y=avg_results["sctp_plain"]["total_conn"], color='b', linestyle='--', alpha=0.5, label=f'SCTP Plain Avg: {avg_results["sctp_plain"]["total_conn"]:.0f}ms')
    ax1.axhline(y=avg_results["sctp_tls"]["tls_handshake"], color='g', linestyle='--', alpha=0.5, label=f'SCTP TLS Avg: {avg_results["sctp_tls"]["tls_handshake"]:.0f}ms')
    ax1.axhline(y=avg_results["quic"]["tls_handshake"], color='r', linestyle='--', alpha=0.5, label=f'QUIC Avg: {avg_results["quic"]["tls_handshake"]:.0f}ms')
    ax1.set_xlabel('Test Number', fontsize=12)
    ax1.set_ylabel('Time (ms)', fontsize=12)
    ax1.set_title('Connection/Handshake Time Comparison', fontsize=14, fontweight='bold')
    ax1.legend(loc='best', fontsize=9)
    ax1.grid(True, alpha=0.3)
    
    ax2 = axes[0, 1]
    width = 0.35
    x_bar = range(len(tests))
    ax2.bar([i - width/2 for i in x_bar], results["sctp_tls"]["tls_handshake"], width, label='SCTP TLS', color='steelblue')
    ax2.bar([i + width/2 for i in x_bar], results["quic"]["tls_handshake"], width, label='QUIC TLS 1.3', color='indianred')
    ax2.set_xlabel('Test Number', fontsize=12)
    ax2.set_ylabel('TLS Handshake Time (ms)', fontsize=12)
    ax2.set_title('TLS Handshake Time Comparison', fontsize=14, fontweight='bold')
    ax2.legend(fontsize=10)
    ax2.grid(True, alpha=0.3, axis='y')
    ax2.set_xticks(x_bar)
    ax2.set_xticklabels(tests)
    
    ax3 = axes[1, 0]
    ax3.plot(tests, results["sctp_plain"]["client_cpu"], 'b-o', label='SCTP Plain Client', linewidth=2, markersize=6)
    ax3.plot(tests, results["sctp_tls"]["client_cpu"], 'g-s', label='SCTP+TLS Client', linewidth=2, markersize=6)
    ax3.plot(tests, results["quic"]["client_cpu"], 'r-^', label='QUIC Client', linewidth=2, markersize=6)
    ax3.set_xlabel('Test Number', fontsize=12)
    ax3.set_ylabel('CPU Usage (%)', fontsize=12)
    ax3.set_title('Client CPU Usage (Encryption Impact)', fontsize=14, fontweight='bold')
    ax3.legend(loc='best', fontsize=9)
    ax3.grid(True, alpha=0.3)
    
    ax4 = axes[1, 1]
    protocols = ['SCTP Plain', 'SCTP+TLS', 'QUIC']
    x_final = range(len(protocols))
    
    connection_times = [avg_results["sctp_plain"]["total_conn"], avg_results["sctp_tls"]["tls_handshake"], avg_results["quic"]["tls_handshake"]]
    client_cpus = [avg_results["sctp_plain"]["client_cpu"], avg_results["sctp_tls"]["client_cpu"], avg_results["quic"]["client_cpu"]]
    
    ax4.bar([i - width/2 for i in x_final], connection_times, width, label='Handshake/Connection (ms)', color='steelblue')
    ax4.bar([i + width/2 for i in x_final], client_cpus, width, label='Client CPU (%)', color='indianred')
    ax4.set_xlabel('Protocol', fontsize=12)
    ax4.set_ylabel('Value', fontsize=12)
    ax4.set_title('Average Comparison Summary', fontsize=14, fontweight='bold')
    ax4.set_xticks(x_final)
    ax4.set_xticklabels(protocols)
    ax4.legend(fontsize=10)
    ax4.grid(True, alpha=0.3, axis='y')
    
    plt.suptitle('SCTP vs QUIC: TLS Handshake Time and Encryption CPU Overhead\n(10 tests average)', 
                 fontsize=16, fontweight='bold')
    plt.tight_layout()
    
    output_path = f"{SCRIPT_DIR}/results/tls_encryption_comparison.png"
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    print(f"\nChart saved to: {output_path}")
    
    with open(f"{SCRIPT_DIR}/results/tls_encryption_data.txt", "w") as f:
        f.write("Test,SCTP_Plain_Conn_ms,SCTP_TLS_Conn_ms,SCTP_TLS_Handshake_ms,QUIC_Handshake_ms,SCTP_Plain_Client_CPU,SCTP_TLS_Client_CPU,QUIC_Client_CPU\n")
        for i in range(NUM_TESTS):
            f.write(f"{i+1},{results['sctp_plain']['total_conn'][i]:.1f},{results['sctp_tls']['total_conn'][i]:.1f},")
            f.write(f"{results['sctp_tls']['tls_handshake'][i]:.1f},{results['quic']['tls_handshake'][i]:.1f},")
            f.write(f"{results['sctp_plain']['client_cpu'][i]:.1f},{results['sctp_tls']['client_cpu'][i]:.1f},")
            f.write(f"{results['quic']['client_cpu'][i]:.1f}\n")
        
        f.write(f"\nAverage,{avg_results['sctp_plain']['total_conn']:.1f},{avg_results['sctp_tls']['total_conn']:.1f},")
        f.write(f"{avg_results['sctp_tls']['tls_handshake']:.1f},{avg_results['quic']['tls_handshake']:.1f},")
        f.write(f"{avg_results['sctp_plain']['client_cpu']:.1f},{avg_results['sctp_tls']['client_cpu']:.1f},")
        f.write(f"{avg_results['quic']['client_cpu']:.1f}\n")
    
    print(f"Data saved to: {SCRIPT_DIR}/results/tls_encryption_data.txt")

if __name__ == "__main__":
    main()