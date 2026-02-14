#include <iostream>
#include <string_view>
#include <span>
#include <optional>
#include <memory>
#include <array>
#include <queue> 
#include <system_error>
#include <cerrno>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <csignal>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <mutex>
#include <functional>
#include <thread>
#include <sys/eventfd.h>
#include <netinet/tcp.h>
#include<sstream>
#include<iomanip>
#include<algorithm>

// ==================== 日志系统 ====================

#ifndef ENABLE_LOGGING
#define ENABLE_LOGGING false
#endif
enum class LogLevel { DEBUG, INFO, WARN, ERROR, OFF };

class Logger {
public:

    class Stream {
    public:
        explicit Stream(LogLevel level) : level_(level) {}
        ~Stream() {
            std::string level_str;
            if (!Logger::should_log(level_)) {
                return;
            }
            std::lock_guard<std::mutex> lock(Logger::mutex_);
            auto& out = (level_ == LogLevel::ERROR || level_ == LogLevel::WARN) ? std::cerr : std::cout;
            out << "[" << Logger::level_name(level_) << "] " 
                << "[" << std::this_thread::get_id() << "] "
                << oss_.str() << std::endl;
        }
        std::ostringstream& stream() {
            return oss_;
        }
    private:
        LogLevel level_;
        std::ostringstream oss_;
    };
    
    static void set_level(LogLevel level) {
        level_.store(level, std::memory_order_release);
    }
    static bool should_log(LogLevel level) {
        return static_cast<int>(level) >= static_cast<int>(level_.load(std::memory_order_acquire));
    }
    static const char* level_name(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO";
            case LogLevel::WARN:  return "WARN";
            case LogLevel::ERROR: return "ERROR";
            default:              return "OFF";
        }
    }

private:
    static std::atomic<LogLevel> level_;
    static std::mutex mutex_;
};

std::atomic<LogLevel> Logger::level_{LogLevel::WARN};
std::mutex Logger::mutex_;

#if ENABLE_LOGGING
    #define LOG_DEBUG if (Logger::should_log(LogLevel::DEBUG)) Logger::Stream(LogLevel::DEBUG).stream()
    #define LOG_INFO  if (Logger::should_log(LogLevel::INFO))  Logger::Stream(LogLevel::INFO).stream()
    #define LOG_WARN  if (Logger::should_log(LogLevel::WARN))  Logger::Stream(LogLevel::WARN).stream()
    #define LOG_ERROR if (Logger::should_log(LogLevel::ERROR)) Logger::Stream(LogLevel::ERROR).stream()
#else
    #define LOG_DEBUG if (false) std::cerr
    #define LOG_INFO  if (false) std::cerr
    #define LOG_WARN  if (false) std::cerr
    #define LOG_ERROR if (false) std::cerr
#endif

// ==================== 全局配置 ====================

const constexpr int MAX_EVENTS = 10;
const constexpr int PORT = 8080;
const constexpr int BUFFER_SIZE = 1024;
const constexpr int WORKER_THREADS = 4;

std::atomic<bool> running{true}; // 控制服务器运行状态，可以通过信号处理函数修改这个变量来优雅地关闭服务器

void signal_handler(int signum) {
    running.store(false, std::memory_order_release);
}

void setup_signal_handlers() {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);  // kill 命令
    signal(SIGHUP, signal_handler);   // 终端断开
}

// ==================== RAII FileDescriptor ====================
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

// ==================== 工作函数 ====================

[[nodiscard]] std::optional<std::string> set_socket(int fd) {
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        return "Failed to set socket options";  
    }
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        return "Failed to set TCP_NODELAY";
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

inline bool del_epoll(int epollfd, int fd) {
    return epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, nullptr) == 0;
}

// ==================== Buffer ====================

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

// ==================== Connection ====================

struct Connection { // 可以防止读写无法一次完成时，数据丢失
    int fd{-1};
    Buffer in;
    Buffer out;
};

// ==================== EventLoop ====================

class EventLoop {
public:
    EventLoop() {
        epollfd_ = FileDescriptor{epoll_create1(0)};
        if (!epollfd_.valid()) {
            throw std::runtime_error("Failed to create epoll instance");
        }

        wakeup_fd_ = FileDescriptor{eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
        if (!wakeup_fd_.valid()) {
            throw std::runtime_error("Failed to create eventfd");
        }

        if (!add_epoll(epollfd_.get(), wakeup_fd_.get(), EPOLLIN | EPOLLET)) {
            throw std::runtime_error("Failed to add wakeup fd to epoll");
        }
    }

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;    
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;
    
    void loop() {
        std::array<epoll_event, MAX_EVENTS> events{};

        while (running.load(std::memory_order_acquire)) {
            int nfds = epoll_wait(epollfd_.get(), events.data(), MAX_EVENTS, 1000); // 1秒超时，允许检查运行状态

            if (nfds < 0) {
                if (errno == EINTR) {
                    LOG_WARN << "Epoll wait interrupted by signal: " << std::strerror(errno);
                    continue;
                }
                LOG_ERROR << "Failed to wait for events: " << std::strerror(errno);
                break;
            }

            run_pending_tasks();

            for (int i = 0; i < nfds; ++i) {
                int fd = events[i].data.fd;
                uint32_t evs = events[i].events;

                if (fd == wakeup_fd_.get()) {
                    handle_wakeup();
                    continue;
                }
                    
                if (evs & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    close_connection(fd);
                    continue;
                }
                auto it = connections_.find(fd);
                if (it == connections_.end()) {
                    continue;
                }

                if (evs & EPOLLIN) {
                    handle_read(it->second);
                }

                if (evs & EPOLLOUT) {
                    handle_write(it->second);
                }

                if (it->second.fd < 0) {
                    LOG_INFO << "[Worker " << std::this_thread::get_id() << "] Cleaning up closed connection: fd=" << fd;
                    connections_.erase(it);
                }
            }
        }
        clean_up();
    }

    void add_connection(int fd) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_tasks_.push([this, fd]() {
                if (auto err = set_nonblocking(fd); err) {
                    LOG_ERROR << *err;
                    close(fd);
                    return;
                }
                if (!add_epoll(epollfd_.get(), fd, EPOLLIN | EPOLLET | EPOLLRDHUP)) {
                    LOG_ERROR << "Failed to add fd=" << fd << " to epoll";
                    close(fd);
                    return;
                }
                connections_.emplace(fd, Connection{fd});
                LOG_INFO << "Added new connection: fd=" << fd;
            });
        }
        wakeup(); 
    }

    std::size_t connection_count() const {
        return connections_.size();
    }
private:
    FileDescriptor epollfd_;
    FileDescriptor wakeup_fd_;
    std::unordered_map<int, Connection> connections_;

    // 待执行的任务队列
    std::mutex mutex_; // 保护 connections_ 的访问
    std::queue<std::function<void()>> pending_tasks_; 

    void wakeup() {
        uint64_t one = 1;
        ssize_t n = write(wakeup_fd_.get(), &one, sizeof(one)); // 向 wakeup_fd_ 写入数据以触发 epoll 事件
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_WARN << "Failed to write to wakeup fd: " << std::strerror(errno);
        }
    }
    
    void handle_wakeup() {
        uint64_t val;
        ssize_t n = read(wakeup_fd_.get(), &val, sizeof(val)); // 读取 wakeup_fd_ 中的数据以清除事件
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_WARN << "Failed to read from wakeup fd: " << std::strerror(errno);
        }
    }

    void run_pending_tasks() {
        std::queue<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::swap(tasks, pending_tasks_); // 交换队列，减少锁的持有时间
        }
        while (!tasks.empty()) {
            tasks.front()(); // 执行任务
            tasks.pop();
        }
    }


    void close_connection(int fd) {
        LOG_INFO << "[Worker " << std::this_thread::get_id() << "] Closing connection: fd=" << fd;
        del_epoll(epollfd_.get(), fd);
        close(fd);
    }

    void handle_read(Connection& c) {
        char buffer[BUFFER_SIZE];

        while (true) {
            int bytes = recv(c.fd, buffer, BUFFER_SIZE, 0);
            if (bytes > 0) {
                c.in.append(buffer, static_cast<std::size_t>(bytes));
            } else if (bytes == 0) {
                LOG_INFO << "Client disconnected: fd=" << c.fd;
                close_connection(c.fd);
                c.fd = -1;
                return;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue; // 中断，继续读取
                }
                LOG_ERROR << "Failed to read: " << std::strerror(errno);
                close_connection(c.fd);
                c.fd = -1;
                return;
            }
        }

        // 按消息长度拆包
        constexpr uint32_t MAX_MESSAGE_SIZE = 64 * 1024;
        while (c.in.readable() >= 4) {
            uint32_t msg_len;
            std::memcpy(&msg_len, c.in.peek().data(), 4);
            msg_len = ntohl(msg_len);

            if (msg_len > MAX_MESSAGE_SIZE) {
                LOG_WARN << "Message too large: " << msg_len;
                close_connection(c.fd);
                c.fd = -1;
                return;
            }
        
            if (c.in.readable() < 4 + msg_len) { // 等待更多数据 
                break;
            }
        
            std::string_view msg(c.in.peek().data() + 4, msg_len);
            LOG_INFO << "[Worker " << std::this_thread::get_id() << "] Received (" << msg_len << " bytes): " << msg;
        
            uint32_t len_be = htonl(msg_len);
            c.out.append(reinterpret_cast<const char*>(&len_be), 4); // 回显消息长度
            c.out.append(c.in.peek().data() + 4, msg_len); // 回显消息内容
            c.in.consume(4 + msg_len);
        }

        if (c.out.readable() > 0) {
            handle_write(c);
        }
    }

    void handle_write(Connection& c) {
        while (c.out.readable() > 0) {
            int n = send(c.fd, c.out.peek().data(), c.out.readable(), MSG_NOSIGNAL);
            if (n > 0) {
                c.out.consume(static_cast<std::size_t>(n));
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    mod_epoll(epollfd_.get(), c.fd, EPOLLIN | EPOLLOUT | EPOLLET);
                    return;
                } 
                LOG_ERROR << "Failed to send response: " << std::strerror(errno);
                close_connection(c.fd); 
                c.fd = -1;
                return;   
            }
        }
        mod_epoll(epollfd_.get(), c.fd, EPOLLIN | EPOLLET); // 没有更多数据要写了，修改事件为只关注可读事件
    }

    void clean_up() {
        for (auto& [fd, conn] : connections_) {
            LOG_INFO << "[Worker " << std::this_thread::get_id() << "] Cleanup: fd=" << fd;
            del_epoll(epollfd_.get(), fd);
            close(fd);
        }
        connections_.clear();
    }
}; 


int main() {

    setup_signal_handlers(); // 设置信号处理函数
    Logger::set_level(LogLevel::OFF); // 设置日志级别
    
    FileDescriptor listen_sock{socket(AF_INET, SOCK_STREAM, 0)};
    if (!listen_sock.valid()) {
        LOG_ERROR << "Failed to create socket";
        return 1;  
    }

    if (auto err = set_socket(listen_sock.get()); err) {
        LOG_ERROR << *err;
        return 1;  
    }

    if (auto err = set_nonblocking(listen_sock.get()); err) {
        LOG_ERROR << *err;
        return 1;  
    }
    
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; 
    address.sin_port = htons(PORT);

    if (bind(listen_sock.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        LOG_ERROR << "Failed to bind socket";
        return 1;  
    }

    if (listen(listen_sock.get(), 10) < 0) {
        LOG_ERROR << "Failed to listen on socket";
        return 1;  
    }

    FileDescriptor main_epollfd{epoll_create1(0)}; // 监听 listen_sock
    if (!main_epollfd.valid()) {
        LOG_ERROR << "Failed to create epoll instance";
        return 1;  
    }

    if (!add_epoll(main_epollfd.get(), listen_sock.get(), EPOLLIN)) {
        LOG_ERROR << "Failed to add listen socket to epoll";
        return 1;  
    }

    // 创建工作线程池
    std::vector<std::thread> workers;
    std::vector<std::unique_ptr<EventLoop>> event_loops;

    for (int i = 0; i < WORKER_THREADS; ++i) {
        event_loops.push_back(std::make_unique<EventLoop>());
        workers.emplace_back([&loop = *event_loops[i]]() {
            loop.loop();
        });
    }

    LOG_INFO << "Server is listening on port " << PORT << " with " << WORKER_THREADS << " worker threads";

    std::size_t next_worker = 0;
    std::array<epoll_event, MAX_EVENTS> events{};

    while (running.load(std::memory_order_acquire)) {
        int nfds = epoll_wait(main_epollfd.get(), events.data(), MAX_EVENTS, 100); // 1秒超时，允许检查运行状态

        if (nfds < 0) {
            if (errno == EINTR) { // 被信号中断，继续等待
                continue;
            }
            LOG_ERROR << "Failed to wait for events";
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            while (true) {
                int conn_fd = accept(listen_sock.get(), nullptr, nullptr);
                if (conn_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break; // 没有更多连接了，继续等待事件  
                    }
                    LOG_WARN << "Failed to accept connection: " << std::strerror(errno);
                    break;
                }
                LOG_INFO << "Accepted new connection: fd=" << conn_fd;
                event_loops[next_worker]->add_connection(conn_fd); // 将新连接分配给工作线程
                next_worker = (next_worker + 1) % WORKER_THREADS;

                LOG_INFO << "[Main] Dispatched connection fd=" << conn_fd << " to worker " << (next_worker - 1 + WORKER_THREADS) % WORKER_THREADS;
            }
        }
    }

    LOG_INFO << "\nServer is shutting down...";

    for (auto& t : workers) {
        if (t.joinable()) {
            t.join();
        }
    }

    LOG_INFO << "All connections closed. Goodbye!";

    return 0;
}