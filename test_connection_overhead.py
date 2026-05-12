#!/usr/bin/env python3
import subprocess
import time
import re
import matplotlib.pyplot as plt
import os
import signal
import psutil

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
NUM_TESTS = 10
TEST_DURATION = 3

def run_test_and_measure(script, mode):
    server_proc = subprocess.Popen(
        ["python3", "-u", f"{SCRIPT_DIR}/{script}", mode, "--duration", str(TEST_DURATION)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        preexec_fn=os.setsid
    )
    
    server_pid = server_proc.pid
    
    server_cpu_samples = []
    server_mem_samples = []
    
    def monitor_server():
        try:
            p = psutil.Process(server_pid)
            start_time = time.time()
            while time.time() - start_time < TEST_DURATION + 5:
                try:
                    cpu = p.cpu_percent(interval=0.1)
                    mem = p.memory_info().rss / 1024 / 1024
                    server_cpu_samples.append(cpu)
                    server_mem_samples.append(mem)
                except psutil.NoSuchProcess:
                    break
        except psutil.NoSuchProcess:
            pass
    
    import threading
    monitor_thread = threading.Thread(target=monitor_server)
    monitor_thread.start()
    
    time.sleep(1)
    
    client_start = time.time()
    
    client_proc = subprocess.Popen(
        ["python3", "-u", f"{SCRIPT_DIR}/{script}", "client", "--duration", str(TEST_DURATION)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        preexec_fn=os.setsid
    )
    
    client_pid = client_proc.pid
    
    client_cpu_samples = []
    client_mem_samples = []
    
    def monitor_client():
        try:
            p = psutil.Process(client_pid)
            start_time = time.time()
            while time.time() - start_time < TEST_DURATION + 3:
                try:
                    cpu = p.cpu_percent(interval=0.1)
                    mem = p.memory_info().rss / 1024 / 1024
                    client_cpu_samples.append(cpu)
                    client_mem_samples.append(mem)
                except psutil.NoSuchProcess:
                    break
        except psutil.NoSuchProcess:
            pass
    
    client_monitor_thread = threading.Thread(target=monitor_client)
    client_monitor_thread.start()
    
    client_out, _ = client_proc.communicate(timeout=TEST_DURATION + 30)
    client_connection_time = time.time() - client_start
    
    server_out, _ = server_proc.communicate(timeout=30)
    
    monitor_thread.join(timeout=5)
    client_monitor_thread.join(timeout=5)
    
    connection_time_match = re.search(r"Duration:\s+([\d.]+)s", client_out)
    throughput_match = re.search(r"Throughput:\s+([\d.]+)\s+Gbps", client_out)
    
    server_avg_cpu = sum(server_cpu_samples) / len(server_cpu_samples) if server_cpu_samples else 0
    server_peak_cpu = max(server_cpu_samples) if server_cpu_samples else 0
    server_avg_mem = sum(server_mem_samples) / len(server_mem_samples) if server_mem_samples else 0
    server_peak_mem = max(server_mem_samples) if server_mem_samples else 0
    
    client_avg_cpu = sum(client_cpu_samples) / len(client_cpu_samples) if client_cpu_samples else 0
    client_peak_cpu = max(client_cpu_samples) if client_cpu_samples else 0
    client_avg_mem = sum(client_mem_samples) / len(client_mem_samples) if client_mem_samples else 0
    client_peak_mem = max(client_mem_samples) if client_mem_samples else 0
    
    throughput = float(throughput_match.group(1)) if throughput_match else 0
    
    return {
        "connection_time": client_connection_time,
        "server_avg_cpu": server_avg_cpu,
        "server_peak_cpu": server_peak_cpu,
        "server_avg_mem": server_avg_mem,
        "server_peak_mem": server_peak_mem,
        "client_avg_cpu": client_avg_cpu,
        "client_peak_cpu": client_peak_cpu,
        "client_avg_mem": client_avg_mem,
        "client_peak_mem": client_peak_mem,
        "throughput": throughput
    }

def main():
    print("=" * 70)
    print("Testing SCTP vs QUIC: Connection Setup and Resource Consumption")
    print(f"Tests per scenario: {NUM_TESTS}, Duration: {TEST_DURATION}s each")
    print("=" * 70)
    
    results = {
        "sctp": {
            "connection": [], "server_cpu": [], "client_cpu": [],
            "server_mem": [], "client_mem": [], "throughput": []
        },
        "quic": {
            "connection": [], "server_cpu": [], "client_cpu": [],
            "server_mem": [], "client_mem": [], "throughput": []
        }
    }
    
    for i in range(NUM_TESTS):
        print(f"\n--- Test {i+1}/{NUM_TESTS} ---")
        
        print("[SCTP] Testing...")
        try:
            r = run_test_and_measure("sctp_bench.py", "server")
            results["sctp"]["connection"].append(r["connection_time"])
            results["sctp"]["server_cpu"].append(r["server_avg_cpu"])
            results["sctp"]["client_cpu"].append(r["client_avg_cpu"])
            results["sctp"]["server_mem"].append(r["server_avg_mem"])
            results["sctp"]["client_mem"].append(r["client_avg_mem"])
            results["sctp"]["throughput"].append(r["throughput"])
            print(f"  Connection: {r['connection_time']*1000:.0f}ms")
            print(f"  CPU: server={r['server_avg_cpu']:.1f}%, client={r['client_avg_cpu']:.1f}%")
            print(f"  Memory: server={r['server_avg_mem']:.1f}MB, client={r['client_avg_mem']:.1f}MB")
            print(f"  Throughput: {r['throughput']:.3f} Gbps")
        except Exception as e:
            print(f"  Error: {e}")
        
        time.sleep(2)
        
        print("[QUIC] Testing...")
        try:
            r = run_test_and_measure("quic_bench.py", "server")
            results["quic"]["connection"].append(r["connection_time"])
            results["quic"]["server_cpu"].append(r["server_avg_cpu"])
            results["quic"]["client_cpu"].append(r["client_avg_cpu"])
            results["quic"]["server_mem"].append(r["server_avg_mem"])
            results["quic"]["client_mem"].append(r["client_avg_mem"])
            results["quic"]["throughput"].append(r["throughput"])
            print(f"  Connection: {r['connection_time']*1000:.0f}ms")
            print(f"  CPU: server={r['server_avg_cpu']:.1f}%, client={r['client_avg_cpu']:.1f}%")
            print(f"  Memory: server={r['server_avg_mem']:.1f}MB, client={r['client_avg_mem']:.1f}MB")
            print(f"  Throughput: {r['throughput']:.3f} Gbps")
        except Exception as e:
            print(f"  Error: {e}")
        
        time.sleep(2)
    
    print("\n" + "=" * 70)
    print("Calculating averages...")
    print("=" * 70)
    
    avg_results = {}
    for proto in ["sctp", "quic"]:
        avg_results[proto] = {
            "connection_ms": sum(results[proto]["connection"]) / len(results[proto]["connection"]) * 1000 if results[proto]["connection"] else 0,
            "server_cpu": sum(results[proto]["server_cpu"]) / len(results[proto]["server_cpu"]) if results[proto]["server_cpu"] else 0,
            "client_cpu": sum(results[proto]["client_cpu"]) / len(results[proto]["client_cpu"]) if results[proto]["client_cpu"] else 0,
            "server_mem": sum(results[proto]["server_mem"]) / len(results[proto]["server_mem"]) if results[proto]["server_mem"] else 0,
            "client_mem": sum(results[proto]["client_mem"]) / len(results[proto]["client_mem"]) if results[proto]["client_mem"] else 0,
            "throughput": sum(results[proto]["throughput"]) / len(results[proto]["throughput"]) if results[proto]["throughput"] else 0
        }
    
    print("\n" + "=" * 70)
    print("FINAL SUMMARY (Average)")
    print("=" * 70)
    print(f"{'Protocol':<10} {'Connection(ms)':>15} {'Server CPU%':>12} {'Client CPU%':>12} {'Server Mem':>12} {'Client Mem':>12} {'Throughput':>10}")
    print("-" * 95)
    for proto in ["sctp", "quic"]:
        r = avg_results[proto]
        print(f"{proto.upper():<10} {r['connection_ms']:>15.1f} {r['server_cpu']:>12.1f} {r['client_cpu']:>12.1f} {r['server_mem']:>10.1f}MB {r['client_mem']:>10.1f}MB {r['throughput']:>10.3f}Gbps")
    
    sctp_plain_conn = avg_results["sctp"]["connection_ms"]
    quic_conn = avg_results["quic"]["connection_ms"]
    
    estimated_tls_overhead = quic_conn - sctp_plain_conn
    
    print("\n" + "=" * 70)
    print("Encryption Overhead Analysis")
    print("=" * 70)
    print(f"SCTP Plain (no TLS):     {sctp_plain_conn:.1f}ms connection")
    print(f"QUIC (built-in TLS):     {quic_conn:.1f}ms connection")
    print(f"Estimated TLS overhead:  {estimated_tls_overhead:.1f}ms (QUIC vs SCTP Plain)")
    print(f"TLS overhead ratio:      {estimated_tls_overhead/sctp_plain_conn*100:.1f}% of SCTP Plain connection time")
    
    print("\n" + "=" * 70)
    print("CPU Overhead Analysis")
    print("=" * 70)
    print(f"SCTP Server CPU: {avg_results['sctp']['server_cpu']:.1f}%")
    print(f"QUIC Server CPU: {avg_results['quic']['server_cpu']:.1f}%")
    print(f"QUIC CPU overhead: {avg_results['quic']['server_cpu'] - avg_results['sctp']['server_cpu']:.1f}%")
    
    print("\n" + "=" * 70)
    print("Generating charts...")
    print("=" * 70)
    
    tests = list(range(1, NUM_TESTS + 1))
    
    fig, axes = plt.subplots(2, 3, figsize=(18, 12))
    
    ax1 = axes[0, 0]
    ax1.plot(tests, [x*1000 for x in results["sctp"]["connection"]], 'b-o', label='SCTP Plain', linewidth=2, markersize=6)
    ax1.plot(tests, [x*1000 for x in results["quic"]["connection"]], 'r-s', label='QUIC', linewidth=2, markersize=6)
    ax1.axhline(y=avg_results["sctp"]["connection_ms"], color='b', linestyle='--', alpha=0.5, label=f'SCTP Avg: {avg_results["sctp"]["connection_ms"]:.0f}ms')
    ax1.axhline(y=avg_results["quic"]["connection_ms"], color='r', linestyle='--', alpha=0.5, label=f'QUIC Avg: {avg_results["quic"]["connection_ms"]:.0f}ms')
    ax1.set_xlabel('Test Number', fontsize=11)
    ax1.set_ylabel('Connection Time (ms)', fontsize=11)
    ax1.set_title('Connection Establishment Time', fontsize=13, fontweight='bold')
    ax1.legend(fontsize=9)
    ax1.grid(True, alpha=0.3)
    
    ax2 = axes[0, 1]
    ax2.plot(tests, results["sctp"]["server_cpu"], 'b-o', label='SCTP Server', linewidth=2, markersize=6)
    ax2.plot(tests, results["quic"]["server_cpu"], 'r-s', label='QUIC Server', linewidth=2, markersize=6)
    ax2.set_xlabel('Test Number', fontsize=11)
    ax2.set_ylabel('CPU Usage (%)', fontsize=11)
    ax2.set_title('Server Average CPU Usage', fontsize=13, fontweight='bold')
    ax2.legend(fontsize=9)
    ax2.grid(True, alpha=0.3)
    
    ax3 = axes[0, 2]
    ax3.plot(tests, results["sctp"]["client_cpu"], 'b-o', label='SCTP Client', linewidth=2, markersize=6)
    ax3.plot(tests, results["quic"]["client_cpu"], 'r-s', label='QUIC Client', linewidth=2, markersize=6)
    ax3.set_xlabel('Test Number', fontsize=11)
    ax3.set_ylabel('CPU Usage (%)', fontsize=11)
    ax3.set_title('Client Average CPU Usage', fontsize=13, fontweight='bold')
    ax3.legend(fontsize=9)
    ax3.grid(True, alpha=0.3)
    
    ax4 = axes[1, 0]
    ax4.plot(tests, results["sctp"]["server_mem"], 'b-o', label='SCTP Server', linewidth=2, markersize=6)
    ax4.plot(tests, results["quic"]["server_mem"], 'r-s', label='QUIC Server', linewidth=2, markersize=6)
    ax4.set_xlabel('Test Number', fontsize=11)
    ax4.set_ylabel('Memory Usage (MB)', fontsize=11)
    ax4.set_title('Server Memory Usage', fontsize=13, fontweight='bold')
    ax4.legend(fontsize=9)
    ax4.grid(True, alpha=0.3)
    
    ax5 = axes[1, 1]
    ax5.plot(tests, results["sctp"]["throughput"], 'b-o', label='SCTP', linewidth=2, markersize=6)
    ax5.plot(tests, results["quic"]["throughput"], 'r-s', label='QUIC', linewidth=2, markersize=6)
    ax5.set_xlabel('Test Number', fontsize=11)
    ax5.set_ylabel('Throughput (Gbps)', fontsize=11)
    ax5.set_title('Throughput Comparison', fontsize=13, fontweight='bold')
    ax5.legend(fontsize=9)
    ax5.grid(True, alpha=0.3)
    
    ax6 = axes[1, 2]
    protocols = ['SCTP Plain', 'QUIC']
    x_bar = range(len(protocols))
    width = 0.25
    ax6.bar([i - width for i in x_bar], [avg_results["sctp"]["connection_ms"], avg_results["quic"]["connection_ms"]], width, label='Connection (ms)', color='steelblue')
    ax6.bar([i for i in x_bar], [avg_results["sctp"]["server_cpu"], avg_results["quic"]["server_cpu"]], width, label='Server CPU (%)', color='indianred')
    ax6.bar([i + width for i in x_bar], [avg_results["sctp"]["client_cpu"], avg_results["quic"]["client_cpu"]], width, label='Client CPU (%)', color='seagreen')
    ax6.set_xlabel('Protocol', fontsize=11)
    ax6.set_ylabel('Value', fontsize=11)
    ax6.set_title('Average Performance Summary', fontsize=13, fontweight='bold')
    ax6.set_xticks(x_bar)
    ax6.set_xticklabels(protocols)
    ax6.legend(fontsize=9)
    ax6.grid(True, alpha=0.3, axis='y')
    
    plt.suptitle('SCTP vs QUIC: Connection Setup and Encryption Overhead\n(10 tests average)', 
                 fontsize=16, fontweight='bold')
    plt.tight_layout()
    
    output_path = f"{SCRIPT_DIR}/results/connection_encryption_comparison.png"
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    print(f"\nChart saved to: {output_path}")
    
    with open(f"{SCRIPT_DIR}/results/connection_encryption_data.txt", "w") as f:
        f.write("Test,SCTP_Connection_ms,QUIC_Connection_ms,SCTP_Server_CPU,QUIC_Server_CPU,SCTP_Client_CPU,QUIC_Client_CPU,SCTP_Server_Mem_MB,QUIC_Server_Mem_MB\n")
        for i in range(NUM_TESTS):
            f.write(f"{i+1},{results['sctp']['connection'][i]*1000:.1f},{results['quic']['connection'][i]*1000:.1f},")
            f.write(f"{results['sctp']['server_cpu'][i]:.1f},{results['quic']['server_cpu'][i]:.1f},")
            f.write(f"{results['sctp']['client_cpu'][i]:.1f},{results['quic']['client_cpu'][i]:.1f},")
            f.write(f"{results['sctp']['server_mem'][i]:.1f},{results['quic']['server_mem'][i]:.1f}\n")
        f.write(f"\nAverage,{avg_results['sctp']['connection_ms']:.1f},{avg_results['quic']['connection_ms']:.1f},")
        f.write(f"{avg_results['sctp']['server_cpu']:.1f},{avg_results['quic']['server_cpu']:.1f},")
        f.write(f"{avg_results['sctp']['client_cpu']:.1f},{avg_results['quic']['client_cpu']:.1f},")
        f.write(f"{avg_results['sctp']['server_mem']:.1f},{avg_results['quic']['server_mem']:.1f}\n")
    
    print(f"Data saved to: {SCRIPT_DIR}/results/connection_encryption_data.txt")

if __name__ == "__main__":
    main()