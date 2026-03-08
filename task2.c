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

# Student #1: Alice
# Student #2: Bob
# Student #3: Charlie

*/

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4
#define PIPELINE_WINDOW 32
#define TIMEOUT_MS 2000
#define IDLE_WAIT_MS 10

#define PACKET_KIND_DATA 1u
#define PACKET_KIND_ACK 2u
#define INVALID_SEQ UINT32_MAX

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

typedef struct {
    uint32_t kind;
    uint32_t seq_num;
    uint32_t ack_num;
    uint32_t payload_len;
    char payload[MESSAGE_SIZE];
} packet_frame_t;

_Static_assert(sizeof(packet_frame_t) == 32, "packet_frame_t must be fixed-size");

typedef struct {
    uint32_t seq_num;
    int in_use;
    struct timespec sent_at;
    packet_frame_t packet;
} window_slot_t;

typedef struct {
    int thread_index;
    int epoll_fd;
    int socket_fd;
    uint32_t base;
    uint32_t next_seq;
    uint32_t window_size;
    long long tx_cnt;
    long long rx_cnt;
    long long timeout_cnt;
    int saw_timeout;
    uint32_t last_timeout_start;
    uint32_t last_timeout_end;
    window_slot_t window[PIPELINE_WINDOW];
} client_thread_data_t;

typedef struct server_client_state {
    struct sockaddr_in addr;
    uint32_t expected_seq;
    uint32_t last_acked_seq;
    int last_ack_valid;
    struct server_client_state *next;
} server_client_state_t;

static void close_if_open(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1) {
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return -1;
    }

    return 0;
}

static long long diff_ms(const struct timespec *end, const struct timespec *start) {
    long long sec_diff = (long long)(end->tv_sec - start->tv_sec) * 1000LL;
    long long nsec_diff = (long long)(end->tv_nsec - start->tv_nsec) / 1000000LL;

    return sec_diff + nsec_diff;
}

static packet_frame_t packet_to_network_order(const packet_frame_t *packet) {
    packet_frame_t network_packet = *packet;

    network_packet.kind = htonl(packet->kind);
    network_packet.seq_num = htonl(packet->seq_num);
    network_packet.ack_num = htonl(packet->ack_num);
    network_packet.payload_len = htonl(packet->payload_len);

    return network_packet;
}

static void packet_from_network_order(packet_frame_t *packet, const packet_frame_t *network_packet) {
    *packet = *network_packet;
    packet->kind = ntohl(network_packet->kind);
    packet->seq_num = ntohl(network_packet->seq_num);
    packet->ack_num = ntohl(network_packet->ack_num);
    packet->payload_len = ntohl(network_packet->payload_len);
}

static int send_packet(int socket_fd, const packet_frame_t *packet) {
    packet_frame_t network_packet = packet_to_network_order(packet);
    ssize_t sent = send(socket_fd, &network_packet, sizeof(network_packet), 0);

    if (sent < 0) {
        return -1;
    }

    if ((size_t)sent != sizeof(network_packet)) {
        errno = EMSGSIZE;
        return -1;
    }

    return 0;
}

static int send_packet_to(int socket_fd, const packet_frame_t *packet,
                          const struct sockaddr_in *client_addr, socklen_t client_len) {
    packet_frame_t network_packet = packet_to_network_order(packet);
    ssize_t sent = sendto(socket_fd, &network_packet, sizeof(network_packet), 0,
                          (const struct sockaddr *)client_addr, client_len);

    if (sent < 0) {
        return -1;
    }

    if ((size_t)sent != sizeof(network_packet)) {
        errno = EMSGSIZE;
        return -1;
    }

    return 0;
}

static int recv_packet(int socket_fd, packet_frame_t *packet) {
    packet_frame_t network_packet;
    ssize_t received = recv(socket_fd, &network_packet, sizeof(network_packet), 0);

    if (received <= 0) {
        return (int)received;
    }

    if ((size_t)received != sizeof(network_packet)) {
        return -2;
    }

    packet_from_network_order(packet, &network_packet);
    return 1;
}

static int recv_packet_from(int socket_fd, packet_frame_t *packet,
                            struct sockaddr_in *client_addr, socklen_t *client_len) {
    packet_frame_t network_packet;
    ssize_t received = recvfrom(socket_fd, &network_packet, sizeof(network_packet), 0,
                                (struct sockaddr *)client_addr, client_len);

    if (received <= 0) {
        return (int)received;
    }

    if ((size_t)received != sizeof(network_packet)) {
        return -2;
    }

    packet_from_network_order(packet, &network_packet);
    return 1;
}

static packet_frame_t make_data_packet(uint32_t seq_num, const char *payload, size_t payload_len) {
    packet_frame_t packet;
    size_t bounded_len = payload_len;

    if (bounded_len > MESSAGE_SIZE) {
        bounded_len = MESSAGE_SIZE;
    }

    memset(&packet, 0, sizeof(packet));
    packet.kind = PACKET_KIND_DATA;
    packet.seq_num = seq_num;
    packet.ack_num = INVALID_SEQ;
    packet.payload_len = (uint32_t)bounded_len;
    memcpy(packet.payload, payload, bounded_len);

    return packet;
}

static packet_frame_t make_ack_packet(uint32_t ack_num) {
    packet_frame_t packet;

    memset(&packet, 0, sizeof(packet));
    packet.kind = PACKET_KIND_ACK;
    packet.seq_num = 0;
    packet.ack_num = ack_num;
    packet.payload_len = 0;

    return packet;
}

static void clear_window_slot(client_thread_data_t *data, uint32_t seq_num) {
    window_slot_t *slot = &data->window[seq_num % data->window_size];

    if (slot->in_use && slot->seq_num == seq_num) {
        memset(slot, 0, sizeof(*slot));
    }
}

static int send_initial_packet(client_thread_data_t *data, const char *payload, size_t payload_len) {
    window_slot_t *slot = &data->window[data->next_seq % data->window_size];

    memset(slot, 0, sizeof(*slot));
    slot->seq_num = data->next_seq;
    slot->packet = make_data_packet(data->next_seq, payload, payload_len);

    if (clock_gettime(CLOCK_MONOTONIC, &slot->sent_at) == -1) {
        perror("clock_gettime");
        memset(slot, 0, sizeof(*slot));
        return -1;
    }

    if (send_packet(data->socket_fd, &slot->packet) == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            memset(slot, 0, sizeof(*slot));
            return 1;
        }

        perror("send");
        memset(slot, 0, sizeof(*slot));
        return -1;
    }

    slot->in_use = 1;
    data->tx_cnt++;
    data->next_seq++;
    return 0;
}

static int compute_epoll_timeout_ms(const client_thread_data_t *data) {
    const window_slot_t *slot;
    struct timespec now;
    long long remaining_ms;

    if (data->base == data->next_seq) {
        return IDLE_WAIT_MS;
    }

    slot = &data->window[data->base % data->window_size];
    if (!slot->in_use || slot->seq_num != data->base) {
        return 0;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return 0;
    }

    remaining_ms = TIMEOUT_MS - diff_ms(&now, &slot->sent_at);
    if (remaining_ms <= 0) {
        return 0;
    }

    if (remaining_ms > INT_MAX) {
        return INT_MAX;
    }

    return (int)remaining_ms;
}

static void handle_timeout_placeholder(client_thread_data_t *data) {
    uint32_t seq_num;

    data->saw_timeout = 1;
    data->last_timeout_start = data->base;
    data->last_timeout_end = data->next_seq;

    if (data->timeout_cnt <= 3) {
        fprintf(stderr,
                "thread %d timeout waiting for ACK, retransmit window [%u, %u)\n",
                data->thread_index, data->base, data->next_seq);
    }

    for (seq_num = data->base; seq_num < data->next_seq; seq_num++) {
        clear_window_slot(data, seq_num);
    }

    data->base = data->next_seq;
}

static void process_ack_packet(client_thread_data_t *data, const packet_frame_t *packet) {
    uint32_t ack_num;
    uint32_t newly_acked_base;

    if (packet->kind != PACKET_KIND_ACK || packet->payload_len != 0) {
        return;
    }

    if (packet->ack_num == INVALID_SEQ) {
        return;
    }

    if (data->base == data->next_seq) {
        return;
    }

    ack_num = packet->ack_num;
    if (ack_num < data->base) {
        return;
    }

    if (ack_num >= data->next_seq) {
        ack_num = data->next_seq - 1;
    }

    newly_acked_base = data->base;
    while (data->base <= ack_num) {
        clear_window_slot(data, data->base);
        data->base++;
    }

    data->rx_cnt += (long long)(data->base - newly_acked_base);
}

static int sockaddr_matches(const struct sockaddr_in *lhs, const struct sockaddr_in *rhs) {
    return lhs->sin_family == rhs->sin_family &&
           lhs->sin_port == rhs->sin_port &&
           lhs->sin_addr.s_addr == rhs->sin_addr.s_addr;
}

static server_client_state_t *find_or_create_client_state(server_client_state_t **client_states,
                                                          const struct sockaddr_in *client_addr) {
    server_client_state_t *current = *client_states;

    while (current != NULL) {
        if (sockaddr_matches(&current->addr, client_addr)) {
            return current;
        }
        current = current->next;
    }

    current = calloc(1, sizeof(*current));
    if (current == NULL) {
        return NULL;
    }

    current->addr = *client_addr;
    current->expected_seq = 0;
    current->last_acked_seq = INVALID_SEQ;
    current->last_ack_valid = 0;
    current->next = *client_states;
    *client_states = current;

    return current;
}

static void free_client_states(server_client_state_t *client_states) {
    server_client_state_t *next_state;

    while (client_states != NULL) {
        next_state = client_states->next;
        free(client_states);
        client_states = next_state;
    }
}

static void handle_server_packet(int server_fd, server_client_state_t **client_states,
                                 const packet_frame_t *packet, const struct sockaddr_in *client_addr,
                                 socklen_t client_len) {
    server_client_state_t *state;
    packet_frame_t ack_packet;

    if (packet->kind != PACKET_KIND_DATA || packet->payload_len > MESSAGE_SIZE) {
        return;
    }

    state = find_or_create_client_state(client_states, client_addr);
    if (state == NULL) {
        fprintf(stderr, "failed to allocate server client state\n");
        return;
    }

    if (packet->seq_num == state->expected_seq) {
        state->last_acked_seq = packet->seq_num;
        state->last_ack_valid = 1;
        state->expected_seq++;
        ack_packet = make_ack_packet(state->last_acked_seq);
    } else if (state->last_ack_valid) {
        ack_packet = make_ack_packet(state->last_acked_seq);
    } else {
        ack_packet = make_ack_packet(INVALID_SEQ);
    }

    if (send_packet_to(server_fd, &ack_packet, client_addr, client_len) == -1) {
        perror("sendto");
    }
}

static void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event;
    struct epoll_event events[MAX_EVENTS];
    char payload[MESSAGE_SIZE] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                                  'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P'};

    memset(data->window, 0, sizeof(data->window));
    data->base = 0;
    data->next_seq = 0;
    data->window_size = PIPELINE_WINDOW;
    data->tx_cnt = 0;
    data->rx_cnt = 0;
    data->timeout_cnt = 0;
    data->saw_timeout = 0;
    data->last_timeout_start = 0;
    data->last_timeout_end = 0;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) == -1) {
        perror("epoll_ctl add client socket");
        close_if_open(&data->socket_fd);
        close_if_open(&data->epoll_fd);
        return NULL;
    }

    while (data->next_seq < (uint32_t)num_requests || data->base < data->next_seq) {
        while (data->next_seq < (uint32_t)num_requests &&
               data->next_seq < data->base + data->window_size) {
            int send_status = send_initial_packet(data, payload, sizeof(payload));

            if (send_status == 0) {
                continue;
            }

            if (send_status > 0) {
                break;
            }

            close_if_open(&data->socket_fd);
            close_if_open(&data->epoll_fd);
            return NULL;
        }

        {
            int timeout_ms = compute_epoll_timeout_ms(data);
            int n_events = epoll_wait(data->epoll_fd, events, MAX_EVENTS, timeout_ms);

            if (n_events < 0) {
                if (errno == EINTR) {
                    continue;
                }

                perror("epoll_wait");
                break;
            }

            if (n_events == 0) {
                if (data->base < data->next_seq) {
                    data->timeout_cnt++;
                    handle_timeout_placeholder(data);
                }
                continue;
            }

            for (int i = 0; i < n_events; i++) {
                if ((events[i].events & EPOLLIN) == 0) {
                    continue;
                }

                while (1) {
                    packet_frame_t packet;
                    int recv_status = recv_packet(data->socket_fd, &packet);

                    if (recv_status > 0) {
                        process_ack_packet(data, &packet);
                        continue;
                    }

                    if (recv_status == -2) {
                        continue;
                    }

                    if (recv_status < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    }

                    if (recv_status < 0) {
                        perror("recv");
                        close_if_open(&data->socket_fd);
                        close_if_open(&data->epoll_fd);
                        return NULL;
                    }

                    if (recv_status == 0) {
                        continue;
                    }
                }
            }
        }
    }

    close_if_open(&data->socket_fd);
    close_if_open(&data->epoll_fd);
    return NULL;
}

static void run_client(void) {
    pthread_t *threads = NULL;
    client_thread_data_t *thread_data = NULL;
    struct sockaddr_in server_addr;
    long long total_tx = 0;
    long long total_rx = 0;
    long long total_timeouts = 0;
    int started_threads = 0;
    int client_error = 0;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "invalid server IP address: %s\n", server_ip);
        exit(EXIT_FAILURE);
    }

    threads = calloc((size_t)num_client_threads, sizeof(*threads));
    thread_data = calloc((size_t)num_client_threads, sizeof(*thread_data));
    if (threads == NULL || thread_data == NULL) {
        fprintf(stderr, "failed to allocate client thread state\n");
        free(threads);
        free(thread_data);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].thread_index = i;
        thread_data[i].epoll_fd = -1;
        thread_data[i].socket_fd = -1;

        thread_data[i].epoll_fd = epoll_create1(0);
        if (thread_data[i].epoll_fd == -1) {
            perror("epoll_create1");
            client_error = 1;
            break;
        }

        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (thread_data[i].socket_fd == -1) {
            perror("socket");
            client_error = 1;
            break;
        }

        if (set_nonblocking(thread_data[i].socket_fd) == -1) {
            perror("fcntl client socket O_NONBLOCK");
            client_error = 1;
            break;
        }

        if (connect(thread_data[i].socket_fd,
                    (struct sockaddr *)&server_addr,
                    sizeof(server_addr)) == -1) {
            perror("connect");
            client_error = 1;
            break;
        }

        {
            int thread_error = pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
            if (thread_error != 0) {
                fprintf(stderr, "pthread_create: %s\n", strerror(thread_error));
                client_error = 1;
                break;
            }
        }

        started_threads++;
    }

    for (int i = 0; i < started_threads; i++) {
        int join_error = pthread_join(threads[i], NULL);

        if (join_error != 0) {
            fprintf(stderr, "pthread_join: %s\n", strerror(join_error));
            client_error = 1;
        }
        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;
        total_timeouts += thread_data[i].timeout_cnt;
    }

    for (int i = 0; i < num_client_threads; i++) {
        close_if_open(&thread_data[i].socket_fd);
        close_if_open(&thread_data[i].epoll_fd);
    }

    if (client_error) {
        free(threads);
        free(thread_data);
        exit(EXIT_FAILURE);
    }

    printf("Total TX packets: %lld\n", total_tx);
    printf("Total RX packets: %lld\n", total_rx);
    printf("Total timeout events: %lld\n", total_timeouts);
    printf("Total effective packet loss (TX-RX): %lld\n", total_tx - total_rx);

    for (int i = 0; i < num_client_threads; i++) {
        if (thread_data[i].saw_timeout) {
            printf("Thread %d last timeout window: [%u, %u)\n",
                   i,
                   thread_data[i].last_timeout_start,
                   thread_data[i].last_timeout_end);
        }
    }

    free(threads);
    free(thread_data);
}

static void run_server(void) {
    int server_fd = -1;
    int epoll_fd = -1;
    struct sockaddr_in server_addr;
    struct epoll_event event;
    struct epoll_event events[MAX_EVENTS];
    server_client_state_t *client_states = NULL;

    server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        close_if_open(&server_fd);
        exit(EXIT_FAILURE);
    }

    if (set_nonblocking(server_fd) == -1) {
        perror("fcntl server socket O_NONBLOCK");
        close_if_open(&server_fd);
        exit(EXIT_FAILURE);
    }

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        close_if_open(&server_fd);
        exit(EXIT_FAILURE);
    }

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        perror("epoll_ctl add server_fd");
        close_if_open(&server_fd);
        close_if_open(&epoll_fd);
        exit(EXIT_FAILURE);
    }

    while (1) {
        int n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        if (n_events < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n_events; i++) {
            if ((events[i].events & EPOLLIN) == 0) {
                continue;
            }

            while (1) {
                packet_frame_t packet;
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int recv_status = recv_packet_from(server_fd, &packet, &client_addr, &client_len);

                if (recv_status > 0) {
                    handle_server_packet(server_fd, &client_states, &packet, &client_addr, client_len);
                    continue;
                }

                if (recv_status == -2) {
                    continue;
                }

                if (recv_status < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }

                if (recv_status < 0) {
                    perror("recvfrom");
                    break;
                }

                if (recv_status == 0) {
                    continue;
                }
            }
        }
    }

    free_client_states(client_states);
    close_if_open(&server_fd);
    close_if_open(&epoll_fd);
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) {
            server_ip = argv[2];
        }
        if (argc > 3) {
            server_port = atoi(argv[3]);
        }

        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) {
            server_ip = argv[2];
        }
        if (argc > 3) {
            server_port = atoi(argv[3]);
        }
        if (argc > 4) {
            num_client_threads = atoi(argv[4]);
        }
        if (argc > 5) {
            num_requests = atoi(argv[5]);
        }

        if (num_client_threads <= 0 || num_requests < 0) {
            fprintf(stderr, "num_client_threads must be > 0 and num_requests must be >= 0\n");
            return EXIT_FAILURE;
        }

        run_client();
    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n",
               argv[0]);
    }

    return 0;
}
