# Oral Examination (Viva Voce) Guide: Adaptive IPC Backpressure & Fairness Control

This comprehensive guide is designed to prepare you for technical questions, deep architectural inquiries, and theoretical evaluations during project defense, viva voce, and course presentations.

---

## 1. Core Operating Systems & Networking Concepts

### Q1: Why did you choose UNIX Domain Sockets (`AF_UNIX`) instead of shared memory, pipes, or POSIX message queues?
**Answer:**
- **UNIX Domain Sockets (`AF_UNIX`)** operate entirely within the Linux VFS/socket layer in kernel memory without network stack overhead (no IP routing, checksumming, or NIC virtualization).
- Unlike **anonymous pipes (`pipe()`)** or **named FIFOs (`mkfifo`)**, UNIX domain sockets support full-duplex communication and allow multiple concurrent client connections multiplexed through standard file descriptors.
- Unlike **POSIX Shared Memory (`shm_open`, `mmap`)**, which requires complex lock-based or lock-free concurrency control (mutexes, semaphores, atomic ring buffers) and lacks built-in kernel signaling for poll/epoll wakeups, `AF_UNIX` integrates seamlessly with `poll()` for event-driven asynchronous I/O.
- Crucially, `AF_UNIX` supports passing file descriptors, client credentials (`SO_PEERCRED`), and connection-oriented sequenced packet delivery.

### Q2: What is `SOCK_SEQPACKET` and why is it superior to `SOCK_STREAM` and `SOCK_DGRAM` for this project?
**Answer:**
Linux supports three primary socket types for `AF_UNIX`:
1. `SOCK_STREAM`: Provides a continuous byte stream. It does **not** preserve message boundaries. If a sender transmits 100 bytes and then 200 bytes, the receiver might read all 300 bytes in a single `read()`, or 50 bytes in the first read. To use stream sockets for discrete messages, user space must implement custom framing (length prefixes or delimiter scanning).
2. `SOCK_DGRAM`: Connectionless, preserves packet boundaries, but does not provide connection state tracking, flow control, or in-order sequencing guarantees.
3. **`SOCK_SEQPACKET`**: **Combines the best of both worlds**:
   - **Sequenced and Reliable**: In-order, reliable delivery (like `SOCK_STREAM`).
   - **Message Boundaries Preserved**: Every `recv()` call returns exactly one discrete message as sent by `send()`. If the user passes a buffer smaller than the packet, the remainder is truncated with `MSG_TRUNC`, never smeared into the next message.
   - **Connection-Oriented**: Provides explicit `connect()`, `accept()`, and disconnect detection (`recv() == 0`).

### Q3: Why did you use `poll()` instead of multi-threading or `epoll()`?
**Answer:**
- **Single-Threaded Event Loop**: Spawning a thread per sender introduces locking overhead, race conditions, cache ping-ponging, and high memory footprints. A single-threaded event loop based on I/O multiplexing guarantees deterministic sequential ordering and eliminates mutex contention.
- **`poll()` vs `epoll()`**: `epoll()` has $O(1)$ scaling when monitoring tens of thousands of idle connections (e.g., in web servers). For an OS IPC broker with up to 8–16 tightly coupled local processes that are actively transmitting, `poll()` has lower kernel setup overhead, zero state maintenance across syscalls, and operates in nanoseconds. Our benchmark proved average controller overhead of only $0.2–0.4\ \mu\text{s}$ per tick.

---

## 2. Flow Control, Credit Reservation & Resource Limits

### Q4: What is Backpressure and how does credit-based flow control differ from packet-dropping?
**Answer:**
- **Backpressure** is a flow control mechanism where downstream congestion (a slow receiver or full queue) exerts backward pressure on upstream senders to slow down their admission rate.
- **Packet-Dropping Schemes** (e.g., TCP RED, CoDel, Tail-Drop): The broker admits packets blindly until its buffer fills, then drops packets. This wastes sender CPU, kernel buffer bandwidth, and requires expensive retransmission loops.
- **Credit-Based Flow Control**: The broker explicitly grants transmission tokens ("credits") to senders. A sender is **never permitted to transmit a data message without an available credit**. Therefore:
  $$\text{Transmission Permitted} \iff \text{Credits Available} > 0$$
  This prevents buffer overflow *at the source* before packets enter the kernel socket buffer.

### Q5: What is the "Non-Revocable Credit Withholding" invariant?
**Answer:**
- When the broker grants credits to a sender, that capacity is **reserved upfront**.
- If the adaptive controller later detects congestion and lowers the global budget (e.g., from 64 to 20 credits), the broker **cannot revoke credits already granted** because the sender may have already transmitted or queued those packets.
- Instead, the broker enforces **credit withholding**:
  $$\text{If } W_{\text{outstanding}} \ge B_{\text{current}} \implies \text{New Grants} = 0$$
  The broker withholds all new credit grants until the receiver processes messages and returns `MSG_ACK`s, reducing $W_{\text{outstanding}}$ below $B_{\text{current}}$. This guarantees that the broker's queue capacity is never exceeded even during rapid congestion onset.

### Q6: What hard resource limits are enforced, and why are they independent of the controller?
**Answer:**
The broker enforces three hard bounds in `src/queue.c`:
1. **Per-Sender Queue Limit**: $\le 64$ messages.
2. **Global Queue Capacity**: $\le 256$ messages.
3. **Global Queued Memory**: $\le 1,048,576$ bytes (1 MiB).
These are **hard safety bounds** that run in the data path ($O(1)$) independently of the controller. If a rogue sender ignores the protocol and floods the broker without credits, the hard enforcer immediately rejects the packet, protecting other senders and preventing memory exhaustion.

---

## 3. Adaptive Control Theory: EWMA, Hysteresis & Gradual Recovery

### Q7: How does the controller estimate arrival and completion rates using EWMA?
**Answer:**
Every 50 ms ($\Delta t$), the broker measures raw arrival count $A$ and receiver completion count $C$:
$$M_{\text{in}} = \frac{A}{\Delta t}, \quad M_{\text{comp}} = \frac{C}{\Delta t}$$
It filters these noisy instantaneous measurements using an **Exponentially Weighted Moving Average (EWMA)** with smoothing factor $\alpha = 0.20$:
$$R_{\text{in}}^{(t)} = 0.20 \cdot M_{\text{in}} + 0.80 \cdot R_{\text{in}}^{(t-1)}$$
$$R_{\text{comp}}^{(t)} = 0.20 \cdot M_{\text{comp}} + 0.80 \cdot R_{\text{comp}}^{(t-1)}$$
- $\alpha = 0.20$ gives $80\%$ weight to historical trends and $20\%$ weight to the latest interval. This filters out momentary micro-bursts while responding decisively to sustained workload shifts within 2–3 intervals (100–150 ms).

### Q8: What is the Rate Imbalance Ratio $\rho$?
**Answer:**
$$\rho = \frac{R_{\text{in}}}{R_{\text{comp}} + 1.0}$$
(The $+1.0$ in the denominator prevents division by zero when the receiver is completely idle or stalled).
- $\rho \approx 1.0$: Ingress matches egress; system is balanced.
- $\rho > 1.30$: Ingress exceeds egress by over $30\%$; queue buildup is mathematically guaranteed if admission is not throttled.
- $\rho < 0.80$: Receiver is clearing messages faster than arrivals; channel has spare headroom.

### Q9: Explain the Three Controller States and Hysteresis.
**Answer:**
1. **`STATE_NORMAL`**: Queue utilization is low ($<30\%$) and arrival matches completion. The controller applies **Gradual Additive Recovery**, increasing the admission budget by $+2, +4, +6 \dots$ up to $B_{\text{max}} = 128$.
2. **`STATE_CAUTION`**: Queue utilization reaches $\ge 50\%$, or moderate utilization ($\ge 30\%$) with rate imbalance ($\rho > 1.10$). The controller stabilizes the budget around the measured receiver completion rate ($R_{\text{comp}} \times 100\text{ms}$).
3. **`STATE_OVERLOAD`**: Severe congestion: queue utilization $\ge 75\%$, or $\ge 50\%$ with severe imbalance ($\rho > 1.30$). The controller triggers **Multiplicative Decrease**:
   $$B \leftarrow \max(B_{\text{min}}, B \times 0.65)$$
- **Hysteresis**: Escalation to a higher congestion state happens **immediately** (within 1 tick) to protect against buffer overflow. However, de-escalation to a lower state requires **2 consecutive low-congestion intervals** ($\ge 100\text{ ms}$). This prevents rapid, oscillatory flapping between states ("chattering").

---

## 4. Scheduling, Fairness & The Root-Cause Discrepancy

### Q10: Why did DRR initially fail to outperform FIFO in live benchmarks ($J \approx 0.6286$ vs $J \approx 0.9989$ in unit tests)?
**Answer:**
An independent audit identified five interlocking structural root causes:
1. **Workload Inequity in the Benchmark**: Senders 0 and 1 were each configured to send 250 messages. Sender 0 sent 512B ($138\text{ KiB}$) while Sender 1 sent 4096B ($1034\text{ KiB}$). Since Sender 0 only offered 1/8th of the bytes, no scheduler could dispatch bytes that did not exist. Over the full run, theoretical maximum fairness was capped at $J = 0.6311$.
2. **Blocking Sockets in Broker Event Loop**: `accept()` did not set client sockets to non-blocking. The broker limited reads to 1 message per tick to avoid hanging, preventing queues from accumulating backlogs.
3. **Premature Deficit Counter Reset**: Because queues held only 1 message, `queue_sender_has_messages()` returned false immediately upon dequeue, causing the scheduler to wipe `deficit_counter = 0`. DRR degenerated into 1:1 packet round-robin.
4. **Destructive Exit Cleanup**: When a sender exited (`MSG_STOP`), `broker.c` invoked `scheduler_reset_sender()`, which wiped `dispatched_bytes = 0`, corrupting the final fairness summary.
5. **Packet Credits vs. Byte Consumption**: The broker allocated equal *message credits* (e.g. 32 pkts). 32 pkts for 4096B gave Sender 1 $132.3\text{ KiB}$, while 32 pkts for 512B gave Sender 0 only $17.6\text{ KiB}$. Once Sender 0 exhausted its 32 credits, its queue emptied and DRR was forced to dispatch Sender 1's remaining packets.

### Q11: How was this fairness discrepancy resolved?
**Answer:**
Four architectural fixes were implemented:
1. **Byte-Equalized Credit Budgeting**: Senders are granted credits based on a fixed byte quota target ($35.3\text{ KiB}$):
   $$\text{Credits}_i = \left\lfloor \frac{35,328}{\text{Frame Size}_i} \right\rfloor$$
   This grants 64 credits to 512B senders ($35.3\text{ KiB}$) and 8 credits to 4096B senders ($33.1\text{ KiB}$), equalizing available byte volume.
2. **Non-Blocking Sockets & Greedy Ingress Draining**: Sockets are switched to non-blocking, and an ingress loop drains up to 64 messages per tick until `-EAGAIN`.
3. **Deficit Persistence Across Rounds**: Deficit counters persist across packet dequeues within each active round (decrementing by packet length) and are reset to 0 immediately when a queue becomes truly empty (Shreedhar–Varghese standard).
4. **Statistical Preservation**: Sender dispatch counters are preserved throughout broker runtime upon `MSG_STOP`.
5. **Two Distinct Experiments**:
   - **Experiment A (Equal Offered Byte Volume)**: Both senders submit 204.8 KiB (400 pkts $\times$ 512B vs 50 pkts $\times$ 4096B) and run to completion ($J = 0.9989$).
   - **Experiment B (Continuous Backlog)**: Both senders remain continuously backlogged (500 msgs vs 250 msgs), synchronized via a deterministic two-way rendezvous POSIX pipe barrier, and evaluated over 200 concurrent dispatches. In Config A (FIFO), $J = 0.6294$ ($0.6950$ in Config C, $0.7106$ in Config B); in Config D (DRR), $J = \mathbf{1.0000}$ ($0.99997$) with near-perfect byte parity (176.67 small pkts vs 23.33 large pkts; individual runs: 176/24, 176/24, 178/22). This demonstrates DRR byte equity for this specific continuous-backlog experiment, without claiming universal fairness for all arbitrary workloads.

### Q12: How does Deficit Round-Robin (DRR) work mechanically?
**Answer:**
DRR (Shreedhar & Varghese) assigns each sender a byte **quantum** $Q$ (set here to $4136\text{ bytes} \ge \text{MaxPacketSize}$) and maintains a **deficit counter**:
1. When visiting Sender $i$, add quantum:
   $$\text{deficit}[i] \leftarrow \text{deficit}[i] + Q$$
2. While $\text{deficit}[i] \ge \text{head\_packet\_size}$:
   - Dequeue and dispatch the packet.
   - Deduct packet size from deficit:
     $$\text{deficit}[i] \leftarrow \text{deficit}[i] - \text{head\_packet\_size}$$
3. When $\text{deficit}[i] < \text{head\_packet\_size}$, move to the next active sender.
4. **Dispatch Ratio**:
   - Sender 0 (552B frame): $4136 / 552 \approx 7.49$ packets per round.
   - Sender 1 (4136B frame): Exactly 1 packet per round.
   Both flows transfer identical byte bandwidth per scheduling round.

---

## 5. Fault Tolerance & Systems Programming Details

### Q13: How does the system handle a rogue sender that floods the broker without credits?
**Answer:**
`bin/sender` supports a `--rogue` (`-X`) flag for testing protocol violations. When active, it bypasses credit checks and sends continuously.
- In `src/broker.c`, when a data message arrives, the broker checks whether the sender's queue has space (`count < 64`) and whether global limits permit it.
- If the sender violates credit allocations, its queue fills up to 64. The 65th message is rejected with `QUEUE_ERR_SENDER_FULL`.
- The broker does not crash, does not block, and does not disconnect other healthy senders. Validated in `tests/test_robustness.c`.

### Q14: How does the system detect and recover from abrupt sender disconnects?
**Answer:**
When a sender process crashes or closes its socket:
1. `poll()` reports `POLLIN` or `POLLHUP` on the sender's file descriptor.
2. `ipc_recv_msg()` calls `recv()`, which returns `0` (clean EOF) or `-ECONNRESET`.
3. The broker frees any remaining queued nodes in `qm`, closes the socket, and reclaims memory. Cumulative throughput metrics remain preserved in `scheduler` for auditing.

### Q15: How is message integrity verified?
**Answer:**
Every message includes a 32-bit Fowler–Noll–Vo (FNV-1a) hash computed over the variable payload:
```c
uint32_t hash = 2166136261u;
for (size_t i = 0; i < len; i++) {
    hash ^= (uint8_t)data[i];
    hash *= 16777619u;
}
```
The receiver recalculates this checksum on every arrived message. Across all 24 benchmark runs, zero integrity errors were recorded.

### Q16: How is byte accounting defined, and why do mean message counts in the summary report have decimal values while each individual run has integer message counts?
**Answer:**
1. **Wire Frame Byte Accounting**: Byte accounting uniformly and strictly measures complete DATA wire frames:
   $$\text{frame\_size} = \text{sizeof}(IpcHeader)\ [40\text{ bytes}] + \text{payload\_len}$$
   - Small message (512B payload): $40 + 512 = 552$ bytes.
   - Large message (4096B payload): $40 + 4096 = 4136$ bytes.
2. **Individual Run Mathematical Exactness**: In every run, delivered bytes strictly equal $\text{messages} \times \text{frame\_size}$:
   - Run 1 Config D: Sender 0 delivered 176 msgs ($176 \times 552 = 97,152\text{ B}$), Sender 1 delivered 24 msgs ($24 \times 4136 = 99,264\text{ B}$).
   - Run 2 Config D: Sender 0 delivered 176 msgs ($176 \times 552 = 97,152\text{ B}$), Sender 1 delivered 24 msgs ($24 \times 4136 = 99,264\text{ B}$).
   - Run 3 Config D: Sender 0 delivered 178 msgs ($178 \times 552 = 98,256\text{ B}$), Sender 1 delivered 22 msgs ($22 \times 4136 = 90,992\text{ B}$).
3. **Statistical Aggregation**: The benchmark summary report records the exact arithmetic mean across the 3 independent runs:
   - Sender 0 mean: $(176 + 176 + 178)/3 = 176.67\text{ msgs}$, mean bytes $= 97,520.0\text{ B}$ ($176.666667 \times 552 = 97,520.0$).
   - Sender 1 mean: $(24 + 24 + 22)/3 = 23.33\text{ msgs}$, mean bytes $= 96,506.7\text{ B}$ ($23.333333 \times 4136 = 96,506.67$).
   - Recalculated Jain's Fairness on mean byte volume:
     $$J = \frac{(97,520.0 + 96,506.67)^2}{2 \times (97,520.0^2 + 96,506.67^2)} = 1.0000 \quad (0.99997)$$
   This proves that DRR achieves true byte bandwidth equality across asymmetric flows.

### Q17: Why did the live benchmark record 0 controller state transitions (`state_changes = 0`) across all scenarios, including the receiver stall in Scenario 6?
**Answer:**
1. **Activation Thresholds**: In `src/controller.c`, transitioning from `STATE_NORMAL` to `STATE_CAUTION` or `STATE_OVERLOAD` requires global queue utilization to reach $\ge 30\%$ (77 messages) or $\ge 50\%$ (128 messages) of `GLOBAL_QUEUE_CAP = 256`.
2. **In-Flight Credit Bounds**: Senders are restricted by credit allocations ($\le 16$ messages per sender). Across 3 active senders, the maximum possible outstanding in-flight messages at any time is $3 \times 16 = 48$ messages ($18.75\%$ utilization).
3. **Credit Depletion Backpressure**: When the receiver stalled for 250 ms in Scenario 6, senders sent up to their credit limit and then blocked on `ipc_send_msg()` waiting for receiver ACKs. Because the queue never exceeded 50 messages ($< 30\%$), the controller remained in `STATE_NORMAL`.
4. **Empirical Takeaway**: Backpressure protection in the live benchmark was maintained through credit exhaustion rather than EWMA state machine transitions. The benchmark does not demonstrate an actual dynamic transition into `STATE_OVERLOAD`.

### Q18: Does adaptive backpressure (Config C/D) double throughput compared to fixed window (Config A)?
**Answer:**
No. In the live 3-run experimental evaluation of Scenario 1 (Normal Traffic), all four configurations achieved approximately equivalent throughput:
- Config A: $3564.8 \pm 8.8\text{ msgs/s}$
- Config B: $3555.6 \pm 14.5\text{ msgs/s}$
- Config C: $3567.9 \pm 6.6\text{ msgs/s}$
- Config D: $3559.8 \pm 22.9\text{ msgs/s}$

Config A exhibited lower measured transit latency ($p99 = 5615.4\ \mu\text{s}$ vs $11,200.7 - 15,057.1\ \mu\text{s}$) because its static window restricted queue buildup. Config D's primary demonstrated advantage is byte fairness under continuous asymmetric backlog (Scenario 5), where it delivers $J = 1.0000$ (exact: $0.99997$) compared to $J = 0.6294$ for Config A.

### Q19: What is the computational time complexity of the Deficit Round-Robin (DRR) packet scheduler?
**Answer:**
- **Amortized Time Complexity**: **$O(1)$ per packet**.
- **Theoretical Basis**: In classic Fair Queueing (e.g., GPS or WFQ), packets must be ordered by simulated virtual finish times, requiring an $O(\log N)$ priority queue or heap insertion where $N$ is active flows.
- **DRR Advantage**: Shreedhar and Varghese (SIGCOMM 1995) proved that if the quantum $Q$ is chosen such that $Q \ge \text{MaxPacketSize}$ (in our system, $Q = 4136\text{ bytes} \ge 4136\text{ bytes}$), **every backlogged flow visited in a round is guaranteed to transmit at least one packet**.
- Therefore, the number of empty queue inspections or round-robin pointer advancements is strictly bounded by the number of packets serviced. Dequeuing, decrementing the deficit, and pointer advancement are constant-time array/pointer operations, achieving true $O(1)$ per-packet scheduling.

### Q20: Why choose Credit-Based Backpressure over a TCP-style Sliding Window?
**Answer:**
1. **Upfront Buffer Reservation**: In credit-based control, a credit is an explicit token that reserves buffer slots *before* transmission occurs. The broker guarantees that total outstanding credits across all senders never exceed buffer capacity:
   $$\sum_{i=1}^N \text{GrantedCredits}_i \le \text{GlobalCapacity}$$
2. **Deterministic Withholding vs Retraction**: In TCP sliding windows, shrinking an already-advertised window ("reneging") is heavily discouraged by RFC 793 because in-flight data may already be transmitted, leading to spurious retransmits and dropped frames. In credit-based backpressure, granted credits are non-revocable; when the controller detects congestion, it simply withholds new grants until outstanding packets drain, avoiding window renegotiation race conditions.
3. **Zero Retransmission Overhead**: Unlike TCP windowing which relies on timeouts and segment drops to signal congestion, user-space credit backpressure eliminates packet loss entirely (0 drops across all 72 scenario runs).

### Q21: What are the trade-offs between User-Space IPC mediation and In-Kernel IPC (e.g., Android Binder, kdbus)?
**Answer:**
- **User-Space IPC (Our Architecture)**:
  - *Advantages*:
    1. **Policy Agility**: Flow control algorithms, EWMA smoothing parameters, hysteresis thresholds, and DRR quanta can be dynamically modified and tuned without recompiling the Linux kernel.
    2. **Fault Isolation**: A fatal bug or assertion failure in the user-space broker terminates only the broker process; the Linux kernel remains completely unharmed and stable.
    3. **Observability & Tooling**: Easily analyzed and profiled with standard user-space debugging tools (AddressSanitizer, UBSan, `gprof`, `perf`).
    4. **Zero Root Privilege**: Runs as an unprivileged user process over standard POSIX UNIX domain sockets.
  - *Trade-Offs*: Requires 2 IPC hops (Sender $\to$ Kernel $\to$ Broker $\to$ Kernel $\to$ Receiver), incurring 4 context switches per message instead of 2 for direct peer-to-peer or kernel-mediated IPC.
- **In-Kernel IPC (e.g., Android Binder)**:
  - *Advantages*: Can implement single-copy (via kernel buffer mapping) or zero-copy IPC; direct thread pool scheduling and priority inheritance.
  - *Trade-Offs*: High kernel complexity; kernel security attack surface; rigid scheduling policies; kernel crashes cause whole-system kernel panics.

---

## 6. Summary of Key Experimental Numbers to Memorize

| Metric | Fixed Window (Config A) | Adaptive EWMA (Config C) | Adaptive + DRR (Config D) |
| :--- | :---: | :---: | :---: |
| **Normal Throughput** | 3564.8 msgs/s | 3567.9 msgs/s | **3559.8 msgs/s** |
| **Exp A Byte Fairness ($J$)** | 0.9989 | 0.9989 | **0.9989** |
| **Exp B Backlog Fairness ($J$)** | 0.6294 (FIFO bias) | 0.6950 (FIFO bias) | **1.0000 (DRR Equity)** |
| **Burst Peak Queue** | 27 msgs | 24 msgs | **33 msgs** |
| **Stall Tail Latency $p99$** | 255.8 ms | 261.8 ms | **262.0 ms** |
| **Controller CPU Overhead** | 0.248 $\mu$s/tick | 0.463 $\mu$s/tick | **0.464 $\mu$s/tick** |
| **Packet Loss / Drops** | 0 | 0 | **0** |
