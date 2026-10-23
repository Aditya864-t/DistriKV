#ifndef DISTRIKV_SOCKET_UTIL_H
#define DISTRIKV_SOCKET_UTIL_H

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <errno.h>

#include <string>
#include <memory>
#include <chrono>
#include <cstdint>

namespace distrikv {

/**
 * RAII Wrapper for a Berkeley Socket File Descriptor.
 * Automatically closes socket on destruction and prevents double-close via move-only semantics.
 */
class Socket {
public:
    Socket();
    explicit Socket(int fd);
    ~Socket();

    // Non-copyable
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Move-constructible and Move-assignable
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    int fd() const { return fd_; }
    bool is_valid() const { return fd_ >= 0; }
    void close();

    // Socket configuration helpers
    bool set_reuse_addr(bool enable = true);
    bool set_reuse_port(bool enable = true);
    bool set_nonblocking(bool enable = true);
    bool set_nodelay(bool enable = true);
    bool set_send_timeout(int timeout_ms);
    bool set_recv_timeout(int timeout_ms);

    // Release ownership of file descriptor
    int release();

private:
    int fd_;
};

/**
 * Socket utility helper functions for Berkeley Sockets syscalls.
 */
class SocketUtil {
public:
    // Create a TCP stream socket
    static int create_tcp_socket();

    // Create a UDP datagram socket
    static int create_udp_socket();

    // Send exactly 'length' bytes across the socket (handles partial sends & EINTR)
    static bool send_all(int fd, const void* data, size_t length);

    // Receive exactly 'length' bytes from socket (handles partial recvs & EINTR)
    static bool recv_all(int fd, void* buffer, size_t length);

    // Wait for socket to become readable using select() with a millisecond timeout
    // Returns 1 if readable, 0 if timeout, -1 if error.
    static int wait_readable(int fd, int timeout_ms);

    // Wait for socket to become writable using select() with a millisecond timeout
    static int wait_writable(int fd, int timeout_ms);

    // Resolve hostname/IP to sockaddr_in
    static bool resolve_address(const std::string& host, int port, sockaddr_in& addr);

    // Convert sockaddr_in to IP string and port
    static void sockaddr_to_ip_port(const sockaddr_in& addr, std::string& ip, int& port);
};

/**
 * TCP Server Listener for accepting client and cluster node connections.
 */
class TcpListener {
public:
    TcpListener();
    ~TcpListener();

    bool listen_on(const std::string& host, int port, int backlog = 128);
    std::unique_ptr<Socket> accept_connection(std::string* client_ip = nullptr, int* client_port = nullptr);

    int fd() const { return socket_.fd(); }
    bool is_valid() const { return socket_.is_valid(); }
    void close() { socket_.close(); }

private:
    Socket socket_;
    int port_;
    std::string host_;
};

/**
 * TCP Client Connection for connecting to cluster nodes.
 */
class TcpConnection {
public:
    TcpConnection();
    explicit TcpConnection(Socket socket);
    ~TcpConnection();

    // Connect to target host and port with configurable timeout
    bool connect_to(const std::string& host, int port, int timeout_ms = 3000);

    bool send_bytes(const void* data, size_t length);
    bool recv_bytes(void* buffer, size_t length);

    int fd() const { return socket_.fd(); }
    bool is_valid() const { return socket_.is_valid(); }
    void close() { socket_.close(); }
    Socket& socket() { return socket_; }

private:
    Socket socket_;
};

/**
 * UDP Datagram Socket for lightweight, fast peer heartbeats.
 */
class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    bool bind_to(const std::string& host, int port);
    ssize_t send_to(const void* data, size_t length, const std::string& dest_host, int dest_port);
    ssize_t recv_from(void* buffer, size_t max_length, std::string& src_host, int& src_port, int timeout_ms = 0);

    int fd() const { return socket_.fd(); }
    bool is_valid() const { return socket_.is_valid(); }
    void close() { socket_.close(); }

private:
    Socket socket_;
};

} // namespace distrikv

#endif // DISTRIKV_SOCKET_UTIL_H
