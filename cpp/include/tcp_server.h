#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <cstddef>
#include <cstdint>
#include <functional>

class TCPServer {
public:
    using DataCallback = std::function<void(const uint8_t* data, size_t size)>;
    using ConnectionCallback = std::function<void(bool connected)>;
    
    TCPServer(const char* host, uint16_t port);
    ~TCPServer();
    
    // Non-copyable, non-movable
    TCPServer(const TCPServer&) = delete;
    TCPServer& operator=(const TCPServer&) = delete;
    
    bool initialize();
    void run(DataCallback data_cb);
    void stop();
    
    bool isConnected() const { return connected_; }
    void setConnectionCallback(ConnectionCallback cb) { connection_cb_ = cb; }
    
    // Connection statistics
    struct ConnectionStats {
        uint64_t connection_attempts = 0;
        uint64_t successful_connections = 0;
        uint64_t disconnections = 0;
        uint64_t reconnect_errors = 0;
        uint64_t bytes_received = 0;
        uint64_t recv_errors = 0;
    };
    
    const ConnectionStats& getConnectionStats() const { return stats_; }
    void resetConnectionStats() { stats_ = ConnectionStats(); }
    
private:
    const char* host_;
    uint16_t port_;
    int socket_;
    bool connected_;
    bool should_stop_;
    ConnectionCallback connection_cb_;
    ConnectionStats stats_;
    
    void closeConnection();
    bool connect();
};

#endif // TCP_SERVER_H
