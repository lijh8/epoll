//server.c

#include <sys/epoll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#define PRINT_LOG(fmt, ...) do \
{ \
    printf("%s:%d:%s: " fmt "\n", \
        __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
} while (0) /* ; no trailing semicolon here */

#define PERROR(fmt, ...) do \
{ \
    printf("%s:%d:%s: %s. " fmt "\n", \
        __FILE__, __LINE__, __func__, strerror(errno), ##__VA_ARGS__); \
} while (0) /* ; no trailing semicolon here */

int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        PERROR("fcntl");
        return -1;
    }
    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) == -1) {
        PERROR("fcntl");
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    struct epoll_event ev, events[10];
    int listenfd, connfd, nfds, efd;

    /* set up listening socket */
    int port = 8000;
    int ret = 0, conn_count = 0;
    struct sockaddr_in my_addr, peer_addr;
    socklen_t addrlen = sizeof(peer_addr);
    int sockopt = 1;
    int more_to_write = 0;

    if (argc == 2){
        int n = atoi(argv[1]);
        if (n != 0) {
            port = n;
        }
    }

    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_port = htons(port);
    my_addr.sin_addr.s_addr = htons(INADDR_ANY);
    my_addr.sin_family = AF_INET;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) {
        PERROR("socket");
        return -1;
    }

    ret = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &sockopt,
                                                            sizeof(sockopt));
    if (ret == -1){
        PERROR("setsockopt");
        close(listenfd);
        return -1;
    }

    ret = bind(listenfd, (struct sockaddr*)&my_addr, sizeof(my_addr));
    if (ret == -1) {
        PERROR("bind");
        close(listenfd);
        return -1;
    }

    ret = listen(listenfd, SOMAXCONN);
    if (ret == -1) {
        PERROR("listen");
        close(listenfd);
        return -1;
    }
    PRINT_LOG("listen on port: %d", port);
    signal(SIGPIPE, SIG_IGN);

    /* set up epoll */
    efd = epoll_create1(0);
    if (efd == -1) {
        PERROR("epoll_create1");
        return -1;
    }

    ev.events = EPOLLIN;
    ev.data.fd = listenfd;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, listenfd, &ev) == -1) {
        PERROR("epoll_ctl");
        return -1;
    }
    PRINT_LOG("waiting for data input from clients");

    for (;;) {
        nfds = epoll_wait(efd, events, sizeof(events) / sizeof(events[0]),
                                                                1000); //1 sec
        if (nfds == -1) {
            PERROR("epoll_wait");

            for (int i = 0; i != sizeof(events) / sizeof(events[0]); i++){
                close(events[i].data.fd);
            }
            return -1;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == listenfd) {
                connfd = accept(listenfd,
                                (struct sockaddr *) &peer_addr, &addrlen);
                if (connfd == -1) {
                    PERROR("accept");
                    continue;
                }
                if (set_nonblock(connfd)){
                    PRINT_LOG("set_nonblock failed");
                    continue;
                }

                conn_count++;
                PRINT_LOG("accept client %s:%u, conn_count: %d",
                    inet_ntoa(peer_addr.sin_addr), peer_addr.sin_port,
                                                                    conn_count);

                more_to_write = 0; // have more to write

                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = connfd;
                if (epoll_ctl(efd, EPOLL_CTL_ADD, connfd, &ev) == -1) {
                    PERROR("epoll_ctl");
                }

            } else if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                int fd = events[i].data.fd;
                close(fd);
                PRINT_LOG("EPOLLHUP | EPOLLERR");

            } else if (events[i].events & EPOLLIN) {
                int fd = events[i].data.fd;
                enum {count = 1024};
                char buf[count + 1] = {'\0'}, *p = buf;
                int len = 0;
                int closed = 0;
                int err = 0;

                while (len < count){
                    ret = read(fd, p, count - len);
                    err = errno;

                    if (ret > 0){
                        len += ret;
                        p += ret;
                    }
                    if (ret == 0){
                        close(fd);
                        closed = 1;
                        break;
                    }

                    if (err == EAGAIN || err == EWOULDBLOCK || err == EPIPE ||
                                                            err == ECONNRESET){
                        break;
                    }
                    if (err == EINTR){
                        continue;
                    }
                    if (err != 0){
                        PRINT_LOG("ret:%d, err:%d, %s", ret, err,
                                                                strerror(err));
                    }
                }

                if (len > 0){
                    PRINT_LOG("ret:%d, err:%d, len:%d, %s", ret, err, len, buf);
                }

                if (!closed){
                    ev.events = EPOLLOUT | EPOLLET;
                    ev.data.fd = fd;
                    if (epoll_ctl(efd, EPOLL_CTL_MOD, fd, &ev) == -1) {
                        PERROR("epoll_ctl");
                    }
                }

            } else if (events[i].events & EPOLLOUT){
                int fd = events[i].data.fd;
                enum {count = 1024};
                char buf[count + 1] = {'\0'}, *p = buf;
                int len = 0;
                int closed = 0;
                int err = 0;

                while (len < count){
                    sleep(1); //test
                    const char *msg = "hello client ";
                    memcpy(buf, msg, count);

                    ret = write(fd, p, count - len);
                    err = errno;

                    if (ret > 0){
                        len += ret;
                        p += ret;
                    }
                    if (ret == 0){
                        close(fd);
                        closed = 1;
                        break;
                    }

                    if (err == EAGAIN || err == EWOULDBLOCK || err == EPIPE ||
                                                            err == ECONNRESET){
                        break;
                    }
                    if (err == EINTR){
                        continue;
                    }
                    if (err != 0){
                        PRINT_LOG("ret:%d, err:%d, %s", ret, err,
                                                                strerror(err));
                    }
                }

                if (!closed){
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = fd;

                    // generate more EPOLLOUT to write more
                    // if (more_to_write++ < 3) {
                    //     ev.events = EPOLLOUT | EPOLLET;
                    // }

                    if (epoll_ctl(efd, EPOLL_CTL_MOD, fd, &ev) == -1) {
                        PERROR("epoll_ctl");
                    }
                }

            } else {
                PRINT_LOG("unknown event");
                int fd = events[i].data.fd;
                close(fd);

            }
        }
    }

    return 0;
}