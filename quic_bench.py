#!/usr/bin/env python3
import asyncio
import ssl
import time
import argparse
import sys
from aioquic.asyncio import serve, connect
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import StreamDataReceived
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
import datetime

SERVER_PORT = 4433
DATA_SIZE = 64 * 1024

server_stats = {"total_received": 0, "start_time": None, "recv_start": None, "lock": None}

def generate_self_signed_cert():
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = issuer = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    cert = x509.CertificateBuilder().subject_name(subject).issuer_name(issuer).public_key(
        key.public_key()
    ).serial_number(x509.random_serial_number()).not_valid_before(
        datetime.datetime.utcnow()
    ).not_valid_after(
        datetime.datetime.utcnow() + datetime.timedelta(days=365)
    ).sign(key, hashes.SHA256())
    
    cert_path = "/tmp/quic_cert.pem"
    key_path = "/tmp/quic_key.pem"
    
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    with open(key_path, "wb") as f:
        f.write(key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.TraditionalOpenSSL,
            encryption_algorithm=serialization.NoEncryption()
        ))
    
    return cert_path, key_path

async def handle_stream(reader, writer):
    total = 0
    recv_start = None
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
            if recv_start is None:
                recv_start = time.time()
            total += len(data)
            if server_stats["lock"]:
                async with server_stats["lock"]:
                    server_stats["total_received"] += len(data)
                    if server_stats["recv_start"] is None:
                        server_stats["recv_start"] = time.time()
    except Exception:
        pass
    finally:
        writer.close()
    return total

def stream_handler(reader, writer):
    asyncio.create_task(handle_stream(reader, writer))

async def run_quic_server(duration: int):
    server_stats["lock"] = asyncio.Lock()
    server_stats["total_received"] = 0
    server_stats["start_time"] = time.time()
    server_stats["recv_start"] = None
    
    cert_path, key_path = generate_self_signed_cert()
    
    configuration = QuicConfiguration(is_client=False)
    configuration.load_cert_chain(cert_path, key_path)
    
    server = await serve("0.0.0.0", SERVER_PORT, configuration=configuration, stream_handler=stream_handler)
    print(f"[QUIC Server] Listening on 0.0.0.0:{SERVER_PORT}")
    sys.stdout.flush()
    
    await asyncio.sleep(duration + 5)
    
    if server_stats["recv_start"]:
        elapsed = time.time() - server_stats["recv_start"]
    else:
        elapsed = time.time() - server_stats["start_time"]
    
    total_mb = server_stats["total_received"] / 1_000_000
    gbps = (server_stats["total_received"] * 8) / (elapsed * 1_000_000_000)
    
    print("-" * 50)
    print("QUIC Server Results:")
    print(f"  Duration:       {elapsed:.2f}s")
    print(f"  Total received: {total_mb:.2f} MB")
    print(f"  Throughput:     {gbps:.2f} Gbps")
    print("-" * 50)
    sys.stdout.flush()
    
    server.close()

async def run_quic_client(duration: int, streams: int):
    configuration = QuicConfiguration(is_client=True)
    configuration.verify_mode = ssl.CERT_NONE
    
    async with connect("127.0.0.1", SERVER_PORT, configuration=configuration) as client:
        data = b'\x00' * DATA_SIZE
        start = time.time()
        end_time = start + duration
        actual_sent = 0
        
        while time.time() < end_time:
            client._quic.send_stream_data(0, data)
            actual_sent += len(data)
            await asyncio.sleep(0.002)
        
        client._quic.send_stream_data(0, b'', end_stream=True)
        
        await asyncio.sleep(3)
        
        elapsed = time.time() - start
        gbps = (actual_sent * 8) / (elapsed * 1_000_000_000)
        
        print("-" * 50)
        print("QUIC Client Results:")
        print(f"  Duration:       {elapsed:.2f}s")
        print(f"  Streams:        {streams}")
        print(f"  Total sent:     {actual_sent / 1_000_000:.2f} MB")
        print(f"  Throughput:     {gbps:.2f} Gbps")
        print("-" * 50)
        sys.stdout.flush()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="QUIC benchmark tool")
    parser.add_argument("mode", choices=["server", "client"])
    parser.add_argument("--duration", type=int, default=10)
    parser.add_argument("--streams", type=int, default=1)
    
    args = parser.parse_args()
    
    if args.mode == "server":
        asyncio.run(run_quic_server(args.duration))
    else:
        asyncio.run(run_quic_client(args.duration, args.streams))