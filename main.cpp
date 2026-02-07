#include<iostream>
#include<stdlib.h>
#include<sys/epoll.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<unistd.h>


#define MAX_EVENTS 10
#define PORT 8080
#define BUFFER_SIZE 1024



int main() {

    epoll_event ev, events[MAX_EVENTS];
    int listen_sock, conn_sock, nfds, epollfd;
    sockaddr_in server_addr, client_addr, address;

    listen_sock = socket(AF_INET, SOCK_STREAM, 0);

    if (listen_sock < 0) {
        std::cout << "Failed to create socket" << std::endl;
        return -1;  
    }

    int opt = 1;
    int rt = setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (rt < 0) {
        std::cout << "Failed to set socket options" << std::endl;
        return -1;  
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    rt = bind(listen_sock, (sockaddr *)&address, sizeof(address));

    if (rt < 0) {
        std::cout << "Failed to bind socket" << std::endl;
        return -1;  
    }

    rt = listen(listen_sock, 10);

    if (rt < 0) {
        std::cout << "Failed to listen on socket" << std::endl;
        return -1;  
    }

    epollfd = epoll_create1(0); 

    if (epollfd < 0) {
        std::cout << "Failed to create epoll instance" << std::endl;
        return -1;  
    }

    ev.events = EPOLLIN;
    ev.data.fd = listen_sock;
    rt = epoll_ctl(epollfd, EPOLL_CTL_ADD, listen_sock, &ev);

    if (rt < 0) {
        std::cout << "Failed to add listen socket to epoll" << std::endl;
        return -1;  
    }

    std::cout << "Server is listening on port " << PORT << std::endl;

    while (true) {
        nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);

        if (nfds < 0) {
            std::cout << "Failed to wait for events" << std::endl;
            return -1;  
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == listen_sock) {
                conn_sock = accept(listen_sock, NULL, NULL);

                if (conn_sock < 0) {
                    std::cout << "Failed to accept connection" << std::endl;
                    continue;  
                }

                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = conn_sock;
                rt = epoll_ctl(epollfd, EPOLL_CTL_ADD, conn_sock, &ev); 

                if (rt < 0) {
                    std::cout << "Failed to add connection socket to epoll" << std::endl;
                    return -1;  
                }
            } else {
                char buffer[BUFFER_SIZE];
                int bytes = recv(events[i].data.fd, buffer, BUFFER_SIZE, 0);

                if (bytes <= 0) {
                    close(events[i].data.fd);
                    continue;  
                } else {
                    buffer[bytes] = '\0';
                    std::cout << "Received data: " << buffer << std::endl;
                    write(events[i].data.fd, buffer, bytes);
                }
            }
        }
    }

    close(listen_sock);
    close(epollfd);


    return 0;
}