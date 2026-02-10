#include <iostream>
#include <string_view>
#include <span>
#include <optional>
#include <memory>
#include <array>
#include <system_error>
#include <cerrno>
#include <cstring>
#include <vector>
#include <unordered_map>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

constexpr int MAX_EVENTS = 10;
constexpr int PORT = 8080;
constexpr int BUFFER_SIZE = 1024;

// RAII fd
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

struct Buffer {
    std::vector<char> data;
    std::size_t read_pos{0};

    std::size_t readable() const { // 可读字节数
        return data.size() - read_pos;
    }

    std::span<const char> peek() const { // 获取可读数据的指针
        return {data.data() + read_pos, readable()};
    }

    void append(const char* buf, std::size_t len) { // 追加数据到缓冲区
        data.insert(data.end(), buf, buf + len);
    }

    void consume(std::size_t len) { // 消费掉已读数据
        read_pos += len;
        if (read_pos > 0 && read_pos * 2 >= data.size()) { // 如果已读数据超过总数据的一半，进行数据压缩
            data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(read_pos));
            read_pos = 0;
        }
    }
};

struct Connection { // 可以防止读写无法一次完成时，数据丢失
    int fd{-1};
    Buffer in;
    Buffer out;
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

inline bool add_epoll(int epollfd, int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev) == 0;
}
inline bool mod_epoll(int epollfd, int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &ev) == 0;
}
// bool del_epoll(int epollfd, int fd) {
//     return epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, nullptr) == 0;
// }

void close_conn(int epfd, std::unordered_map<int, Connection>& conns, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    conns.erase(fd);
}

void handle_write(int epfd, Connection& c) {
    while (c.out.readable() > 0) {
        int n = send(c.fd, c.out.peek().data(), c.out.readable(), MSG_NOSIGNAL);
        if (n > 0) {
            c.out.consume(static_cast<std::size_t>(n));
        } else if (n == 0) {
            std::cerr << "Connection closed by peer: fd=" << c.fd << std::endl;
            close(c.fd);
            c.fd = -1;
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                mod_epoll(epfd, c.fd, EPOLLIN | EPOLLOUT | EPOLLET);
                break;
            } else {
                std::cerr << "Failed to send response: " << std::strerror(errno) << std::endl;
                close(c.fd); 
                c.fd = -1;
                return;   
            }
        }
    }
}

void handle_read(int epfd, Connection& c) {
    char buffer[BUFFER_SIZE];

    // 1. 读数据到缓冲区
    while (true) {
        int bytes = recv(c.fd, buffer, BUFFER_SIZE, 0);
        if (bytes > 0) {
            c.in.append(buffer, static_cast<std::size_t>(bytes));
        } else if (bytes == 0) {
            std::cerr << "Client disconnected: fd=" << c.fd << std::endl;
            close(c.fd);
            c.fd = -1;
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            std::cerr << "Failed to read: " << std::strerror(errno) << std::endl;
            close(c.fd);
            c.fd = -1;
            return;
        }
    }

    // 2. 按消息长度拆包
    constexpr uint32_t MAX_MESSAGE_SIZE = 64 * 1024;
    while (c.in.readable() >= 4) {
        uint32_t msg_len;
        std::memcpy(&msg_len, c.in.peek().data(), 4);
        msg_len = ntohl(msg_len);

        if (msg_len > MAX_MESSAGE_SIZE) {
            std::cerr << "Message too large, closing connection\n";
            close(c.fd);
            c.fd = -1;
            return;
        }

        if (c.in.readable() < 4 + msg_len) { // 等待更多数据 
            break;
        }

        std::string_view msg(c.in.peek().data() + 4, msg_len);
        std::cout << "Received message: " << msg << std::endl;

        uint32_t len_be = htonl(msg_len);
        c.out.append(reinterpret_cast<const char*>(&len_be), 4); // 回显消息长度
        c.out.append(c.in.peek().data() + 4, msg_len); // 回显消息内容
        c.in.consume(4 + msg_len);
    }

    // 3. 尝试发送
    handle_write(epfd, c);
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

    if (!add_epoll(epollfd.get(), listen_sock.get(), EPOLLIN)) {
        std::cerr << "Failed to add listen socket to epoll" << std::endl;
        return 1;  
    }

    std::cout << "Server is listening on port " << PORT << std::endl;

    std::unordered_map<int, Connection> connections; // fd -> Connection
    std::array<epoll_event, MAX_EVENTS> events{};
    while (true) {
        int nfds = epoll_wait(epollfd.get(), events.data(), MAX_EVENTS, -1);

        if (nfds < 0) {
            if (errno == EINTR) { // 被信号中断，继续等待
                continue;
            }
            std::cerr << "Failed to wait for events" << std::endl;
            return 1;  
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            uint32_t evs = events[i].events;

            if (fd == listen_sock.get()) {
                int conn_sock = accept(listen_sock.get(), nullptr, nullptr); // 不能提前关闭，需要手动关闭

                if (conn_sock < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;  
                    }
                    std::cerr << "Failed to accept connection" << std::endl;
                    continue;  
                }

                if (auto err = set_nonblocking(conn_sock); err) {
                    std::cerr << *err << std::endl;
                    close(conn_sock);
                    continue;  
                }

                if (!add_epoll(epollfd.get(), conn_sock, EPOLLIN | EPOLLET | EPOLLRDHUP)) {
                    std::cerr << "Failed to add connection socket to epoll" << std::endl;
                    close(conn_sock);
                    continue;
                }
                
                connections.emplace(conn_sock, Connection{conn_sock});
                std::cout << "Accepted new connection: fd=" << conn_sock << std::endl;

            } else {
                if (evs & (EPOLLRDHUP | EPOLLHUP)) {
                    std::cout << "Client disconnected: fd=" << fd << std::endl;
                    close_conn(epollfd.get(), connections, fd);
                    continue;
                }
                if (evs & EPOLLERR) {
                    std::cerr << "Connection error or hang up: fd=" << fd << std::endl;
                    close_conn(epollfd.get(), connections, fd);
                    continue;
                }

                auto it = connections.find(fd);
                if (it == connections.end()) {
                    std::cerr << "Unknown file descriptor: " << fd << std::endl;
                    continue;
                }

                try {
                    if (evs & EPOLLIN) {
                        handle_read(epollfd.get(), it->second);
                    }
                    if (evs & EPOLLOUT) {
                        handle_write(epollfd.get(), it->second);
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Error handling connection: " << e.what() << std::endl;
                    close_conn(epollfd.get(), connections, fd);
                }
            }
        }
    }

    return 0;
}