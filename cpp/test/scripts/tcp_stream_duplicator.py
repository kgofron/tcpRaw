#!/usr/bin/env python3
#
# Author: Kazimierz Gofron
#         Oak Ridge National Laboratory
#
# Created:  November 2, 2025
# Modified: November 4, 2025
#
"""
TCP Stream Duplicator
Listens on a source port and duplicates the stream to multiple destination ports.
This allows multiple tools to receive the same data stream simultaneously.
"""

import socket
import threading
import sys
import time
from typing import List, Tuple

class TCPStreamDuplicator:
    def __init__(self, source_host: str, source_port: int, dest_ports: List[int]):
        self.source_host = source_host
        self.source_port = source_port
        self.dest_ports = dest_ports
        self.dest_connections: List[socket.socket] = []
        self.running = False
        self.bytes_transferred = 0
        self.start_time = None
        
    def start(self):
        """Start the duplicator"""
        self.running = True
        self.start_time = time.time()
        
        # Connect to source
        print(f"Connecting to source: {self.source_host}:{self.source_port}")
        source_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        source_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        
        try:
            source_sock.connect((self.source_host, self.source_port))
            print(f"Connected to source")
        except Exception as e:
            print(f"Error connecting to source: {e}")
            sys.exit(1)
        
        # Connect to all destinations
        print(f"Connecting to {len(self.dest_ports)} destinations...")
        for port in self.dest_ports:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                sock.connect(('127.0.0.1', port))
                self.dest_connections.append(sock)
                print(f"  Connected to destination port {port}")
            except Exception as e:
                print(f"  Error connecting to destination port {port}: {e}")
                sock.close()
        
        if not self.dest_connections:
            print("Error: No destinations connected!")
            source_sock.close()
            sys.exit(1)
        
        print(f"\nDuplicating stream to {len(self.dest_connections)} destinations...")
        print("Press Ctrl+C to stop\n")
        
        # Duplicate stream
        buffer_size = 8192
        try:
            while self.running:
                try:
                    # Read from source
                    data = source_sock.recv(buffer_size)
                    if not data:
                        print("Source connection closed")
                        break
                    
                    self.bytes_transferred += len(data)
                    
                    # Write to all destinations
                    for dest_sock in self.dest_connections[:]:  # Copy list to allow modification
                        try:
                            dest_sock.sendall(data)
                        except Exception as e:
                            print(f"Error writing to destination: {e}")
                            self.dest_connections.remove(dest_sock)
                            dest_sock.close()
                    
                    # Periodically print statistics
                    elapsed = time.time() - self.start_time
                    if elapsed > 0 and int(elapsed) % 5 == 0:
                        rate_mbps = (self.bytes_transferred * 8.0) / (elapsed * 1e6)
                        print(f"[{int(elapsed)}s] {self.bytes_transferred:,} bytes, {rate_mbps:.2f} Mbps")
                        
                except KeyboardInterrupt:
                    print("\nStopping...")
                    self.running = False
                    break
                except Exception as e:
                    print(f"Error during transfer: {e}")
                    break
        finally:
            # Cleanup
            source_sock.close()
            for dest_sock in self.dest_connections:
                dest_sock.close()
            
            elapsed = time.time() - self.start_time
            if elapsed > 0:
                rate_mbps = (self.bytes_transferred * 8.0) / (elapsed * 1e6)
                print(f"\nFinal: {self.bytes_transferred:,} bytes in {elapsed:.2f}s, {rate_mbps:.2f} Mbps")

def main():
    if len(sys.argv) < 4:
        print("Usage: {} <source_host> <source_port> <dest_port1> [dest_port2] ...".format(sys.argv[0]))
        print("Example: {} 127.0.0.1 8085 8086 8087".format(sys.argv[0]))
        sys.exit(1)
    
    source_host = sys.argv[1]
    source_port = int(sys.argv[2])
    dest_ports = [int(p) for p in sys.argv[3:]]
    
    duplicator = TCPStreamDuplicator(source_host, source_port, dest_ports)
    duplicator.start()

if __name__ == '__main__':
    main()

