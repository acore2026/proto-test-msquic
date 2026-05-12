#!/usr/bin/env python3
import socket
import time
import argparse
import threading
import sys

SERVER_PORT = 4434
DATA_SIZE = 64 * 1024

def run_sctp_server(duration: int):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_SCTP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, DATA_SIZE * 256)
    sock.bind(('0.0.0.0', SERVER_PORT))
    sock.listen(5)
    print(f"[SCTP Server] Listening on 0.0.0.0:{SERVER_PORT}")
    sys.stdout.flush()
    
    sock.settimeout(duration + 5)
    total_received = [0]
    clients = []
    
    def handle_client(conn):
        try:
            while True:
                data = conn.recv(DATA_SIZE)
                if not data:
                    break
                total_received[0] += len(data)
        except Exception:
            pass
        finally:
            conn.close()
    
    start = time.time()
    while time.time() - start < duration + 3:
        try:
            conn, addr = sock.accept()
            clients.append(conn)
            t = threading.Thread(target=handle_client, args=(conn,))
            t.daemon = True
            t.start()
        except socket.timeout:
            break
        except Exception:
            pass
    
    elapsed = time.time() - start
    total_mb = total_received[0] / 1_000_000
    gbps = (total_received[0] * 8) / (elapsed * 1_000_000_000)
    
    print("-" * 50)
    print("SCTP Server Results:")
    print(f"  Duration:       {elapsed:.2f}s")
    print(f"  Total received: {total_mb:.2f} MB")
    print(f"  Throughput:     {gbps:.2f} Gbps")
    print("-" * 50)
    sys.stdout.flush()
    sock.close()

def run_sctp_client(duration: int, streams: int):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_SCTP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, DATA_SIZE * 256)
    sock.connect(('127.0.0.1', SERVER_PORT))
    
    data = b'\x00' * DATA_SIZE
    total_bytes = 0
    start = time.time()
    end_time = start + duration
    
    while time.time() < end_time:
        try:
            sent = sock.send(data)
            total_bytes += sent
        except Exception as e:
            break
    
    elapsed = time.time() - start
    gbps = (total_bytes * 8) / (elapsed * 1_000_000_000)
    
    sock.close()
    
    print("-" * 50)
    print("SCTP Client Results:")
    print(f"  Duration:       {elapsed:.2f}s")
    print(f"  Streams:        {streams}")
    print(f"  Total sent:     {total_bytes / 1_000_000:.2f} MB")
    print(f"  Throughput:     {gbps:.2f} Gbps")
    print("-" * 50)
    sys.stdout.flush()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="SCTP kernel benchmark")
    parser.add_argument("mode", choices=["server", "client"])
    parser.add_argument("--duration", type=int, default=10)
    parser.add_argument("--streams", type=int, default=1)
    
    args = parser.parse_args()
    
    if args.mode == "server":
        run_sctp_server(args.duration)
    else:
        run_sctp_client(args.duration, args.streams)
