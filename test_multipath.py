#!/usr/bin/env python3
import subprocess
import time
import re
import matplotlib.pyplot as plt
import os
import signal
import threading

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DURATION = 5
NUM_TESTS = 3
TIMEOUT = 30

SCENARIOS = [
    {"name": "stable", "description": "Stable Path (Baseline)", "switch_interval": 0},
    {"name": "switch_2s", "description": "Switch every 2s", "switch_interval": 2},
    {"name": "switch_1s", "description": "Switch every 1s", "switch_interval": 1},
    {"name": "switch_0.5s", "description": "Switch every 0.5s", "switch_interval": 0.5},
    {"name": "switch_0.2s", "description": "Switch every 0.2s", "switch_interval": 0.2},
]

PATH_CONFIGS = [
    {"delay": 10, "loss": 0.5},
    {"delay": 25, "loss": 1},
]

stop_switcher = False
current_path = 0

def set_network_path(config):
    subprocess.run(["tc", "qdisc", "del", "dev", "lo", "root"], capture_output=True)
    delay = config["delay"]
    loss = config["loss"]
    cmd = ["tc", "qdisc", "add", "dev", "lo", "root", "handle", "1:", "netem", "delay", f"{delay}ms", "loss", f"{loss}%"]
    subprocess.run(cmd, capture_output=True)

def path_switcher_thread(interval):
    global current_path, stop_switcher
    while not stop_switcher:
        current_path = (current_path + 1) % len(PATH_CONFIGS)
        set_network_path(PATH_CONFIGS[current_path])
        time.sleep(interval)

def setup_stable():
    subprocess.run(["tc", "qdisc", "del", "dev", "lo", "root"], capture_output=True)

def cleanup_network():
    global stop_switcher
    stop_switcher = True
    time.sleep(0.5)
    subprocess.run(["tc", "qdisc", "del", "dev", "lo", "root"], capture_output=True)
    stop_switcher = False

def run_single_test(script, use_switcher=False, switch_interval=0):
    global stop_switcher
    
    switcher = None
    if use_switcher and switch_interval > 0:
        stop_switcher = False
        current_path_idx = 0
        set_network_path(PATH_CONFIGS[current_path_idx])
        switcher = threading.Thread(target=path_switcher_thread, args=(switch_interval,))
        switcher.daemon = True
        switcher.start()
    else:
        setup_stable()
    
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
        if switcher:
            stop_switcher = True
        return 0.0, 0.0
    
    if switcher:
        stop_switcher = True
        switcher.join(timeout=1)
    
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
    print(f"\n  {scenario['description']}")
    
    use_switcher = scenario["switch_interval"] > 0
    switch_interval = scenario["switch_interval"]
    
    sctp_results = {"client": [], "server": []}
    quic_results = {"client": [], "server": []}
    
    for i in range(NUM_TESTS):
        print(f"    Test {i+1}/{NUM_TESTS}...", end=" ", flush=True)
        
        c_t, s_t = run_single_test("sctp_bench.py", use_switcher, switch_interval)
        sctp_results["client"].append(c_t)
        sctp_results["server"].append(s_t)
        
        time.sleep(1)
        
        c_t, s_t = run_single_test("quic_bench.py", use_switcher, switch_interval)
        quic_results["client"].append(c_t)
        quic_results["server"].append(s_t)
        
        print(f"SCTP: {sctp_results['client'][-1]:.3f}/{sctp_results['server'][-1]:.3f}, QUIC: {quic_results['client'][-1]:.3f}/{quic_results['server'][-1]:.3f}")
        
        time.sleep(1)
    
    cleanup_network()
    
    sctp_avg = {
        "client": sum(sctp_results["client"]) / NUM_TESTS,
        "server": sum(sctp_results["server"]) / NUM_TESTS
    }
    quic_avg = {
        "client": sum(quic_results["client"]) / NUM_TESTS,
        "server": sum(quic_results["server"]) / NUM_TESTS
    }
    
    return sctp_avg, quic_avg

def main():
    print("=" * 70)
    print(f"Testing SCTP vs QUIC under multipath frequent switching")
    print(f"Tests per scenario: {NUM_TESTS}, Duration: {DURATION}s each")
    print(f"Path configurations: {len(PATH_CONFIGS)} paths")
    for i, cfg in enumerate(PATH_CONFIGS):
        print(f"  Path {i+1}: delay={cfg['delay']}ms, loss={cfg['loss']}%")
    print("=" * 70)
    
    all_results = {}
    
    for scenario in SCENARIOS:
        print(f"\n{'='*70}")
        print(f"Scenario: {scenario['name']}")
        print("=" * 70)
        
        sctp_avg, quic_avg = test_scenario(scenario)
        
        all_results[scenario["name"]] = {
            "sctp": sctp_avg,
            "quic": quic_avg,
            "description": scenario["description"],
            "switch_interval": scenario["switch_interval"]
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
    labels = [all_results[s]["description"] for s in scenarios]
    
    ax1 = axes[0, 0]
    ax1.plot(x, sctp_client, 'b-o', label='SCTP', linewidth=2, markersize=10)
    ax1.plot(x, quic_client, 'r-s', label='QUIC', linewidth=2, markersize=10)
    ax1.set_xlabel('Scenario', fontsize=12)
    ax1.set_ylabel('Throughput (Gbps)', fontsize=12)
    ax1.set_title('Client Throughput Comparison', fontsize=14, fontweight='bold')
    ax1.set_xticks(x)
    ax1.set_xticklabels(labels, rotation=30, ha='right', fontsize=10)
    ax1.legend(fontsize=11)
    ax1.grid(True, alpha=0.3)
    
    ax2 = axes[0, 1]
    ax2.plot(x, sctp_server, 'b-o', label='SCTP', linewidth=2, markersize=10)
    ax2.plot(x, quic_server, 'r-s', label='QUIC', linewidth=2, markersize=10)
    ax2.set_xlabel('Scenario', fontsize=12)
    ax2.set_ylabel('Throughput (Gbps)', fontsize=12)
    ax2.set_title('Server Throughput Comparison', fontsize=14, fontweight='bold')
    ax2.set_xticks(x)
    ax2.set_xticklabels(labels, rotation=30, ha='right', fontsize=10)
    ax2.legend(fontsize=11)
    ax2.grid(True, alpha=0.3)
    
    ax3 = axes[1, 0]
    width = 0.35
    ax3.bar([i - width/2 for i in x], sctp_client, width, label='SCTP', color='steelblue')
    ax3.bar([i + width/2 for i in x], quic_client, width, label='QUIC', color='indianred')
    ax3.set_xlabel('Scenario', fontsize=12)
    ax3.set_ylabel('Throughput (Gbps)', fontsize=12)
    ax3.set_title('Client Throughput (Bar Chart)', fontsize=14, fontweight='bold')
    ax3.set_xticks(x)
    ax3.set_xticklabels(labels, rotation=30, ha='right', fontsize=10)
    ax3.legend(fontsize=11)
    ax3.grid(True, alpha=0.3, axis='y')
    
    ax4 = axes[1, 1]
    ax4.bar([i - width/2 for i in x], sctp_server, width, label='SCTP', color='steelblue')
    ax4.bar([i + width/2 for i in x], quic_server, width, label='QUIC', color='indianred')
    ax4.set_xlabel('Scenario', fontsize=12)
    ax4.set_ylabel('Throughput (Gbps)', fontsize=12)
    ax4.set_title('Server Throughput (Bar Chart)', fontsize=14, fontweight='bold')
    ax4.set_xticks(x)
    ax4.set_xticklabels(labels, rotation=30, ha='right', fontsize=10)
    ax4.legend(fontsize=11)
    ax4.grid(True, alpha=0.3, axis='y')
    
    plt.suptitle('SCTP vs QUIC Performance under Multipath Frequent Switching\n(3 tests average per scenario)', 
                 fontsize=16, fontweight='bold')
    plt.tight_layout()
    
    output_path = f"{SCRIPT_DIR}/results/multipath_switching_comparison.png"
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    print(f"\nChart saved to: {output_path}")
    
    with open(f"{SCRIPT_DIR}/results/multipath_switching_data.txt", "w") as f:
        f.write("Scenario,Description,Switch_Interval_s,SCTP_Client_Gbps,SCTP_Server_Gbps,QUIC_Client_Gbps,QUIC_Server_Gbps\n")
        for name in scenarios:
            r = all_results[name]
            f.write(f"{name},{r['description']},{r['switch_interval']},{r['sctp']['client']:.4f},{r['sctp']['server']:.4f},{r['quic']['client']:.4f},{r['quic']['server']:.4f}\n")
    
    print(f"Data saved to: {SCRIPT_DIR}/results/multipath_switching_data.txt")
    
    print("\n" + "=" * 70)
    print("FINAL SUMMARY")
    print("=" * 70)
    print(f"{'Scenario':<15} {'Description':<25} {'SCTP Client':>12} {'SCTP Server':>12} {'QUIC Client':>12} {'QUIC Server':>12}")
    print("-" * 90)
    for name in scenarios:
        r = all_results[name]
        print(f"{name:<15} {r['description']:<25} {r['sctp']['client']:>12.4f} {r['sctp']['server']:>12.4f} {r['quic']['client']:>12.4f} {r['quic']['server']:>12.4f}")
    
    print("\n" + "=" * 70)
    print("Performance Ratio (QUIC / SCTP)")
    print("=" * 70)
    print(f"{'Scenario':<15} {'Client Ratio':>15} {'Server Ratio':>15}")
    print("-" * 50)
    for name in scenarios:
        r = all_results[name]
        client_ratio = r['quic']['client'] / r['sctp']['client'] if r['sctp']['client'] > 0 else float('inf')
        server_ratio = r['quic']['server'] / r['sctp']['server'] if r['sctp']['server'] > 0 else float('inf')
        print(f"{name:<15} {client_ratio:>15.2f}x {server_ratio:>15.2f}x")

if __name__ == "__main__":
    try:
        main()
    finally:
        cleanup_network()