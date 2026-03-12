CS371 PA2: Error & Flow Control in RDT
======================================

This repository contains the code for **CS 371: Introduction to Computer Networking – Programming Assignment 2 (PA2)** at the University of Kentucky (Spring 2026). The goal is to implement **error control** and **flow control** for reliable data transfer over **UDP** using C.

You will extend your PA1 client–server code to:

- **Task 1**: Implement a **UDP-based pipelined protocol** and measure packet loss.
- **Task 2**: Add **sequence numbers, ACKs, timeouts, and retransmission (Go-Back-N)** to eliminate packet loss.


Project Structure
-----------------

- `task1.c`: UDP client/server implementing:
  - UDP-based **pipelined protocol** (multiple in-flight packets).
  - Per-packet timers on the client.
  - Metrics collection: transmitted vs. received packets and inferred packet loss.
- `task2.c`: UDP client/server implementing:
  - **Sequence numbers (SN)** and ACKs.
  - **Go-Back-N–style retransmission** using timeouts.
  - Multiple in-flight packets with **no effective packet loss**.

Build Instructions
------------------

The assignment is evaluated on **Ubuntu 22.04 (x86)**. Make sure your code builds and runs there (e.g., CloudLab).

Example build commands:

- **Task 1**:

```bash
gcc -o pa2_task1 task1.c -pthread
```

- **Task 2**:

```bash
gcc -o pa2_task2 task2.c -pthread
```

Command-Line Arguments
----------------------

Both Task 1 and Task 2 programs are expected to use the same basic argument pattern (as in PA1):

```bash
./pa2_taskX <server|client> <server_ip> <server_port> <num_client_threads> <num_requests>
```

- `<server|client>`: Run in server mode or client mode.
- `<server_ip>`: IPv4 address of the server (e.g., `127.0.0.1`).
- `<server_port>`: UDP port the server listens on.
- `<num_client_threads>`: Number of concurrent client threads.
- `<num_requests>`: Number of requests each client thread sends.


Task 1: UDP-Based Pipelined Protocol
------------------------------------

**Goal**: Observe packet loss when UDP is saturated and measure it.

Main requirements:

- Use **UDP sockets** (not TCP).
- Extend the PA1 **stop-and-wait** protocol to a **pipelined** protocol:
  - Allow multiple in-flight, yet-to-be-ACKed packets from each client thread.
  - Use a per-packet timer (or epoll with timeouts) on the client side.
- Track metrics on each client thread:
  - `tx_cnt`: number of packets sent (original transmissions only).
  - `rx_cnt`: number of packets successfully received from server.
  - `lost_pkt_cnt = tx_cnt - rx_cnt`.
- When you increase:
  - The number of clients (`num_client_threads`), and/or
  - The number of in-flight packets (pipeline concurrency),
  you should be able to **observe packet loss** once the server is overloaded.

Typical usage examples:

```bash
# Start server
./pa2_task1 server 0.0.0.0 9000 0 0

# Start client with 4 threads, 1000 requests each
./pa2_task1 client 127.0.0.1 9000 4 1000
```

Task 2: UDP-Based SN, ACK, and Retransmission (Go-Back-N)
---------------------------------------------------------

**Goal**: Eliminate effective packet loss using sequence numbers, ACKs, and retransmissions.

Main requirements:

- Build on Task 1 (still **UDP** and **pipelined**).
- Implement:
  - **Sequence numbers (SN)** for each packet.
  - **ACKs** from server back to client.
  - **Timeout-based retransmission** of unacknowledged packets.
  - A **Go-Back-N–style sliding window**.
- Hints from the assignment:
  - You may use **epoll’s built-in timer** or your own timer management.
  - When retransmitting a packet, you **do not** increment `tx_cnt`. Only the initial send counts.
  - Verify correctness by checking that **`tx_cnt == rx_cnt`** (no effective loss).

Expected behavior:

- As you increase `num_client_threads` and the pipeline depth, **no packet loss** should be observed (`lost_pkt_cnt` should stay at 0 when the protocol is configured correctly).

Typical usage examples:

```bash
# Start server
./pa2_task2 server 0.0.0.0 9000 0 0

# Start client with 8 threads, 5000 requests each
./pa2_task2 client 127.0.0.1 9000 8 5000
```

Metrics and Output
------------------

Your client program should, at minimum, log or print:

- Per-thread or aggregated:
  - `tx_cnt`
  - `rx_cnt`
  - `lost_pkt_cnt = tx_cnt - rx_cnt`
- For Task 2, show that **`tx_cnt == rx_cnt`** across runs with large numbers of clients and requests.

Development & Testing Environment
---------------------------------

- **Required**: Test on **Ubuntu 22.04 x86** (e.g., NSF CloudLab).
- Suggested workflow:
  1. Develop locally (Linux VM or WSL).
  2. Push to GitHub.
  3. Clone and test on CloudLab before submitting.

Submission Notes
----------------

- Your GitHub repository should contain at least:
  - Source code for **Task 1**.
  - Source code for **Task 2**.
- The graders will:
  - Build your code using `gcc` with `-pthread`.
  - Run the server and client as documented above.
  - Check that sockets and epoll file descriptors are properly closed.
  - Check that error cases are handled gracefully (failed sockets, epoll failures, etc.).
