#include <iostream>
#include <string_view>
#include <span>
#include <optional>
#include <memory>
#include <array>
#include <system_error>
#include <cerrno>
#include <cstring>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

constexpr int MAX_EVENTS = 10;
constexpr int PORT = 8080;
constexpr int BUFFER_SIZE = 1024;

// modern c++ RAII 
class FileDescriptor {
    int fd{-1};
public:
    FileDescriptor() = default;
    explicit FileDescriptor(int fd) : fd(fd) {}
    
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : fd(other.fd) {
        other.fd = -1;
    }
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) {
                close(fd);
            }
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }

    ~FileDescriptor() {
        if (fd >= 0) {
            close(fd);
        }
    }

    int get() const {
        return fd;
    }

    bool valid() const {
        return fd >= 0;
    }
};

[[nodiscard]] std::optional<std::string> set_socket(int fd) {
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        return "Failed to set socket options";  
    }
    return std::nullopt;
}   

[[nodiscard]] std::optional<std::string> set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return "Failed to get file descriptor flags";  
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return "Failed to set non-blocking mode";  
    }
    return std::nullopt;
}

int main() {

    FileDescriptor listen_sock{socket(AF_INET, SOCK_STREAM, 0)};
    if (!listen_sock.valid()) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;  
    }

    if (set_socket(listen_sock.get())) {
        std::cerr << "Failed to set socket options" << std::endl;
        return 1;  
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(listen_sock.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::cerr << "Failed to bind socket" << std::endl;
        return 1;  
    }

    if (listen(listen_sock.get(), 10) < 0) {
        std::cerr << "Failed to listen on socket" << std::endl;
        return 1;  
    }

    FileDescriptor epollfd{epoll_create1(0)};
    if (!epollfd.valid()) {
        std::cerr << "Failed to create epoll instance" << std::endl;
        return 1;  
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_sock.get();

    if (epoll_ctl(epollfd.get(), EPOLL_CTL_ADD, listen_sock.get(), &ev)) {
        std::cerr << "Failed to add listen socket to epoll" << std::endl;
        return 1;  
    }

    std::cout << "Server is listening on port " << PORT << std::endl;

    std::array<epoll_event, MAX_EVENTS> events{};
    while (true) {
        int nfds = epoll_wait(epollfd.get(), events.data(), MAX_EVENTS, -1);

        if (nfds < 0) {
            std::cerr << "Failed to wait for events" << std::endl;
            return 1;  
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == listen_sock.get()) {
                int conn_sock = accept(listen_sock.get(), nullptr, nullptr); // 不能提前关闭，需要手动关闭

                if (conn_sock < 0) {
                    std::cerr << "Failed to accept connection" << std::endl;
                    continue;  
                }

                if (auto err = set_nonblocking(conn_sock); err) {
                    std::cerr << *err << std::endl;
                    close(conn_sock);
                    continue;  
                }

                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = conn_sock;

                if (epoll_ctl(epollfd.get(), EPOLL_CTL_ADD, conn_sock, &ev) < 0) {
                    std::cerr << "Failed to add connection socket to epoll" << std::endl;
                    close(conn_sock);
                    continue;
                }

                std::cout << "Accepted new connection" << std::endl;

            } else {
                char buffer[BUFFER_SIZE];
                int fd = events[i].data.fd;

                while (true) {
                    int bytes = recv(fd, buffer, BUFFER_SIZE - 1, 0);

                    if (bytes < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;  
                        }
                        std::cerr << "Failed to read from socket" << std::endl;
                        epoll_ctl(epollfd.get(), EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        break;  
                    } else if (bytes == 0) {
                        std::cout << "Client disconnected" << std::endl;
                        epoll_ctl(epollfd.get(), EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        break;  
                    } else {
                        buffer[bytes] = '\0';
                        std::cout << "Received: " << buffer << std::endl;
                        if (send(fd, buffer, bytes, MSG_NOSIGNAL) < 0) { // 发送失败，可能是客户端已断开连接
                            std::cerr << "Failed to send response" << std::endl;
                            epoll_ctl(epollfd.get(), EPOLL_CTL_DEL, fd, nullptr);
                            close(fd);
                            break;  
                        }
                    }
                }
            }
        }
    }

    return 0;
}