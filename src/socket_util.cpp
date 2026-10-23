#include "socket_util.h"
#include <iostream>
#include <cstring>
#include <chrono>

namespace distrikv {

// ==========================================
// Socket RAII Implementation
// ==========================================

Socket::Socket() : fd_(-1) {}

Socket::Socket(int fd) : fd_(fd) {}

Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void Socket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

int Socket::release() {
    int old_fd = fd_;
    fd_ = -1;
    return old_fd;
}

bool Socket::set_reuse_addr(bool enable) {
    if (fd_ < 0) return false;
    int opt = enable ? 1 : 0;
    return ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0;
}

bool Socket::set_reuse_port(bool enable) {
    if (fd_ < 0) return false;
#ifdef SO_REUSEPORT
    int opt = enable ? 1 : 0;
    return ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == 0;
#else
    (void)enable;
    return true;
#endif
}

bool Socket::set_nonblocking(bool enable) {
    if (fd_ < 0) return false;
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags == -1) return false;
    if (enable) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return ::fcntl(fd_, F_SETFL, flags) == 0;
}

bool Socket::set_nodelay(bool enable) {
    if (fd_ < 0) return false;
    int opt = enable ? 1 : 0;
    return ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == 0;
}

bool Socket::set_send_timeout(int timeout_ms) {
    if (fd_ < 0) return false;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

bool Socket::set_recv_timeout(int timeout_ms) {
    if (fd_ < 0) return false;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

// ==========================================
// SocketUtil Implementation
// ==========================================

int SocketUtil::create_tcp_socket() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "[SocketUtil] socket(SOCK_STREAM) error: " << std::strerror(errno) << "\n";
    }
    return fd;
}

int SocketUtil::create_udp_socket() {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::cerr << "[SocketUtil] socket(SOCK_DGRAM) error: " << std::strerror(errno) << "\n";
    }
    return fd;
}

bool SocketUtil::send_all(int fd, const void* data, size_t length) {
    const char* ptr = static_cast<const char*>(data);
    size_t total_sent = 0;

    while (total_sent < length) {
        ssize_t sent = ::send(fd, ptr + total_sent, length - total_sent, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue; // Interrupted by signal, retry
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Wait for socket to become writable
                int res = wait_writable(fd, 2000);
                if (res > 0) continue;
                return false;
            }
            return false;
        }
        if (sent == 0) {
            return false; // Connection closed
        }
        total_sent += static_cast<size_t>(sent);
    }
    return true;
}

bool SocketUtil::recv_all(int fd, void* buffer, size_t length) {
    char* ptr = static_cast<char*>(buffer);
    size_t total_read = 0;

    while (total_read < length) {
        ssize_t n = ::recv(fd, ptr + total_read, length - total_read, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                int res = wait_readable(fd, 2000);
                if (res > 0) continue;
                return false;
            }
            return false;
        }
        if (n == 0) {
            return false; // Peer closed connection
        }
        total_read += static_cast<size_t>(n);
    }
    return true;
}

int SocketUtil::wait_readable(int fd, int timeout_ms) {
    if (fd < 0) return -1;
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);

    struct timeval tv;
    struct timeval* ptv = nullptr;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }

    int result = ::select(fd + 1, &read_fds, nullptr, nullptr, ptv);
    if (result < 0 && errno == EINTR) {
        return 0; // Treat as timeout on interrupted
    }
    return result;
}

int SocketUtil::wait_writable(int fd, int timeout_ms) {
    if (fd < 0) return -1;
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(fd, &write_fds);

    struct timeval tv;
    struct timeval* ptv = nullptr;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }

    int result = ::select(fd + 1, nullptr, &write_fds, nullptr, ptv);
    if (result < 0 && errno == EINTR) {
        return 0;
    }
    return result;
}

bool SocketUtil::resolve_address(const std::string& host, int port, sockaddr_in& addr) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        return true;
    }

    // Try direct IPv4 parsing
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1) {
        return true;
    }

    // Fallback to DNS resolution using getaddrinfo
    struct addrinfo hints;
    struct addrinfo* res = nullptr;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int status = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (status != 0 || res == nullptr) {
        return false;
    }

    struct sockaddr_in* ipv4 = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
    addr.sin_addr = ipv4->sin_addr;
    ::freeaddrinfo(res);
    return true;
}

void SocketUtil::sockaddr_to_ip_port(const sockaddr_in& addr, std::string& ip, int& port) {
    char ip_buf[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &(addr.sin_addr), ip_buf, INET_ADDRSTRLEN);
    ip = ip_buf;
    port = ntohs(addr.sin_port);
}

// ==========================================
// TcpListener Implementation
// ==========================================

TcpListener::TcpListener() : port_(0) {}

TcpListener::~TcpListener() {
    close();
}

bool TcpListener::listen_on(const std::string& host, int port, int backlog) {
    close();
    host_ = host;
    port_ = port;

    int raw_fd = SocketUtil::create_tcp_socket();
    if (raw_fd < 0) return false;
    socket_ = Socket(raw_fd);

    if (!socket_.set_reuse_addr(true)) {
        std::cerr << "[TcpListener] Warning: set_reuse_addr failed: " << std::strerror(errno) << "\n";
    }
    socket_.set_reuse_port(true);

    sockaddr_in server_addr;
    if (!SocketUtil::resolve_address(host, port, server_addr)) {
        std::cerr << "[TcpListener] Failed to resolve host: " << host << "\n";
        close();
        return false;
    }

    if (::bind(socket_.fd(), reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::cerr << "[TcpListener] bind() failed on " << host << ":" << port << ": " << std::strerror(errno) << "\n";
        close();
        return false;
    }

    if (::listen(socket_.fd(), backlog) < 0) {
        std::cerr << "[TcpListener] listen() failed: " << std::strerror(errno) << "\n";
        close();
        return false;
    }

    return true;
}

std::unique_ptr<Socket> TcpListener::accept_connection(std::string* client_ip, int* client_port) {
    if (!socket_.is_valid()) return nullptr;

    sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = ::accept(socket_.fd(), reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);

    if (client_fd < 0) {
        return nullptr;
    }

    if (client_ip && client_port) {
        SocketUtil::sockaddr_to_ip_port(client_addr, *client_ip, *client_port);
    }

    auto client_sock = std::make_unique<Socket>(client_fd);
    client_sock->set_nodelay(true);
    return client_sock;
}

// ==========================================
// TcpConnection Implementation
// ==========================================

TcpConnection::TcpConnection() {}

TcpConnection::TcpConnection(Socket socket) : socket_(std::move(socket)) {}

TcpConnection::~TcpConnection() {
    close();
}

bool TcpConnection::connect_to(const std::string& host, int port, int timeout_ms) {
    close();

    int raw_fd = SocketUtil::create_tcp_socket();
    if (raw_fd < 0) return false;
    socket_ = Socket(raw_fd);

    socket_.set_nodelay(true);

    sockaddr_in target_addr;
    if (!SocketUtil::resolve_address(host, port, target_addr)) {
        close();
        return false;
    }

    // Set non-blocking to honor timeout
    if (!socket_.set_nonblocking(true)) {
        close();
        return false;
    }

    int rc = ::connect(socket_.fd(), reinterpret_cast<struct sockaddr*>(&target_addr), sizeof(target_addr));
    if (rc == 0) {
        // Connected immediately
        socket_.set_nonblocking(false);
        return true;
    }

    if (rc < 0 && errno != EINPROGRESS) {
        close();
        return false;
    }

    // Wait for connection to establish using select()
    int sel_rc = SocketUtil::wait_writable(socket_.fd(), timeout_ms);
    if (sel_rc <= 0) {
        // Timeout or error
        close();
        return false;
    }

    // Check SO_ERROR
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (::getsockopt(socket_.fd(), SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
        close();
        return false;
    }

    socket_.set_nonblocking(false);
    return true;
}

bool TcpConnection::send_bytes(const void* data, size_t length) {
    if (!socket_.is_valid()) return false;
    return SocketUtil::send_all(socket_.fd(), data, length);
}

bool TcpConnection::recv_bytes(void* buffer, size_t length) {
    if (!socket_.is_valid()) return false;
    return SocketUtil::recv_all(socket_.fd(), buffer, length);
}

// ==========================================
// UdpSocket Implementation
// ==========================================

UdpSocket::UdpSocket() {}

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::bind_to(const std::string& host, int port) {
    close();
    int raw_fd = SocketUtil::create_udp_socket();
    if (raw_fd < 0) return false;
    socket_ = Socket(raw_fd);

    socket_.set_reuse_addr(true);
    socket_.set_reuse_port(true);

    sockaddr_in bind_addr;
    if (!SocketUtil::resolve_address(host, port, bind_addr)) {
        close();
        return false;
    }

    if (::bind(socket_.fd(), reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
        std::cerr << "[UdpSocket] bind() failed on " << host << ":" << port << ": " << std::strerror(errno) << "\n";
        close();
        return false;
    }

    return true;
}

ssize_t UdpSocket::send_to(const void* data, size_t length, const std::string& dest_host, int dest_port) {
    if (!socket_.is_valid()) return -1;

    sockaddr_in dest_addr;
    if (!SocketUtil::resolve_address(dest_host, dest_port, dest_addr)) {
        return -1;
    }

    return ::sendto(socket_.fd(), data, length, 0,
                    reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
}

ssize_t UdpSocket::recv_from(void* buffer, size_t max_length, std::string& src_host, int& src_port, int timeout_ms) {
    if (!socket_.is_valid()) return -1;

    if (timeout_ms > 0) {
        int res = SocketUtil::wait_readable(socket_.fd(), timeout_ms);
        if (res <= 0) {
            return -1; // Timeout or error
        }
    }

    sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);
    ssize_t bytes = ::recvfrom(socket_.fd(), buffer, max_length, 0,
                               reinterpret_cast<struct sockaddr*>(&src_addr), &addr_len);

    if (bytes >= 0) {
        SocketUtil::sockaddr_to_ip_port(src_addr, src_host, src_port);
    }
    return bytes;
}

} // namespace distrikv
