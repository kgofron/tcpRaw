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
    
private:
    const char* host_;
    uint16_t port_;
    int socket_;
    bool connected_;
    bool should_stop_;
    ConnectionCallback connection_cb_;
    
    void closeConnection();
    bool connect();
};

#endif // TCP_SERVER_H
