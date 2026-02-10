#include <iostream>
#include <string>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

constexpr int PORT = 8080;
constexpr int BUFFER_SIZE = 1024;

class SocketClient {
    int sock{-1};

public:
    SocketClient() = default;
    
    ~SocketClient() {
        if (sock >= 0) {
            close(sock);
        }
    }

    bool connect_to_server(const char* ip, int port) {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cerr << "Failed to create socket" << std::endl;
            return false;
        }

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);

        if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
            std::cerr << "Invalid address: " << ip << std::endl;
            return false;
        }

        if (connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
            std::cerr << "Connection failed: " << std::strerror(errno) << std::endl;
            return false;
        }

        std::cout << "Connected to server " << ip << ":" << port << std::endl;
        return true;
    }

    bool send_message(const std::string& msg) {
        // 1. 发送长度（4 字节网络序）
        uint32_t len = htonl(static_cast<uint32_t>(msg.size()));
        if (send(sock, &len, 4, 0) != 4) {
            std::cerr << "Failed to send message length" << std::endl;
            return false;
        }

        // 2. 发送消息内容
        if (send(sock, msg.data(), msg.size(), 0) != static_cast<ssize_t>(msg.size())) {
            std::cerr << "Failed to send message content" << std::endl;
            return false;
        }

        std::cout << "Sent (" << msg.size() << " bytes): " << msg << std::endl;
        return true;
    }

    bool recv_message(std::string& msg) {
        // 1. 接收长度（4 字节）
        uint32_t len_be;
        ssize_t n = recv(sock, &len_be, 4, MSG_WAITALL);
        if (n <= 0) {
            if (n == 0) {
                std::cout << "Server closed connection" << std::endl;
            } else {
                std::cerr << "Failed to receive message length: " << std::strerror(errno) << std::endl;
            }
            return false;
        }

        uint32_t len = ntohl(len_be);
        if (len > 64 * 1024) {
            std::cerr << "Message too large: " << len << " bytes" << std::endl;
            return false;
        }

        // 2. 接收消息内容
        msg.resize(len);
        n = recv(sock, msg.data(), len, MSG_WAITALL);
        if (n != static_cast<ssize_t>(len)) {
            std::cerr << "Failed to receive complete message" << std::endl;
            return false;
        }

        std::cout << "Received (" << len << " bytes): " << msg << std::endl;
        return true;
    }

    bool is_connected() const {
        return sock >= 0;
    }
};

int main(int argc, char* argv[]) {
    const char* server_ip = "127.0.0.1";
    
    if (argc > 1) {
        server_ip = argv[1];
    }

    SocketClient client;
    
    if (!client.connect_to_server(server_ip, PORT)) {
        return 1;
    }

    std::cout << "\nEnter messages (Ctrl+D or 'quit' to exit):\n" << std::endl;

    std::string input;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, input)) {
            std::cout << "\nExiting..." << std::endl;
            break;
        }

        if (input.empty()) {
            continue;
        }

        if (input == "quit" || input == "exit") {
            std::cout << "Goodbye!" << std::endl;
            break;
        }

        // 发送消息
        if (!client.send_message(input)) {
            break;
        }

        // 接收回显
        std::string response;
        if (!client.recv_message(response)) {
            break;
        }

        // 验证回显内容
        if (response != input) {
            std::cerr << "Warning: Echo mismatch!" << std::endl;
        }
    }

    return 0;
}