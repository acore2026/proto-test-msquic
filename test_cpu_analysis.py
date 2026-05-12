#!/usr/bin/env python3
import subprocess
import time
import re
import matplotlib.pyplot as plt
import os
import psutil
import threading

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DURATION = 5
NUM_TESTS = 5

def monitor_process_cpu(pid, duration, results_dict, key):
    try:
        p = psutil.Process(pid)
        cpu_samples = []
        mem_samples = []
        start_time = time.time()
        while time.time() - start_time < duration:
            try:
                cpu = p.cpu_percent(interval=0.2)
                cpu_samples.append(cpu)
                mem_samples.append(p.memory_info().rss / 1024 / 1024)
            except psutil.NoSuchProcess:
                break
        results_dict[key] = {
            "avg_cpu": sum(cpu_samples) / len(cpu_samples) if cpu_samples else 0,
            "peak_cpu": max(cpu_samples) if cpu_samples else 0,
            "avg_mem": sum(mem_samples) / len(mem_samples) if mem_samples else 0,
            "peak_mem": max(mem_samples) if mem_samples else 0
        }
    except psutil.NoSuchProcess:
        results_dict[key] = {"avg_cpu": 0, "peak_cpu": 0, "avg_mem": 0, "peak_mem": 0}

def monitor_system_cpu(duration, results_dict):
    cpu_samples = []
    start_time = time.time()
    while time.time() - start_time < duration:
        cpu_total = psutil.cpu_percent(interval=0.2)
        cpu_samples.append(cpu_total)
    results_dict["system"] = {
        "avg": sum(cpu_samples) / len(cpu_samples) if cpu_samples else 0,
        "peak": max(cpu_samples) if cpu_samples else 0
    }

def run_single_test(proto, script):
    print(f"  Starting {proto} server...")
    server_proc = subprocess.Popen(
        ["python3", "-u", f"{SCRIPT_DIR}/{script}", "server", "--duration", str(DURATION)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    server_pid = server_proc.pid
    server_cpu_results = {}
    server_monitor = threading.Thread(
        target=monitor_process_cpu,
        args=(server_pid, DURATION + 5, server_cpu_results, "server")
    )
    server_monitor.start()
    
    system_cpu_results = {}
    system_monitor = threading.Thread(
        target=monitor_system_cpu,
        args=(DURATION + 5, system_cpu_results)
    )
    system_monitor.start()
    
    time.sleep(2)
    
    print(f"  Starting {proto} client...")
    client_proc = subprocess.Popen(
        ["python3", "-u", f"{SCRIPT_DIR}/{script}", "client", "--duration", str(DURATION)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    client_pid = client_proc.pid
    client_cpu_results = {}
    client_monitor = threading.Thread(
        target=monitor_process_cpu,
        args=(client_pid, DURATION + 3, client_cpu_results, "client")
    )
    client_monitor.start()
    
    client_out, _ = client_proc.communicate(timeout=DURATION + 30)
    server_out, _ = server_proc.communicate(timeout=30)
    
    server_monitor.join(timeout=5)
    client_monitor.join(timeout=5)
    system_monitor.join(timeout=5)
    
    client_throughput = 0.0
    server_throughput = 0.0
    
    match = re.search(r"Throughput:\s+([\d.]+)\s+Gbps", client_out)
    if match:
        client_throughput = float(match.group(1))
    
    match = re.search(r"Throughput:\s+([\d.]+)\s+Gbps", server_out)
    if match:
        server_throughput = float(match.group(1))
    
    return {
        "client_throughput": client_throughput,
        "server_throughput": server_throughput,
        "server_avg_cpu": server_cpu_results.get("server", {}).get("avg_cpu", 0),
        "server_peak_cpu": server_cpu_results.get("server", {}).get("peak_cpu", 0),
        "server_avg_mem": server_cpu_results.get("server", {}).get("avg_mem", 0),
        "client_avg_cpu": client_cpu_results.get("client", {}).get("avg_cpu", 0),
        "client_peak_cpu": client_cpu_results.get("client", {}).get("peak_cpu", 0),
        "client_avg_mem": client_cpu_results.get("client", {}).get("avg_mem", 0),
        "system_avg_cpu": system_cpu_results.get("system", {}).get("avg", 0),
        "system_peak_cpu": system_cpu_results.get("system", {}).get("peak", 0)
    }

def main():
    print("=" * 70)
    print("SCTP vs QUIC Performance and CPU Analysis")
    print("=" * 70)
    print(f"\nTests: {NUM_TESTS}, Duration: {DURATION}s each")
    print("\nExplanation of CPU measurement:")
    print("- Process CPU: User-space process CPU usage (psutil.Process.cpu_percent)")
    print("- System CPU: Total system CPU including kernel (psutil.cpu_percent)")
    print("\nSCTP: Kernel implementation - most work in kernel, low user-space CPU")
    print("QUIC: User-space implementation - all work in user-space, high CPU")
    print("=" * 70)
    
    sctp_results = {
        "client_throughput": [], "server_throughput": [],
        "server_avg_cpu": [], "client_avg_cpu": [],
        "server_avg_mem": [], "client_avg_mem": [],
        "system_avg_cpu": []
    }
    quic_results = {
        "client_throughput": [], "server_throughput": [],
        "server_avg_cpu": [], "client_avg_cpu": [],
        "server_avg_mem": [], "client_avg_mem": [],
        "system_avg_cpu": []
    }
    
    for i in range(NUM_TESTS):
        print(f"\n{'='*70}")
        print(f"Test {i+1}/{NUM_TESTS}")
        print("=" * 70)
        
        print("\n[SCTP Test]")
        r = run_single_test("SCTP", "sctp_bench.py")
        sctp_results["client_throughput"].append(r["client_throughput"])
        sctp_results["server_throughput"].append(r["server_throughput"])
        sctp_results["server_avg_cpu"].append(r["server_avg_cpu"])
        sctp_results["client_avg_cpu"].append(r["client_avg_cpu"])
        sctp_results["server_avg_mem"].append(r["server_avg_mem"])
        sctp_results["client_avg_mem"].append(r["client_avg_mem"])
        sctp_results["system_avg_cpu"].append(r["system_avg_cpu"])
        print(f"  Throughput: Client={r['client_throughput']:.3f} Gbps, Server={r['server_throughput']:.3f} Gbps")
        print(f"  Process CPU: Server={r['server_avg_cpu']:.1f}%, Client={r['client_avg_cpu']:.1f}%")
        print(f"  Memory: Server={r['server_avg_mem']:.1f}MB, Client={r['client_avg_mem']:.1f}MB")
        print(f"  System Total CPU: {r['system_avg_cpu']:.1f}%")
        
        time.sleep(2)
        
        print("\n[QUIC Test]")
        r = run_single_test("QUIC", "quic_bench.py")
        quic_results["client_throughput"].append(r["client_throughput"])
        quic_results["server_throughput"].append(r["server_throughput"])
        quic_results["server_avg_cpu"].append(r["server_avg_cpu"])
        quic_results["client_avg_cpu"].append(r["client_avg_cpu"])
        quic_results["server_avg_mem"].append(r["server_avg_mem"])
        quic_results["client_avg_mem"].append(r["client_avg_mem"])
        quic_results["system_avg_cpu"].append(r["system_avg_cpu"])
        print(f"  Throughput: Client={r['client_throughput']:.3f} Gbps, Server={r['server_throughput']:.3f} Gbps")
        print(f"  Process CPU: Server={r['server_avg_cpu']:.1f}%, Client={r['client_avg_cpu']:.1f}%")
        print(f"  Memory: Server={r['server_avg_mem']:.1f}MB, Client={r['client_avg_mem']:.1f}MB")
        print(f"  System Total CPU: {r['system_avg_cpu']:.1f}%")
        
        time.sleep(2)
    
    print("\n" + "=" * 70)
    print("Summary (Average)")
    print("=" * 70)
    
    sctp_c_avg_t = sum(sctp_results["client_throughput"]) / NUM_TESTS
    sctp_s_avg_t = sum(sctp_results["server_throughput"]) / NUM_TESTS
    quic_c_avg_t = sum(quic_results["client_throughput"]) / NUM_TESTS
    quic_s_avg_t = sum(quic_results["server_throughput"]) / NUM_TESTS
    
    sctp_s_avg_cpu = sum(sctp_results["server_avg_cpu"]) / NUM_TESTS
    sctp_c_avg_cpu = sum(sctp_results["client_avg_cpu"]) / NUM_TESTS
    quic_s_avg_cpu = sum(quic_results["server_avg_cpu"]) / NUM_TESTS
    quic_c_avg_cpu = sum(quic_results["client_avg_cpu"]) / NUM_TESTS
    
    sctp_sys_cpu = sum(sctp_results["system_avg_cpu"]) / NUM_TESTS
    quic_sys_cpu = sum(quic_results["system_avg_cpu"]) / NUM_TESTS
    
    sctp_s_mem = sum(sctp_results["server_avg_mem"]) / NUM_TESTS
    quic_s_mem = sum(quic_results["server_avg_mem"]) / NUM_TESTS
    
    print(f"\n{'Metric':<25} {'SCTP':>15} {'QUIC':>15} {'Ratio':>15}")
    print("-" * 70)
    print(f"{'Client Throughput':<25} {sctp_c_avg_t:>15.3f} {quic_c_avg_t:>15.3f} Gbps")
    print(f"{'Server Throughput':<25} {sctp_s_avg_t:>15.3f} {quic_s_avg_t:>15.3f} Gbps")
    print(f"{'Server Process CPU':<25} {sctp_s_avg_cpu:>15.1f} {quic_s_avg_cpu:>15.1f} {quic_s_avg_cpu/sctp_s_avg_cpu if sctp_s_avg_cpu > 0 else 'inf':>15.1f}x")
    print(f"{'Client Process CPU':<25} {sctp_c_avg_cpu:>15.1f} {quic_c_avg_cpu:>15.1f} {quic_c_avg_cpu/sctp_c_avg_cpu if sctp_c_avg_cpu > 0 else 'inf':>15.1f}x")
    print(f"{'System Total CPU':<25} {sctp_sys_cpu:>15.1f} {quic_sys_cpu:>15.1f}")
    print(f"{'Server Memory':<25} {sctp_s_mem:>15.1f} {quic_s_mem:>15.1f} MB")
    
    print("\n" + "=" * 70)
    print("Analysis: Why QUIC has higher user-space CPU?")
    print("=" * 70)
    print("""
1. SCTP (Kernel Implementation):
   - Socket operations handled by kernel
   - User-space process only calls recv()/send()
   - Kernel handles: congestion control, retransmission, checksum
   - User-space CPU: ~0% (most work in kernel)

2. QUIC (User-Space Implementation - aioquic):
   - All protocol logic in user-space Python
   - TLS 1.3 encryption/decryption in user-space
   - Congestion control, packet framing, ACK handling
   - User-space CPU: ~50%+ (all processing visible)

3. Measurement Method:
   - psutil.Process.cpu_percent() = user-space process CPU only
   - psutil.cpu_percent() = total system CPU (user + kernel)

The CPU difference is REAL and expected:
- SCTP kernel CPU is NOT measured by psutil.Process
- QUIC user-space CPU IS fully measured
""")
    
    print("\n" + "=" * 70)
    print("Generating charts...")
    print("=" * 70)
    
    tests = list(range(1, NUM_TESTS + 1))
    
    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    
    ax1 = axes[0, 0]
    ax1.plot(tests, sctp_results["client_throughput"], 'b-o', label='SCTP', linewidth=2, markersize=8)
    ax1.plot(tests, quic_results["client_throughput"], 'r-s', label='QUIC', linewidth=2, markersize=8)
    ax1.set_xlabel('Test Number', fontsize=12)
    ax1.set_ylabel('Throughput (Gbps)', fontsize=12)
    ax1.set_title('Client Throughput', fontsize=14, fontweight='bold')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    
    ax2 = axes[0, 1]
    ax2.plot(tests, sctp_results["server_avg_cpu"], 'b-o', label='SCTP Server', linewidth=2, markersize=8)
    ax2.plot(tests, quic_results["server_avg_cpu"], 'r-s', label='QUIC Server', linewidth=2, markersize=8)
    ax2.set_xlabel('Test Number', fontsize=12)
    ax2.set_ylabel('User-Space CPU (%)', fontsize=12)
    ax2.set_title('Server Process CPU (User-Space Only)', fontsize=14, fontweight='bold')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    
    ax3 = axes[1, 0]
    ax3.plot(tests, sctp_results["system_avg_cpu"], 'b-o', label='SCTP Total', linewidth=2, markersize=8)
    ax3.plot(tests, quic_results["system_avg_cpu"], 'r-s', label='QUIC Total', linewidth=2, markersize=8)
    ax3.set_xlabel('Test Number', fontsize=12)
    ax3.set_ylabel('System CPU (%)', fontsize=12)
    ax3.set_title('System Total CPU (User + Kernel)', fontsize=14, fontweight='bold')
    ax3.legend()
    ax3.grid(True, alpha=0.3)
    
    ax4 = axes[1, 1]
    protocols = ['SCTP', 'QUIC']
    x_bar = range(len(protocols))
    width = 0.25
    ax4.bar([i - width for i in x_bar], [sctp_s_avg_cpu, quic_s_avg_cpu], width, label='Server Process CPU', color='steelblue')
    ax4.bar([i for i in x_bar], [sctp_sys_cpu, quic_sys_cpu], width, label='System Total CPU', color='indianred')
    ax4.bar([i + width for i in x_bar], [sctp_s_mem, quic_s_mem], width, label='Server Memory (MB)', color='seagreen')
    ax4.set_xlabel('Protocol', fontsize=12)
    ax4.set_ylabel('Value', fontsize=12)
    ax4.set_title('Summary Comparison', fontsize=14, fontweight='bold')
    ax4.set_xticks(x_bar)
    ax4.set_xticklabels(protocols)
    ax4.legend()
    ax4.grid(True, alpha=0.3, axis='y')
    
    plt.suptitle(f'SCTP vs QUIC: Throughput and Resource Usage\n(Process CPU = User-space only, System CPU = User+Kernel)', 
                 fontsize=16, fontweight='bold')
    plt.tight_layout()
    
    output_path = f"{SCRIPT_DIR}/results/cpu_analysis.png"
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    print(f"Chart saved to: {output_path}")

if __name__ == "__main__":
    main()