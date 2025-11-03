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

TCPServer::TCPServer(const char* host, uint16_t port)
    : host_(host), port_(port), socket_(-1), 
      connected_(false), should_stop_(false)
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
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
    if (connected_ && connection_cb_) {
        connected_ = false;
        connection_cb_(false);
    } else {
        connected_ = false;
    }
}

bool TCPServer::connect() {
    // Create socket
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
        return false;
    }
    
    // Set socket options for reliability
    int opt = 1;
    
    // Enable TCP keepalive to detect dead connections
    if (setsockopt(socket_, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) < 0) {
        close(socket_);
        socket_ = -1;
        return false;
    }
    
    // Disable Nagle's algorithm for low latency
    if (setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        close(socket_);
        socket_ = -1;
        return false;
    }
    
    // Set receive buffer size for better throughput
    // At 8 MHz (64 MB/s), we need large buffers to avoid overruns
    // 64MB works best (requested) vs 128MB (system may reject or mishandle)
    int rcvbuf = 64 * 1024 * 1024; // 64MB requested
    if (setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        // Not critical, continue if fails
    }
    
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
        close(socket_);
        socket_ = -1;
        return false;
    }
    
    connected_ = true;
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
        uint8_t buffer[BUFFER_SIZE];
        
        while (connected_ && !should_stop_) {
            // Read as much as possible, but ensure we read in multiples of 8
            size_t bytes_to_read = BUFFER_SIZE;
            
            ssize_t bytes_read = recv(socket_, buffer, bytes_to_read, 0);
            
            if (bytes_read == 0) {
                // Connection closed by peer
                closeConnection();
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
                    closeConnection();
                    break;
                }
            }
            
            // Only process complete 8-byte words
            size_t complete_words = (bytes_read / 8) * 8;
            if (complete_words > 0 && data_cb) {
                data_cb(buffer, complete_words);
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
