/*
 * Author: Kazimierz Gofron
 *         Oak Ridge National Laboratory
 *
 * Created:  November 2, 2025
 * Modified: November 4, 2025
 */

#include "tcp_server.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <ctime>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <sstream>

TCPServer::TCPServer(const char* host, uint16_t port)
    : host_(host), port_(port), socket_(-1), 
      connected_(false), should_stop_(false), stats_(),
      incomplete_buffer_size_(0)
{
}

TCPServer::~TCPServer() {
    stop();
}

bool TCPServer::initialize() {
    // No initialization needed for client mode
    // Connection will be established in run()
    return true;
}

void TCPServer::closeConnection() {
    // Clear incomplete buffer on disconnect
    if (incomplete_buffer_size_ > 0) {
        stats_.bytes_dropped_incomplete += incomplete_buffer_size_;
        incomplete_buffer_size_ = 0;
    }
    
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
    if (connected_) {
        stats_.disconnections++;
        
        // Log disconnection with timestamp
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        
        std::cout << "[TCP] Disconnected at " 
                  << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
                  << "." << std::setfill('0') << std::setw(3) << ms.count()
                  << " (Total disconnections: " << stats_.disconnections << ")" << std::endl;
        
        if (connection_cb_) {
            connected_ = false;
            connection_cb_(false);
        } else {
            connected_ = false;
        }
    } else {
        connected_ = false;
    }
}

bool TCPServer::connect() {
    stats_.connection_attempts++;
    
    // Create socket
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
        stats_.reconnect_errors++;
        return false;
    }
    
    // Set socket options for reliability
    int opt = 1;
    
    // Enable TCP keepalive to detect dead connections and keep connection alive
    if (setsockopt(socket_, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) < 0) {
        close(socket_);
        socket_ = -1;
        return false;
    }
    
    // Configure TCP keepalive parameters (Linux-specific)
    // Keepalive probe interval: 5 seconds
    int keepidle = 5;  // Start sending keepalive probes after 5 seconds of idle
    int keepintvl = 5; // Send keepalive probes every 5 seconds
    int keepcnt = 3;   // Send 3 probes before considering connection dead
    
    if (setsockopt(socket_, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) < 0) {
        // Not critical, continue if fails (some systems may not support this)
    }
    if (setsockopt(socket_, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl)) < 0) {
        // Not critical, continue if fails
    }
    if (setsockopt(socket_, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt)) < 0) {
        // Not critical, continue if fails
    }
    
    // Disable Nagle's algorithm for low latency
    if (setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        close(socket_);
        socket_ = -1;
        return false;
    }
    
    // Set receive buffer size for better throughput
    // At 8 MHz (64 MB/s), we need large buffers to avoid overruns
    // 64MB provides best performance (51% capture vs 24% at 1MB, 27% at 128MB)
    int rcvbuf = 64 * 1024 * 1024; // 64MB requested
    if (setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        // Not critical, continue if fails
    }
    
    // Verify actual buffer size (may be clamped by system limits)
    // Note: Linux doubles the buffer size, so requested 64MB -> actual 50MB
    // due to system rmem_max limit of 25MB
    // (getsockopt returns doubled value, actual allocated is half)
    
    // Set up server address
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    
    if (inet_pton(AF_INET, host_, &addr.sin_addr) <= 0) {
        close(socket_);
        socket_ = -1;
        return false;
    }
    
    // Connect to server
    if (::connect(socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        stats_.reconnect_errors++;
        close(socket_);
        socket_ = -1;
        return false;
    }
    
    stats_.successful_connections++;
    connected_ = true;
    
    // Log successful connection with timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::cout << "[TCP] Connected at " 
              << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
              << "." << std::setfill('0') << std::setw(3) << ms.count()
              << " (Attempt " << stats_.connection_attempts 
              << ", Success " << stats_.successful_connections << ")" << std::endl;
    
    if (connection_cb_) {
        connection_cb_(true);
    }
    
    return true;
}

void TCPServer::run(DataCallback data_cb) {
    should_stop_ = false;
    
    while (!should_stop_) {
        // Try to connect
        if (!connect()) {
            // Connection failed, wait a bit before retrying
            // Use shorter sleep (100ms) to connect faster when server becomes available
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 100000000; // 100ms
            nanosleep(&ts, nullptr);
            continue;
        }
        
        // Connected, now read data
        constexpr size_t BUFFER_SIZE = 8192; // 8KB buffer
        uint8_t buffer[BUFFER_SIZE + 8];  // Extra space for incomplete bytes
        
        while (connected_ && !should_stop_) {
            // First, copy any incomplete bytes from previous recv() to start of buffer
            size_t bytes_to_process = incomplete_buffer_size_;
            if (incomplete_buffer_size_ > 0) {
                std::memcpy(buffer, incomplete_buffer_, incomplete_buffer_size_);
            }
            
            // Read new data after the incomplete bytes
            size_t bytes_to_read = BUFFER_SIZE;
            ssize_t bytes_read = recv(socket_, buffer + incomplete_buffer_size_, bytes_to_read, 0);
            
            if (bytes_read == 0) {
                // Connection closed by peer
                if (incomplete_buffer_size_ > 0) {
                    std::cout << "[TCP] WARNING: Connection closed with " 
                              << incomplete_buffer_size_ << " incomplete bytes in buffer" << std::endl;
                    stats_.bytes_dropped_incomplete += incomplete_buffer_size_;
                }
                std::cout << "[TCP] Connection closed by peer (EOF)" << std::endl;
                closeConnection();
                incomplete_buffer_size_ = 0;
                break;
            } else if (bytes_read < 0) {
                // Check for recoverable errors
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // No data available, continue
                    continue;
                } else if (errno == EINTR) {
                    // Interrupted, continue
                    continue;
                } else {
                    // Serious error, close connection
                    stats_.recv_errors++;
                    std::cout << "[TCP] recv() error: " << strerror(errno) 
                              << " (errno=" << errno << ")" << std::endl;
                    if (incomplete_buffer_size_ > 0) {
                        stats_.bytes_dropped_incomplete += incomplete_buffer_size_;
                        incomplete_buffer_size_ = 0;
                    }
                    closeConnection();
                    break;
                }
            } else {
                // Successfully received data
                stats_.bytes_received += bytes_read;
                bytes_to_process += bytes_read;  // Total bytes to process
            }
            
            // Process complete 8-byte words
            size_t complete_words = (bytes_to_process / 8) * 8;
            if (complete_words > 0 && data_cb) {
                data_cb(buffer, complete_words);
            }
            
            // Save incomplete bytes for next recv() call
            size_t incomplete_bytes = bytes_to_process % 8;
            if (incomplete_bytes > 0) {
                std::memcpy(incomplete_buffer_, buffer + complete_words, incomplete_bytes);
                incomplete_buffer_size_ = incomplete_bytes;
            } else {
                incomplete_buffer_size_ = 0;
            }
        }
        
        // Note: closeConnection() is already called before break above
        // No need to call it here again
    }
}

void TCPServer::stop() {
    should_stop_ = true;
    closeConnection();
}
