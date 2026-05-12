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
NUM_TESTS = 10

def monitor_system_cpu(duration, results_dict, key):
    cpu_samples = []
    start_time = time.time()
    while time.time() - start_time < duration:
        cpu_total = psutil.cpu_percent(interval=0.2)
        cpu_samples.append(cpu_total)
    results_dict[key] = {
        "avg": sum(cpu_samples) / len(cpu_samples) if cpu_samples else 0,
        "peak": max(cpu_samples) if cpu_samples else 0
    }

def run_single_test(proto, script):
    server_proc = subprocess.Popen(
        ["python3", "-u", f"{SCRIPT_DIR}/{script}", "server", "--duration", str(DURATION)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    server_cpu_results = {}
    server_monitor = threading.Thread(
        target=monitor_system_cpu,
        args=(DURATION + 5, server_cpu_results, "server_system")
    )
    server_monitor.start()
    
    time.sleep(2)
    
    client_proc = subprocess.Popen(
        ["python3", "-u", f"{SCRIPT_DIR}/{script}", "client", "--duration", str(DURATION)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    client_cpu_results = {}
    client_monitor = threading.Thread(
        target=monitor_system_cpu,
        args=(DURATION + 3, client_cpu_results, "client_system")
    )
    client_monitor.start()
    
    client_out, _ = client_proc.communicate(timeout=DURATION + 30)
    server_out, _ = server_proc.communicate(timeout=30)
    
    server_monitor.join(timeout=5)
    client_monitor.join(timeout=5)
    
    client_throughput = 0.0
    server_throughput = 0.0
    
    match = re.search(r"Throughput:\s+([\d.]+)\s+Gbps", client_out)
    if match:
        client_throughput = float(match.group(1))
    
    match = re.search(r"Throughput:\s+([\d.]+)\s+Gbps", server_out)
    if match:
        server_throughput = float(match.group(1))
    
    server_system_avg_cpu = server_cpu_results.get("server_system", {}).get("avg", 0)
    server_system_peak_cpu = server_cpu_results.get("server_system", {}).get("peak", 0)
    client_system_avg_cpu = client_cpu_results.get("client_system", {}).get("avg", 0)
    client_system_peak_cpu = client_cpu_results.get("client_system", {}).get("peak", 0)
    
    return {
        "client_throughput": client_throughput,
        "server_throughput": server_throughput,
        "server_system_avg_cpu": server_system_avg_cpu,
        "server_system_peak_cpu": server_system_peak_cpu,
        "client_system_avg_cpu": client_system_avg_cpu,
        "client_system_peak_cpu": client_system_peak_cpu
    }

def main():
    print("=" * 70)
    print(f"Running {NUM_TESTS} tests per protocol (duration: {DURATION}s each)")
    print("Measuring throughput and System CPU (User-Space + Kernel)")
    print("=" * 70)
    
    sctp_results = {
        "client_throughput": [], "server_throughput": [],
        "server_system_cpu": [], "client_system_cpu": [],
        "server_system_peak": [], "client_system_peak": []
    }
    quic_results = {
        "client_throughput": [], "server_throughput": [],
        "server_system_cpu": [], "client_system_cpu": [],
        "server_system_peak": [], "client_system_peak": []
    }
    
    for i in range(NUM_TESTS):
        print(f"\n--- Test {i+1}/{NUM_TESTS} ---")
        
        print(f"[SCTP] Running test {i+1}...")
        r = run_single_test("SCTP", "sctp_bench.py")
        sctp_results["client_throughput"].append(r["client_throughput"])
        sctp_results["server_throughput"].append(r["server_throughput"])
        sctp_results["server_system_cpu"].append(r["server_system_avg_cpu"])
        sctp_results["client_system_cpu"].append(r["client_system_avg_cpu"])
        sctp_results["server_system_peak"].append(r["server_system_peak_cpu"])
        sctp_results["client_system_peak"].append(r["client_system_peak_cpu"])
        print(f"  Throughput: Client={r['client_throughput']:.3f} Gbps, Server={r['server_throughput']:.3f} Gbps")
        print(f"  System CPU: Server={r['server_system_avg_cpu']:.1f}% (peak {r['server_system_peak_cpu']:.1f}%), Client={r['client_system_avg_cpu']:.1f}% (peak {r['client_system_peak_cpu']:.1f}%)")
        
        time.sleep(2)
        
        print(f"[QUIC] Running test {i+1}...")
        r = run_single_test("QUIC", "quic_bench.py")
        quic_results["client_throughput"].append(r["client_throughput"])
        quic_results["server_throughput"].append(r["server_throughput"])
        quic_results["server_system_cpu"].append(r["server_system_avg_cpu"])
        quic_results["client_system_cpu"].append(r["client_system_avg_cpu"])
        quic_results["server_system_peak"].append(r["server_system_peak_cpu"])
        quic_results["client_system_peak"].append(r["client_system_peak_cpu"])
        print(f"  Throughput: Client={r['client_throughput']:.3f} Gbps, Server={r['server_throughput']:.3f} Gbps")
        print(f"  System CPU: Server={r['server_system_avg_cpu']:.1f}% (peak {r['server_system_peak_cpu']:.1f}%), Client={r['client_system_avg_cpu']:.1f}% (peak {r['client_system_peak_cpu']:.1f}%)")
        
        time.sleep(2)
    
    print("\n" + "=" * 70)
    print("Results Summary")
    print("=" * 70)
    
    sctp_c_avg_t = sum(sctp_results["client_throughput"]) / NUM_TESTS
    sctp_s_avg_t = sum(sctp_results["server_throughput"]) / NUM_TESTS
    quic_c_avg_t = sum(quic_results["client_throughput"]) / NUM_TESTS
    quic_s_avg_t = sum(quic_results["server_throughput"]) / NUM_TESTS
    
    sctp_s_sys_cpu = sum(sctp_results["server_system_cpu"]) / NUM_TESTS
    sctp_c_sys_cpu = sum(sctp_results["client_system_cpu"]) / NUM_TESTS
    quic_s_sys_cpu = sum(quic_results["server_system_cpu"]) / NUM_TESTS
    quic_c_sys_cpu = sum(quic_results["client_system_cpu"]) / NUM_TESTS
    
    sctp_s_sys_peak = sum(sctp_results["server_system_peak"]) / NUM_TESTS
    sctp_c_sys_peak = sum(sctp_results["client_system_peak"]) / NUM_TESTS
    quic_s_sys_peak = sum(quic_results["server_system_peak"]) / NUM_TESTS
    quic_c_sys_peak = sum(quic_results["client_system_peak"]) / NUM_TESTS
    
    sctp_total_sys_cpu = sctp_s_sys_cpu + sctp_c_sys_cpu
    quic_total_sys_cpu = quic_s_sys_cpu + quic_c_sys_cpu
    
    print(f"\n{'Metric':<25} {'SCTP':>15} {'QUIC':>15} {'Difference':>15}")
    print("-" * 70)
    print(f"{'Client Throughput':<25} {sctp_c_avg_t:>15.3f} {quic_c_avg_t:>15.3f} {sctp_c_avg_t-quic_c_avg_t:>15.3f} Gbps")
    print(f"{'Server Throughput':<25} {sctp_s_avg_t:>15.3f} {quic_s_avg_t:>15.3f} {sctp_s_avg_t-quic_s_avg_t:>15.3f} Gbps")
    print(f"{'Server System CPU':<25} {sctp_s_sys_cpu:>15.1f} {quic_s_sys_cpu:>15.1f} {quic_s_sys_cpu-sctp_s_sys_cpu:>15.1f}%")
    print(f"{'Client System CPU':<25} {sctp_c_sys_cpu:>15.1f} {quic_c_sys_cpu:>15.1f} {quic_c_sys_cpu-sctp_c_sys_cpu:>15.1f}%")
    print(f"{'Total System CPU':<25} {sctp_total_sys_cpu:>15.1f} {quic_total_sys_cpu:>15.1f} {quic_total_sys_cpu-sctp_total_sys_cpu:>15.1f}%")
    print(f"{'Server System Peak':<25} {sctp_s_sys_peak:>15.1f} {quic_s_sys_peak:>15.1f} {quic_s_sys_peak-sctp_s_sys_peak:>15.1f}%")
    print(f"{'Client System Peak':<25} {sctp_c_sys_peak:>15.1f} {quic_c_sys_peak:>15.1f} {quic_c_sys_peak-sctp_c_sys_peak:>15.1f}%")
    
    tests = list(range(1, NUM_TESTS + 1))
    
    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    
    ax1 = axes[0, 0]
    ax1.plot(tests, sctp_results["client_throughput"], 'b-o', label='SCTP Client', linewidth=2, markersize=8)
    ax1.plot(tests, quic_results["client_throughput"], 'r-s', label='QUIC Client', linewidth=2, markersize=8)
    ax1.axhline(y=sctp_c_avg_t, color='b', linestyle='--', alpha=0.5, label=f'SCTP Avg: {sctp_c_avg_t:.2f} Gbps')
    ax1.axhline(y=quic_c_avg_t, color='r', linestyle='--', alpha=0.5, label=f'QUIC Avg: {quic_c_avg_t:.2f} Gbps')
    ax1.set_xlabel('Test Number', fontsize=12)
    ax1.set_ylabel('Throughput (Gbps)', fontsize=12)
    ax1.set_title('Client Throughput Comparison', fontsize=14, fontweight='bold')
    ax1.legend(loc='best')
    ax1.grid(True, alpha=0.3)
    ax1.set_xticks(tests)
    
    ax2 = axes[0, 1]
    ax2.plot(tests, sctp_results["server_throughput"], 'b-o', label='SCTP Server', linewidth=2, markersize=8)
    ax2.plot(tests, quic_results["server_throughput"], 'r-s', label='QUIC Server', linewidth=2, markersize=8)
    ax2.axhline(y=sctp_s_avg_t, color='b', linestyle='--', alpha=0.5, label=f'SCTP Avg: {sctp_s_avg_t:.2f} Gbps')
    ax2.axhline(y=quic_s_avg_t, color='r', linestyle='--', alpha=0.5, label=f'QUIC Avg: {quic_s_avg_t:.2f} Gbps')
    ax2.set_xlabel('Test Number', fontsize=12)
    ax2.set_ylabel('Throughput (Gbps)', fontsize=12)
    ax2.set_title('Server Throughput Comparison', fontsize=14, fontweight='bold')
    ax2.legend(loc='best')
    ax2.grid(True, alpha=0.3)
    ax2.set_xticks(tests)
    
    ax3 = axes[1, 0]
    ax3.plot(tests, sctp_results["server_system_cpu"], 'b-o', label='SCTP Server', linewidth=2, markersize=8)
    ax3.plot(tests, quic_results["server_system_cpu"], 'r-s', label='QUIC Server', linewidth=2, markersize=8)
    ax3.axhline(y=sctp_s_sys_cpu, color='b', linestyle='--', alpha=0.5, label=f'SCTP Avg: {sctp_s_sys_cpu:.1f}%')
    ax3.axhline(y=quic_s_sys_cpu, color='r', linestyle='--', alpha=0.5, label=f'QUIC Avg: {quic_s_sys_cpu:.1f}%')
    ax3.set_xlabel('Test Number', fontsize=12)
    ax3.set_ylabel('System CPU (%)', fontsize=12)
    ax3.set_title('Server System CPU (User+Kernel)', fontsize=14, fontweight='bold')
    ax3.legend(loc='best')
    ax3.grid(True, alpha=0.3)
    ax3.set_xticks(tests)
    
    ax4 = axes[1, 1]
    ax4.plot(tests, sctp_results["client_system_cpu"], 'b-o', label='SCTP Client', linewidth=2, markersize=8)
    ax4.plot(tests, quic_results["client_system_cpu"], 'r-s', label='QUIC Client', linewidth=2, markersize=8)
    ax4.axhline(y=sctp_c_sys_cpu, color='b', linestyle='--', alpha=0.5, label=f'SCTP Avg: {sctp_c_sys_cpu:.1f}%')
    ax4.axhline(y=quic_c_sys_cpu, color='r', linestyle='--', alpha=0.5, label=f'QUIC Avg: {quic_c_sys_cpu:.1f}%')
    ax4.set_xlabel('Test Number', fontsize=12)
    ax4.set_ylabel('System CPU (%)', fontsize=12)
    ax4.set_title('Client System CPU (User+Kernel)', fontsize=14, fontweight='bold')
    ax4.legend(loc='best')
    ax4.grid(True, alpha=0.3)
    ax4.set_xticks(tests)
    
    plt.suptitle(f'SCTP vs QUIC Performance Comparison ({NUM_TESTS} Tests)\nThroughput and System CPU (User-Space + Kernel)', fontsize=16, fontweight='bold')
    plt.tight_layout()
    
    output_path = f"{SCRIPT_DIR}/results/performance_comparison.png"
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    print(f"\nChart saved to: {output_path}")
    
    with open(f"{SCRIPT_DIR}/results/test_data.txt", "w") as f:
        f.write("Test,SCTP_Client_Throughput_Gbps,SCTP_Server_Throughput_Gbps,QUIC_Client_Throughput_Gbps,QUIC_Server_Throughput_Gbps,")
        f.write("SCTP_Server_System_CPU_pct,SCTP_Client_System_CPU_pct,QUIC_Server_System_CPU_pct,QUIC_Client_System_CPU_pct\n")
        for i in range(NUM_TESTS):
            f.write(f"{i+1},{sctp_results['client_throughput'][i]:.3f},{sctp_results['server_throughput'][i]:.3f},")
            f.write(f"{quic_results['client_throughput'][i]:.3f},{quic_results['server_throughput'][i]:.3f},")
            f.write(f"{sctp_results['server_system_cpu'][i]:.1f},{sctp_results['client_system_cpu'][i]:.1f},")
            f.write(f"{quic_results['server_system_cpu'][i]:.1f},{quic_results['client_system_cpu'][i]:.1f}\n")
        f.write(f"\nAverage,{sctp_c_avg_t:.3f},{sctp_s_avg_t:.3f},{quic_c_avg_t:.3f},{quic_s_avg_t:.3f},")
        f.write(f"{sctp_s_sys_cpu:.1f},{sctp_c_sys_cpu:.1f},{quic_s_sys_cpu:.1f},{quic_c_sys_cpu:.1f}\n")
    
    print(f"Data saved to: {SCRIPT_DIR}/results/test_data.txt")
    
    print("\n" + "=" * 70)
    print("Key Observations")
    print("=" * 70)
    print(f"1. Throughput: SCTP client {sctp_c_avg_t:.2f} Gbps vs QUIC {quic_c_avg_t:.2f} Gbps")
    print(f"   - SCTP {'higher' if sctp_c_avg_t > quic_c_avg_t else 'lower'} by {abs(sctp_c_avg_t-quic_c_avg_t):.2f} Gbps ({abs(sctp_c_avg_t-quic_c_avg_t)/quic_c_avg_t*100:.1f}%)")
    print(f"2. Server System CPU: QUIC {quic_s_sys_cpu:.1f}% vs SCTP {sctp_s_sys_cpu:.1f}%")
    print(f"   - QUIC {'higher' if quic_s_sys_cpu > sctp_s_sys_cpu else 'lower'} by {abs(quic_s_sys_cpu-sctp_s_sys_cpu):.1f}% ({abs(quic_s_sys_cpu-sctp_s_sys_cpu)/sctp_s_sys_cpu*100 if sctp_s_sys_cpu > 0 else 0:.1f}%)")
    print(f"3. Client System CPU: QUIC {quic_c_sys_cpu:.1f}% vs SCTP {sctp_c_sys_cpu:.1f}%")
    print(f"   - QUIC {'higher' if quic_c_sys_cpu > sctp_c_sys_cpu else 'lower'} by {abs(quic_c_sys_cpu-sctp_c_sys_cpu):.1f}%")
    print(f"4. Total System CPU: QUIC {quic_total_sys_cpu:.1f}% vs SCTP {sctp_total_sys_cpu:.1f}%")
    print(f"   - QUIC uses {quic_total_sys_cpu/sctp_total_sys_cpu if sctp_total_sys_cpu > 0 else 0:.1f}x more system CPU resources")

if __name__ == "__main__":
    main()