#!/usr/bin/env python3
import subprocess
import time
import re
import matplotlib.pyplot as plt
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DURATION = 5
NUM_TESTS = 10

def run_single_test(proto, script):
    server_proc = subprocess.Popen(
        ["python3", "-u", f"{SCRIPT_DIR}/{script}", "server", "--duration", str(DURATION)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    time.sleep(2)
    
    client_proc = subprocess.Popen(
        ["python3", "-u", f"{SCRIPT_DIR}/{script}", "client", "--duration", str(DURATION)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    client_out, _ = client_proc.communicate(timeout=DURATION + 30)
    server_out, _ = server_proc.communicate(timeout=30)
    
    client_throughput = 0.0
    server_throughput = 0.0
    
    match = re.search(r"Throughput:\s+([\d.]+)\s+Gbps", client_out)
    if match:
        client_throughput = float(match.group(1))
    
    match = re.search(r"Throughput:\s+([\d.]+)\s+Gbps", server_out)
    if match:
        server_throughput = float(match.group(1))
    
    return client_throughput, server_throughput

def main():
    print("=" * 60)
    print(f"Running {NUM_TESTS} tests per protocol (duration: {DURATION}s each)")
    print("=" * 60)
    
    sctp_results = {"client": [], "server": []}
    quic_results = {"client": [], "server": []}
    
    for i in range(NUM_TESTS):
        print(f"\n--- Test {i+1}/{NUM_TESTS} ---")
        
        print(f"[SCTP] Running test {i+1}...")
        c_t, s_t = run_single_test("SCTP", "sctp_bench.py")
        sctp_results["client"].append(c_t)
        sctp_results["server"].append(s_t)
        print(f"  Client: {c_t:.3f} Gbps, Server: {s_t:.3f} Gbps")
        
        time.sleep(2)
        
        print(f"[QUIC] Running test {i+1}...")
        c_t, s_t = run_single_test("QUIC", "quic_bench.py")
        quic_results["client"].append(c_t)
        quic_results["server"].append(s_t)
        print(f"  Client: {c_t:.3f} Gbps, Server: {s_t:.3f} Gbps")
        
        time.sleep(2)
    
    print("\n" + "=" * 60)
    print("Results Summary")
    print("=" * 60)
    
    sctp_c_avg = sum(sctp_results["client"]) / NUM_TESTS
    sctp_s_avg = sum(sctp_results["server"]) / NUM_TESTS
    quic_c_avg = sum(quic_results["client"]) / NUM_TESTS
    quic_s_avg = sum(quic_results["server"]) / NUM_TESTS
    
    print(f"\nSCTP Client Average: {sctp_c_avg:.3f} Gbps")
    print(f"SCTP Server Average: {sctp_s_avg:.3f} Gbps")
    print(f"QUIC Client Average: {quic_c_avg:.3f} Gbps")
    print(f"QUIC Server Average: {quic_s_avg:.3f} Gbps")
    
    tests = list(range(1, NUM_TESTS + 1))
    
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
    
    ax1.plot(tests, sctp_results["client"], 'b-o', label='SCTP Client', linewidth=2, markersize=8)
    ax1.plot(tests, quic_results["client"], 'r-s', label='QUIC Client', linewidth=2, markersize=8)
    ax1.axhline(y=sctp_c_avg, color='b', linestyle='--', alpha=0.5, label=f'SCTP Avg: {sctp_c_avg:.2f} Gbps')
    ax1.axhline(y=quic_c_avg, color='r', linestyle='--', alpha=0.5, label=f'QUIC Avg: {quic_c_avg:.2f} Gbps')
    ax1.set_xlabel('Test Number', fontsize=12)
    ax1.set_ylabel('Throughput (Gbps)', fontsize=12)
    ax1.set_title('Client Throughput Comparison', fontsize=14, fontweight='bold')
    ax1.legend(loc='best')
    ax1.grid(True, alpha=0.3)
    ax1.set_xticks(tests)
    
    ax2.plot(tests, sctp_results["server"], 'b-o', label='SCTP Server', linewidth=2, markersize=8)
    ax2.plot(tests, quic_results["server"], 'r-s', label='QUIC Server', linewidth=2, markersize=8)
    ax2.axhline(y=sctp_s_avg, color='b', linestyle='--', alpha=0.5, label=f'SCTP Avg: {sctp_s_avg:.2f} Gbps')
    ax2.axhline(y=quic_s_avg, color='r', linestyle='--', alpha=0.5, label=f'QUIC Avg: {quic_s_avg:.2f} Gbps')
    ax2.set_xlabel('Test Number', fontsize=12)
    ax2.set_ylabel('Throughput (Gbps)', fontsize=12)
    ax2.set_title('Server Throughput Comparison', fontsize=14, fontweight='bold')
    ax2.legend(loc='best')
    ax2.grid(True, alpha=0.3)
    ax2.set_xticks(tests)
    
    plt.suptitle('SCTP vs QUIC Performance Comparison (10 Tests)', fontsize=16, fontweight='bold')
    plt.tight_layout()
    
    output_path = f"{SCRIPT_DIR}/results/performance_comparison.png"
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    print(f"\nChart saved to: {output_path}")
    
    with open(f"{SCRIPT_DIR}/results/test_data.txt", "w") as f:
        f.write("Test,SCTP_Client,Gbps,SCTP_Server,Gbps,QUIC_Client,Gbps,QUIC_Server,Gbps\n")
        for i in range(NUM_TESTS):
            f.write(f"{i+1},{sctp_results['client'][i]:.3f},{sctp_results['server'][i]:.3f},{quic_results['client'][i]:.3f},{quic_results['server'][i]:.3f}\n")
        f.write(f"\nAverage,{sctp_c_avg:.3f},{sctp_s_avg:.3f},{quic_c_avg:.3f},{quic_s_avg:.3f}\n")
    
    print(f"Data saved to: {SCRIPT_DIR}/results/test_data.txt")

if __name__ == "__main__":
    main()