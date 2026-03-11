/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/* 
Please specify the group members here

# Student #1: Katie Bell
# Student #2: Ian Rowe 
# Student #3: Kaleb Gordon 

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4
#define PIPELINE_WINDOW 32
#define TIMEOUT_US 2000000  //2 seconds

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

typedef struct {
    int epoll_fd;
    int socket_fd;
    long long total_rtt;
    long total_messages;
    float request_rate;
    long long tx_cnt;
    long long rx_cnt;
    long long lost_cnt;
} client_thread_data_t;

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKMLNOP";
    char recv_buf[MESSAGE_SIZE];
    struct timeval first_send_time, end_time;
    int have_first_send_time = 0;

    //each packet timing for pipeline
    struct timeval send_times[PIPELINE_WINDOW];
    int head = 0;
    int tail = 0;
    int outstanding = 0;

    data->request_rate = 0;
    data->total_rtt = 0;
    data->total_messages = 0;
    data->tx_cnt = 0;
    data->rx_cnt = 0;
    data->lost_cnt = 0;

    //socket in epoll
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) == -1) {
        perror("epoll_ctl add client socket");
        close(data->socket_fd);
        close(data->epoll_fd);
        return NULL;
    }

    while (data->tx_cnt < num_requests || outstanding > 0) {
        //fill pipeline with new requests
        while (data->tx_cnt < num_requests && outstanding < PIPELINE_WINDOW) {
            struct timeval now;
            gettimeofday(&now, NULL);
            if (!have_first_send_time) {
                first_send_time = now;
                have_first_send_time = 1;
            }

            ssize_t sent = send(data->socket_fd, send_buf, MESSAGE_SIZE, 0);
            if (sent < 0) {
                perror("send");
                break;
            }

            send_times[tail] = now;
            tail = (tail + 1) % PIPELINE_WINDOW;
            outstanding++;
            data->tx_cnt++;
        }

        //handle each packet timeouts 
        if (outstanding > 0) {
            struct timeval now;
            gettimeofday(&now, NULL);
            while (outstanding > 0) {
                long long age_us =
                    (now.tv_sec - send_times[head].tv_sec) * 1000000LL +
                    (now.tv_usec - send_times[head].tv_usec);
                if (age_us >= TIMEOUT_US) {
                    //packet lost
                    data->lost_cnt++;
                    head = (head + 1) % PIPELINE_WINDOW;
                    outstanding--;
                } else {
                    break;
                }
            }
        }

        //wait for responses
        int n_events = epoll_wait(data->epoll_fd, events, MAX_EVENTS, 10);
        if (n_events < 0) {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == data->socket_fd && (events[i].events & EPOLLIN)) {
                while (1) {
                    ssize_t n = recv(data->socket_fd, recv_buf, MESSAGE_SIZE, 0);
                    if (n > 0) {
                        struct timeval now;
                        gettimeofday(&now, NULL);

                        if (outstanding > 0) {
                            long long rtt =
                                (now.tv_sec - send_times[head].tv_sec) * 1000000LL +
                                (now.tv_usec - send_times[head].tv_usec);
                            data->total_rtt += rtt;
                            data->total_messages++;
                            data->rx_cnt++;
                            head = (head + 1) % PIPELINE_WINDOW;
                            outstanding--;
                        } else {
                            //unexpected extra packet
                            data->rx_cnt++;
                        }
                    } else {
                        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                            //no more data to read on this socket
                            break;
                        }
                        if (n == 0) {
                            //peer closed; stop reading
                            break;
                        }
                    }
                }
            }
        }
    }

    if (have_first_send_time && data->total_messages > 0) {
        gettimeofday(&end_time, NULL);
        long long elapsed_us =
            (end_time.tv_sec - first_send_time.tv_sec) * 1000000LL +
            (end_time.tv_usec - first_send_time.tv_usec);
        if (elapsed_us > 0) {
            data->request_rate = (float)data->total_messages * 1000000.0f / (float)elapsed_us;
        }
    }

    close(data->socket_fd);
    close(data->epoll_fd);
    return NULL;
}

void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    //create client threads
    for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].epoll_fd = epoll_create1(0);
        if (thread_data[i].epoll_fd == -1) {
            perror("epoll_create1");
            exit(EXIT_FAILURE);
        }

        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (thread_data[i].socket_fd == -1) {
            perror("socket");
            exit(EXIT_FAILURE);
        }

        //make client socket non-blocking for epoll-based IO
        int flags = fcntl(thread_data[i].socket_fd, F_GETFL, 0);
        if (flags == -1 || fcntl(thread_data[i].socket_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            perror("fcntl client socket O_NONBLOCK");
            exit(EXIT_FAILURE);
        }

        if (connect(thread_data[i].socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
            perror("connect");
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < num_client_threads; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    //wait for threads to complete
    long long total_rtt = 0;
    long total_messages = 0;
    float total_request_rate = 0;
    long long total_tx = 0;
    long long total_rx = 0;
    long long total_lost = 0;

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;
        total_lost += thread_data[i].lost_cnt;
    }

    if (total_messages > 0) {
        printf("Average RTT (successful): %lld us\n", total_rtt / total_messages);
    } else {
        printf("Average RTT (successful): N/A (no successful responses)\n");
    }
    printf("Aggregated Request Rate: %f messages/s\n", total_request_rate);
    printf("Total TX packets: %lld\n", total_tx);
    printf("Total RX packets: %lld\n", total_rx);
    printf("Total lost packets (timer-detected): %lld\n", total_lost);
    printf("Total lost packets (TX-RX): %lld\n", total_tx - total_rx);
}

void run_server() {
    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    //make server socket non-blocking for epoll based IO
    int flags = fcntl(server_fd, F_GETFL, 0);
    if (flags == -1 || fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl server socket O_NONBLOCK");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    int epoll_fd = epoll_create1(0);
    struct epoll_event event, events[MAX_EVENTS];

    event.events = EPOLLIN;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        perror("epoll_ctl add server_fd");
        close(server_fd);
        close(epoll_fd);
        exit(EXIT_FAILURE);
    }

    while (1) {
        int n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == server_fd && (events[i].events & EPOLLIN)) {
                while (1) {
                    char buffer[MESSAGE_SIZE];
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    ssize_t n = recvfrom(server_fd, buffer, MESSAGE_SIZE, 0,
                                         (struct sockaddr *)&client_addr, &client_len);
                    if (n > 0) {
                        //echo the datagram back to the sender
                        ssize_t sent = sendto(server_fd, buffer, n, 0,
                                              (struct sockaddr *)&client_addr, client_len);
                        if (sent < 0) {
                            perror("sendto");
                        }
                    } else {
                        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                            //no more data to read on this socket
                            break;
                        }
                        if (n == 0) {
                            break;
                        }
                    }
                }
            }
        }
    }

    close(server_fd);
    close(epoll_fd);
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);

        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);

        run_client();
    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}
