#!/usr/bin/env python3
import subprocess
import time
import re
import matplotlib.pyplot as plt
import os
import signal

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DURATION = 3
NUM_TESTS = 5
TIMEOUT = 60

SCENARIOS = [
    {"name": "baseline", "delay": 0, "loss": 0},
    {"name": "delay_10ms", "delay": 10, "loss": 0},
    {"name": "delay_50ms", "delay": 50, "loss": 0},
    {"name": "loss_1%", "delay": 0, "loss": 1},
    {"name": "loss_5%", "delay": 0, "loss": 5},
    {"name": "delay_50ms_loss_1%", "delay": 50, "loss": 1},
]

def setup_network(delay_ms, loss_percent):
    subprocess.run(["tc", "qdisc", "del", "dev", "lo", "root"], capture_output=True)
    if delay_ms == 0 and loss_percent == 0:
        return
    cmd = ["tc", "qdisc", "add", "dev", "lo", "root", "handle", "1:", "netem"]
    if delay_ms > 0 and loss_percent > 0:
        cmd.extend(["delay", f"{delay_ms}ms", "loss", f"{loss_percent}%"])
    elif delay_ms > 0:
        cmd.extend(["delay", f"{delay_ms}ms"])
    elif loss_percent > 0:
        cmd.extend(["loss", f"{loss_percent}%"])
    subprocess.run(cmd, check=True)

def cleanup_network():
    subprocess.run(["tc", "qdisc", "del", "dev", "lo", "root"], capture_output=True)

def run_single_test(script):
    server_proc = subprocess.Popen(
        ["python3", "-u", f"{SCRIPT_DIR}/{script}", "server", "--duration", str(DURATION)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        preexec_fn=os.setsid
    )
    
    time.sleep(1)
    
    client_proc = subprocess.Popen(
        ["python3", "-u", f"{SCRIPT_DIR}/{script}", "client", "--duration", str(DURATION)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        preexec_fn=os.setsid
    )
    
    try:
        client_out, _ = client_proc.communicate(timeout=TIMEOUT)
        server_out, _ = server_proc.communicate(timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        os.killpg(os.getpgid(server_proc.pid), signal.SIGKILL)
        os.killpg(os.getpgid(client_proc.pid), signal.SIGKILL)
        return 0.0, 0.0
    
    client_throughput = 0.0
    server_throughput = 0.0
    
    match = re.search(r"Throughput:\s+([\d.]+)\s+Gbps", client_out)
    if match:
        client_throughput = float(match.group(1))
    
    match = re.search(r"Throughput:\s+([\d.]+)\s+Gbps", server_out)
    if match:
        server_throughput = float(match.group(1))
    
    return client_throughput, server_throughput

def test_scenario(scenario):
    print(f"\n  Setting up: delay={scenario['delay']}ms, loss={scenario['loss']}%")
    setup_network(scenario["delay"], scenario["loss"])
    time.sleep(0.5)
    
    sctp_results = {"client": [], "server": []}
    quic_results = {"client": [], "server": []}
    
    for i in range(NUM_TESTS):
        print(f"    Test {i+1}/{NUM_TESTS}...", end=" ", flush=True)
        
        c_t, s_t = run_single_test("sctp_bench.py")
        sctp_results["client"].append(c_t)
        sctp_results["server"].append(s_t)
        
        time.sleep(0.5)
        
        c_t, s_t = run_single_test("quic_bench.py")
        quic_results["client"].append(c_t)
        quic_results["server"].append(s_t)
        
        print(f"SCTP: {sctp_results['client'][-1]:.3f}/{sctp_results['server'][-1]:.3f}, QUIC: {quic_results['client'][-1]:.3f}/{quic_results['server'][-1]:.3f}")
        
        time.sleep(0.5)
    
    sctp_avg = {
        "client": sum(sctp_results["client"]) / NUM_TESTS,
        "server": sum(sctp_results["server"]) / NUM_TESTS
    }
    quic_avg = {
        "client": sum(quic_results["client"]) / NUM_TESTS,
        "server": sum(quic_results["server"]) / NUM_TESTS
    }
    
    cleanup_network()
    
    return sctp_avg, quic_avg

def main():
    print("=" * 70)
    print(f"Testing SCTP vs QUIC under different network conditions")
    print(f"Tests per scenario: {NUM_TESTS}, Duration: {DURATION}s each")
    print("=" * 70)
    
    all_results = {}
    
    for scenario in SCENARIOS:
        print(f"\n{'='*70}")
        print(f"Scenario: {scenario['name']} (delay={scenario['delay']}ms, loss={scenario['loss']}%)")
        print("=" * 70)
        
        sctp_avg, quic_avg = test_scenario(scenario)
        
        all_results[scenario["name"]] = {
            "sctp": sctp_avg,
            "quic": quic_avg,
            "delay": scenario["delay"],
            "loss": scenario["loss"]
        }
        
        print(f"\n  Results for {scenario['name']}:")
        print(f"    SCTP: Client={sctp_avg['client']:.3f} Gbps, Server={sctp_avg['server']:.3f} Gbps")
        print(f"    QUIC: Client={quic_avg['client']:.3f} Gbps, Server={quic_avg['server']:.3f} Gbps")
    
    print("\n" + "=" * 70)
    print("Generating charts...")
    print("=" * 70)
    
    scenarios = [s["name"] for s in SCENARIOS]
    sctp_client = [all_results[s]["sctp"]["client"] for s in scenarios]
    sctp_server = [all_results[s]["sctp"]["server"] for s in scenarios]
    quic_client = [all_results[s]["quic"]["client"] for s in scenarios]
    quic_server = [all_results[s]["quic"]["server"] for s in scenarios]
    
    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    
    x = range(len(scenarios))
    
    ax1 = axes[0, 0]
    ax1.plot(x, sctp_client, 'b-o', label='SCTP', linewidth=2, markersize=8)
    ax1.plot(x, quic_client, 'r-s', label='QUIC', linewidth=2, markersize=8)
    ax1.set_xlabel('Scenario', fontsize=11)
    ax1.set_ylabel('Throughput (Gbps)', fontsize=11)
    ax1.set_title('Client Throughput Comparison', fontsize=13, fontweight='bold')
    ax1.set_xticks(x)
    ax1.set_xticklabels(scenarios, rotation=45, ha='right', fontsize=9)
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    
    ax2 = axes[0, 1]
    ax2.plot(x, sctp_server, 'b-o', label='SCTP', linewidth=2, markersize=8)
    ax2.plot(x, quic_server, 'r-s', label='QUIC', linewidth=2, markersize=8)
    ax2.set_xlabel('Scenario', fontsize=11)
    ax2.set_ylabel('Throughput (Gbps)', fontsize=11)
    ax2.set_title('Server Throughput Comparison', fontsize=13, fontweight='bold')
    ax2.set_xticks(x)
    ax2.set_xticklabels(scenarios, rotation=45, ha='right', fontsize=9)
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    
    ax3 = axes[1, 0]
    width = 0.35
    bars1 = ax3.bar([i - width/2 for i in x], sctp_client, width, label='SCTP', color='steelblue')
    bars2 = ax3.bar([i + width/2 for i in x], quic_client, width, label='QUIC', color='indianred')
    ax3.set_xlabel('Scenario', fontsize=11)
    ax3.set_ylabel('Throughput (Gbps)', fontsize=11)
    ax3.set_title('Client Throughput (Bar Chart)', fontsize=13, fontweight='bold')
    ax3.set_xticks(x)
    ax3.set_xticklabels(scenarios, rotation=45, ha='right', fontsize=9)
    ax3.legend()
    ax3.grid(True, alpha=0.3, axis='y')
    
    ax4 = axes[1, 1]
    ax4.bar([i - width/2 for i in x], sctp_server, width, label='SCTP', color='steelblue')
    ax4.bar([i + width/2 for i in x], quic_server, width, label='QUIC', color='indianred')
    ax4.set_xlabel('Scenario', fontsize=11)
    ax4.set_ylabel('Throughput (Gbps)', fontsize=11)
    ax4.set_title('Server Throughput (Bar Chart)', fontsize=13, fontweight='bold')
    ax4.set_xticks(x)
    ax4.set_xticklabels(scenarios, rotation=45, ha='right', fontsize=9)
    ax4.legend()
    ax4.grid(True, alpha=0.3, axis='y')
    
    plt.suptitle('SCTP vs QUIC Performance under Network Perturbation\n(5 tests average per scenario)', 
                 fontsize=15, fontweight='bold')
    plt.tight_layout()
    
    output_path = f"{SCRIPT_DIR}/results/network_perturbation_comparison.png"
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    print(f"\nChart saved to: {output_path}")
    
    with open(f"{SCRIPT_DIR}/results/network_perturbation_data.txt", "w") as f:
        f.write("Scenario,Delay_ms,Loss_%,SCTP_Client_Gbps,SCTP_Server_Gbps,QUIC_Client_Gbps,QUIC_Server_Gbps\n")
        for name in scenarios:
            r = all_results[name]
            f.write(f"{name},{r['delay']},{r['loss']},{r['sctp']['client']:.4f},{r['sctp']['server']:.4f},{r['quic']['client']:.4f},{r['quic']['server']:.4f}\n")
    
    print(f"Data saved to: {SCRIPT_DIR}/results/network_perturbation_data.txt")
    
    print("\n" + "=" * 70)
    print("FINAL SUMMARY")
    print("=" * 70)
    print(f"{'Scenario':<25} {'SCTP Client':>12} {'SCTP Server':>12} {'QUIC Client':>12} {'QUIC Server':>12}")
    print("-" * 70)
    for name in scenarios:
        r = all_results[name]
        print(f"{name:<25} {r['sctp']['client']:>12.4f} {r['sctp']['server']:>12.4f} {r['quic']['client']:>12.4f} {r['quic']['server']:>12.4f}")

if __name__ == "__main__":
    try:
        main()
    finally:
        cleanup_network()