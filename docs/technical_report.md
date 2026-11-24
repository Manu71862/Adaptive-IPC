# Technical Report: Adaptive IPC Backpressure and Fairness Control in Linux

**Course Project:** Advanced Operating Systems<br/>
**Evaluation Platform:** Linux x86_64 (Kernel 6.11), GCC 13.3.0, `AF_UNIX` (`SOCK_SEQPACKET`)<br/>
**Implementation Language:** Pure C / C++ (Zero Python dependencies for core execution)<br/>
**Verification Status:** 140/140 Unit and Robustness Tests Passing; ASan/UBSan Clean; Static Analyzer Clean<br/>

---

## 1. Abstract

Inter-Process Communication (IPC) is the foundational substrate of modern operating systems, microkernel architectures, and containerized runtime environments. When multiple producer processes concurrently transmit messages through a shared broker, uncoordinated transmission produces queue buildup, bufferbloat, unpredictable tail latencies, and unfair bandwidth allocation. Conventional mitigations—such as static socket buffer limits or binary threshold backpressure—suffer from limit-cycle oscillations and fail to prevent byte starvation when flows carry heterogeneous message sizes.

This paper presents the design, implementation, formal audit, and empirical evaluation of an original, high-performance user-space IPC architecture in pure C for Linux. The system integrates:
1. **Exponentially Weighted Moving Average (EWMA) Rate Sensing**: Continuous estimation of ingress arrival rate ($R_{\text{in}}$) and receiver completion rate ($R_{\text{comp}}$) with smoothing factor $\alpha = 0.20$ to detect channel imbalance before queues fill.
2. **Three-State Hysteresis Rate Controller**: Dynamic admission modulation between `STATE_NORMAL`, `STATE_CAUTION`, and `STATE_OVERLOAD` with hold timers and gradual additive recovery.
3. **Non-Revocable Credit Withholding**: Upfront buffer reservation preventing queue overflow during sudden congestion onset.
4. **Byte-Equalized Credit Budgeting**: Quotas based on byte volume rather than packet count, eliminating the structural bias against small packets.
5. **Deficit Round-Robin (DRR) Fair Scheduling**: $O(1)$ amortized packet scheduling enforcing byte-level allocation equity ($Q = 4136\text{ bytes}$) with standard Shreedhar & Varghese deficit-reset semantics.

The implementation is validated by a 140-assertion unit and robustness test suite, verified clean under AddressSanitizer, UndefinedBehaviorSanitizer, and `gcc -fanalyzer`, and evaluated across 24 distinct experimental permutations (4 configurations $\times$ 6 scenarios) over 3 independent evaluation runs ($N=3$, 72 total scenario executions). Key empirical findings include:
- **Steady-State Throughput Equivalence**: Under symmetric normal traffic, all configurations achieve approximately equivalent throughput ($3555 - 3568\text{ msgs/s}$, $\approx 3.62\text{ MiB/s}$), demonstrating that adaptive control incurs negligible overhead ($0.21 - 0.58\ \mu\text{s}$ per tick). Config A achieves lower steady-state latency ($p99 = 5.62\text{ ms}$ vs $11.20 - 15.06\text{ ms}$) due to static windowing preventing queue accumulation.
- **Resolution of FIFO Byte Starvation**: Under continuous concurrent backlog with asymmetric message sizes (512B vs 4096B payloads), strict FIFO scheduling degrades to $J = 0.6294$ (Config A) and $J = 0.6950$ (Config C). In contrast, DRR with byte-equalized credits achieves near-perfect byte parity ($J = \mathbf{1.0000}$, exact: $0.99997$), dynamically dispatching 176.67 small packets to 23.33 large packets ($97.5\text{ KiB}$ vs $96.5\text{ KiB}$ delivered mean), matching the theoretical $7.49:1$ inverse packet ratio.
- **Credit Depletion Backpressure**: During an induced 250 ms receiver stall, credit exhaustion acts as the primary backpressure mechanism, bounding peak queue depth to 21–50 messages with zero dropped packets across all runs.

---

## 2. Introduction

Modern multiprocess systems depend on high-throughput, low-latency IPC to coordinate microservices, graphics compositors (e.g., Wayland), system daemons (e.g., `systemd`, D-Bus), and container runtimes. In a typical broker-mediated topology, multiple sender processes submit discrete request messages destined for one or more consumer processes through an intermediate dispatch broker.

When producer processes generate traffic faster than consumers can process it, queues expand. In the absence of proactive backpressure, this disparity leads to **bufferbloat**: queues fill to capacity, introducing excessive transit delays for newly arrived packets. When buffers eventually exhaust, operating systems either block senders indefinitely or drop messages, precipitating cascading timeouts and expensive application-layer retries.

Furthermore, real-world IPC workloads are inherently **heterogeneous**. Latency-sensitive telemetry, heartbeats, and control messages typically carry small payloads (e.g., 64–512 bytes), whereas bulk transfers, file transfers, and video frames carry large payloads (e.g., 4096 bytes or larger). Standard FIFO dispatch schedulers schedule packets on a packet-by-packet basis, allocating equal *packet slots* rather than equal *bandwidth*. Consequently, large-packet flows capture up to $8\times$ more transmission bandwidth, subjecting small, high-priority control flows to severe throughput starvation and head-of-line blocking.

### Core Contributions
1. **Pure C Linux Architecture**: An unprivileged user-space IPC daemon and client library utilizing Sequenced Packet UNIX Domain Sockets (`AF_UNIX`, `SOCK_SEQPACKET`), guaranteeing reliable, in-order message delivery with preserved packet boundaries and zero external dependencies.
2. **Adaptive EWMA Backpressure**: A feedback rate controller that monitors arrival and service rates at 50 ms intervals, utilizing hysteresis to avoid oscillatory limit cycles and non-revocable credit withholding to prevent buffer overruns.
3. **Byte-Equalized Credit Budgeting**: A flow control scheme that dimensions credit limits inversely to frame size, ensuring that all senders receive equivalent byte admission capacity.
4. **$O(1)$ Deficit Round-Robin Scheduling**: A fair queueing implementation compliant with Shreedhar & Varghese semantics, featuring deficit persistence during active backlogs and immediate deficit zeroing upon queue exhaustion.
5. **Deterministic Benchmark Suite & Publication Visualization**: A comprehensive testbed evaluating 6 stress scenarios across 4 configurations with POSIX pipe rendezvous synchronization, automated statistical aggregation, and offline visualization generating 7 publication-quality plots.

---

## 3. Problem Statement & Linux IPC Bottlenecks

### 3.1 Limitations of Existing Linux IPC Mechanisms
Linux provides several native user-space IPC primitives, each presenting fundamental trade-offs:
- **Pipes and FIFOs (`pipe()`, `mkfifo`)**: Provide unidirectional byte streams. They lack native support for discrete message boundaries, requiring application-layer framing. Furthermore, pipes cannot multiplex multiple client connections through a single file descriptor without race conditions, and block writers when the fixed 64 KiB kernel buffer fills.
- **POSIX Message Queues (`mq_open`)**: Support discrete, prioritized messages, but enforce restrictive system-wide limits (`/proc/sys/fs/mqueue/msg_max`, default 10 messages) that require elevated privileges to modify. They do not support dynamic feedback rate control or non-blocking event loop integration via standard `poll()`.
- **Shared Memory (`shm_open`, `mmap`)**: Provides zero-copy throughput, but requires complex user-space synchronization primitives (mutexes, semaphores, or atomic lock-free rings). Shared memory lacks asynchronous event notification; polling threads consume excessive CPU, while eventfd signaling reintroduces system call overhead.
- **UNIX Domain Sockets (`AF_UNIX`)**:
  - `SOCK_STREAM`: Provides reliable, bidirectional byte streams, but lacks message framing.
  - `SOCK_DGRAM`: Preserves packet boundaries, but lacks connection tracking, flow control, and ordered delivery guarantees.
  - `SOCK_SEQPACKET`: Combines reliable, connection-oriented, sequenced delivery with discrete message boundaries. A single `recv()` call returns exactly one discrete message. This makes `SOCK_SEQPACKET` the optimal substrate for discrete IPC messaging.

### 3.2 The Bufferbloat and Oscillation Dilemma
In static-window flow control (**Config A**), each sender is granted a fixed number of in-flight message credits ($W = 16$). While simple, static windows cannot adapt to shifting consumer processing rates:
- If consumer capacity drops, the queue fills to $N \times W$, inflating transit latency.
- If consumer capacity surges, throughput is capped by the static window size divided by round-trip transit time ($W / \text{RTT}$).

Binary threshold schemes (**Config B**) attempt to remedy this by switching budgets between $B_{\text{min}} = 8$ and $B_{\text{max}} = 128$ based on queue occupancy thresholds (e.g., 75% high watermark, 25% low watermark). However, instantaneous queue depth is a lag indicator. By the time queue occupancy drops below 25%, the sender may surge, rapidly pushing occupancy past 75% and causing violent **limit-cycle oscillations** (Figure 3).

### 3.3 The Structural Inequity of FIFO Scheduling
When multiple flows share an IPC broker, strict FIFO dispatch orders packets purely by ingress arrival timestamp. Consider two competing flows:
- **Flow 0 (Control)**: 512 bytes payload ($552\text{ bytes}$ wire frame).
- **Flow 1 (Bulk Data)**: 4096 bytes payload ($4136\text{ bytes}$ wire frame).

Under concurrent backlog, FIFO dispatches packets in a 1:1 alternating pattern. For every $552\text{ bytes}$ dispatched for Flow 0, Flow 1 receives $4136\text{ bytes}$. Flow 1 captures:
$$\frac{4136}{552 + 4136} = 88.23\% \text{ of channel byte bandwidth}$$
Flow 0 is allocated only $11.77\%$. Using Jain's Fairness Index:
$$J = \frac{(552 + 4136)^2}{2 \times (552^2 + 4136^2)} = \frac{4688^2}{2 \times (304,704 + 17,106,496)} = \frac{21,977,344}{34,822,400} = 0.6311$$
Strict FIFO imposes a theoretical minimum fairness bound of $J = 0.6311$, causing severe throughput starvation for small-packet flows.

---

## 4. System Architecture

The architecture comprises three distinct process roles communicating over `AF_UNIX` sequenced packet sockets: **Sender Clients**, a central **Broker Daemon**, and a **Receiver Process**.

```mermaid
sequenceDiagram
    autonumber
    participant S as Sender Process
    participant B as Broker Daemon
    participant R as Receiver Process

    Note over B,R: Initialization & Connection
    R->>B: Connect & MSG_REGISTER (Receiver)
    B-->>R: Connection Accepted
    S->>B: Connect & MSG_REGISTER (Sender ID, Frame Size)
    B-->>S: Connection Accepted & Initial MSG_CREDIT Grant

    Note over S,B: Steady-State Data Transmission
    loop Credit-Regulated Transmission
        S->>B: MSG_DATA (Seq No, Timestamp, Payload, FNV-1a Hash)
        Note over B: Enqueue in Sender Multi-Queue<br/>Update EWMA Arrival Rate (Rin)
        Note over B: DRR Dispatcher selects packet<br/>Deficit decremented by wire size
        B->>R: MSG_DATA Forwarded
        R->>R: Checksum Verification & Latency Sample
        R->>B: MSG_ACK (Seq No)
        Note over B: Update EWMA Service Rate (Rcomp)<br/>Reclaim in-flight credit
        opt Admission Budget Permits
            B->>S: MSG_CREDIT (Replenish Credits)
        end
    end

    Note over S,B: Graceful Shutdown
    S->>B: MSG_STOP & shutdown(SHUT_WR)
    B-->>S: Drain remaining credits & close socket
    B->>R: Forward remaining queued packets & MSG_STOP
    R->>R: Output Performance Metrics CSV & Exit
```

### 4.1 Single-Threaded Non-Blocking Event Loop
The Broker Daemon utilizes a single-threaded event loop driven by Linux `poll()`. Spawning separate worker threads per connection introduces lock contention, cache thrashing, and non-deterministic scheduling races. By maintaining a single thread of execution:
- Global queue structures and scheduler states require no mutex locks or atomic CAS operations.
- Socket file descriptors are set to non-blocking mode via `fcntl(fd, F_SETFL, O_NONBLOCK)`.
- Upon receiving a `POLLIN` event, the broker greedily drains up to `PER_SENDER_QUEUE_CAP` messages until `-EAGAIN` or `-EWOULDBLOCK` is returned, preventing socket buffer congestion.

### 4.2 Broker State Machine
The broker coordinates registration, admission, queuing, scheduling, and dispatch:
1. **Registration**: Senders register via `MSG_REGISTER`, declaring their sender ID and expected payload frame size. The broker assigns an active slot, allocates queue structures, and arms the sender with initial credits.
2. **Admission**: Arriving `MSG_DATA` packets are validated for protocol compliance and admitted into per-sender circular ring buffers, provided hard queue limits are satisfied.
3. **Periodic Control**: Every 50 ms, a timer triggers `controller_tick()`, which updates EWMA rate estimates, transitions hysteresis states, and recalculates admission budgets.
4. **Dispatch Engine**: The broker invokes either `scheduler_dispatch_fifo()` or `scheduler_dispatch_drr()`, popping the selected message and forwarding it to the receiver socket.

---

## 5. Protocol Design

### 5.1 Wire Frame Structure
All messages exchanged over the IPC fabric share a standardized 40-byte packed header (`IpcHeader`) defined in `include/ipc_protocol.h`, followed by an optional variable-length payload.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          magic (0x41495043)           |    version    | msg_type |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           sender_id           |           reserved            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           seq_num                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         payload_len                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                         timestamp_ns                          +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           checksum                            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           credits                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                    Variable Payload (0..4096B)                +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Field | Type | Size | Description |
| :--- | :--- | :---: | :--- |
| `magic` | `uint32_t` | 4 B | Protocol identifier (`0x41495043`, ASCII "AIPC"). |
| `version` | `uint8_t` | 1 B | Protocol version (currently `1`). |
| `msg_type` | `uint8_t` | 1 B | Message opcode (`REGISTER`, `DATA`, `CREDIT`, `ACK`, `STOP`). |
| `sender_id` | `uint16_t` | 2 B | Logical identifier of transmitting sender (0..7). |
| `seq_num` | `uint32_t` | 4 B | Monotonically increasing sequence number for ordering and ACKs. |
| `payload_len` | `uint32_t` | 4 B | Length of payload immediately following the header (0..4096 B). |
| `timestamp_ns` | `uint64_t` | 8 B | Monotonic raw nanosecond timestamp captured at sender transmission. |
| `checksum` | `uint32_t` | 4 B | 32-bit FNV-1a hash over variable payload bytes. |
| `credits` | `uint32_t` | 4 B | Explicit credit grant count in `MSG_CREDIT` frames. |
| **Total Header** | | **40 B** | **Strictly packed (`__attribute__((packed))`).** |

### 5.2 Message Types and Control Handshake
1. `MSG_REGISTER`: Transmitted by endpoints upon initial connection. Senders specify their identity and payload size; the receiver registers as the data sink.
2. `MSG_DATA`: Carries user payload with timestamp and checksum. Consumes exactly 1 transmission credit.
3. `MSG_CREDIT`: Sent asynchronously from broker to sender. Contains explicit credit increment in the `credits` field.
4. `MSG_ACK`: Sent from receiver to broker upon message consumption. Reclaims broker credit headroom.
5. `MSG_STOP`: Signals graceful completion. Initiates queue draining and client teardown.

### 5.3 Payload Integrity Verification
Message payloads are validated using a 32-bit Fowler–Noll–Vo (FNV-1a) non-cryptographic hash:
$$\text{hash}_0 = 2166136261, \quad \text{hash}_{i+1} = (\text{hash}_i \oplus \text{byte}_i) \times 16777619$$
The receiver independently recalculates this checksum on every arriving packet. Across all 72 benchmark executions and robustness tests, **0 checksum mismatches** were recorded.

---

## 6. Queue Architecture

The broker manages buffering across competing flows via a modular multi-queue manager (`src/queue.c`).

```mermaid
flowchart TD
    subgraph Storage ["Queue Storage Layer (Bounded Memory)"]
        direction TB
        Q0["Sender 0 Ring Buffer<br/>Cap: 64 msgs<br/>Head / Tail Pointers"]
        Q1["Sender 1 Ring Buffer<br/>Cap: 64 msgs<br/>Head / Tail Pointers"]
        QN["Sender N Ring Buffer<br/>Cap: 64 msgs<br/>Head / Tail Pointers"]
    end

    subgraph Enforcers ["Hard Resource Isolation Enforcers"]
        E_SENDER["Per-Sender Limit: count ≤ 64 msgs"]
        E_GLOBAL["Global Capacity: global_count ≤ 256 msgs"]
        E_MEM["Total Queued Memory: global_bytes ≤ 1,048,576 B (1 MiB)"]
    end

    IN["Incoming MSG_DATA"] --> E_SENDER
    E_SENDER --> E_GLOBAL
    E_GLOBAL --> E_MEM
    E_MEM -->|"Admitted (O(1))"| Storage
    E_MEM -- Limit Exceeded --> REJECT["Drop / Return QUEUE_ERR"]
```

### 6.1 Multi-Queue Structure
Each registered sender is allocated an independent, fixed-size circular ring buffer:
- **Node Capacity**: Pre-allocated array of `QueueNode` structures, eliminating heap churn during steady-state forwarding.
- **Complexity**: Enqueue and dequeue operations execute in strict $O(1)$ constant time with zero dynamic memory allocation.

### 6.2 Hard Resource Isolation Bounds
To prevent rogue senders from inducing out-of-memory kernel panics or starving concurrent flows, three independent limits are strictly enforced:
1. **Per-Sender Queue Limit**: $\le 64$ messages. Protects against a single runaway producer filling the broker.
2. **Global Queue Capacity**: $\le 256$ messages. Sets the global upper bound across all active flows.
3. **Global Queued Payload Memory**: $\le 1,048,576$ bytes (1 MiB). Restricts total memory allocated to unconsumed payload bytes.

If any limit is violated, the incoming message is immediately rejected with `QUEUE_ERR_SENDER_FULL` or `QUEUE_ERR_GLOBAL_FULL`, and the sender's credit allowance is paused.

---

## 7. Adaptive Backpressure & State Controller

### 7.1 EWMA Rate Estimation
Every 50 ms ($\Delta t = 0.05\text{ s}$), `controller_tick()` calculates the instantaneous arrival rate $M_{\text{in}} = \Delta A / \Delta t$ and completion rate $M_{\text{comp}} = \Delta C / \Delta t$. These noisy measurements are smoothed using an Exponentially Weighted Moving Average (EWMA):
$$R_{\text{in}}^{(t)} = (1 - \alpha) R_{\text{in}}^{(t-1)} + \alpha M_{\text{in}}^{(t)}$$
$$R_{\text{comp}}^{(t)} = (1 - \alpha) R_{\text{comp}}^{(t-1)} + \alpha M_{\text{comp}}^{(t)}$$
We configure $\alpha = 0.20$. This assigns 80% weight to historical performance and 20% to recent traffic, filtering out transient spikes while adapting to persistent trends within 100–150 ms (2–3 ticks).

The traffic intensity ratio $\rho$ represents channel balance:
$$\rho = \frac{R_{\text{in}}}{R_{\text{comp}} + 1.0}$$
where the $+1.0$ term prevents division by zero if the receiver is stalled.

### 7.2 Three-State Hysteresis State Machine

```mermaid
stateDiagram-v2
    [*] --> STATE_NORMAL: System Init (Budget = 64)

    STATE_NORMAL --> STATE_CAUTION: Queue Util ≥ 50% OR (Util ≥ 30% AND ρ > 1.10)
    STATE_NORMAL --> STATE_OVERLOAD: Queue Util ≥ 75% OR (Util ≥ 50% AND ρ > 1.30)

    STATE_CAUTION --> STATE_OVERLOAD: Immediate Escalation (Queue Util ≥ 75% OR Util ≥ 50% AND ρ > 1.30)
    STATE_CAUTION --> STATE_NORMAL: Delayed De-escalation (Hold ≥ 2 ticks / 100ms)

    STATE_OVERLOAD --> STATE_CAUTION: Delayed De-escalation (Hold ≥ 2 ticks / 100ms)
    STATE_OVERLOAD --> STATE_NORMAL: Delayed De-escalation (Queue clear, Hold ≥ 2 ticks)

    note right of STATE_OVERLOAD
        Multiplicative Decrease:
        Budget ← max(min_budget, Budget * 0.65)
        Step ← 2
    end note

    note right of STATE_CAUTION
        Stabilization:
        Target ← Rcomp * 0.10s
        Budget ← (Budget + Target) / 2
        Step ← 4
    end note

    note left of STATE_NORMAL
        Gradual Additive Recovery:
        Budget ← min(max_budget, Budget + step)
        Step ← min(16, step + 2)
    end note
```

1. **`STATE_NORMAL`**: Characterized by low queue occupancy ($<30\%$) and balanced traffic ($\rho \le 1.10$). The controller executes **Gradual Additive Recovery**, increasing admission budgets by $\text{step}$ ($+2, +4, +6 \dots$ up to $+16$) each tick until reaching $B_{\text{max}} = 128$.
2. **`STATE_CAUTION`**: Triggered when queue utilization reaches $\ge 50\%$, or moderate occupancy ($\ge 30\%$) with arrival rate exceeding completion rate ($\rho > 1.10$). The budget is stabilized toward receiver capacity:
   $$B \leftarrow \frac{B + (R_{\text{comp}} \times 0.10)}{2}$$
3. **`STATE_OVERLOAD`**: Triggered under severe congestion: queue utilization $\ge 75\%$, or $\ge 50\%$ with severe rate imbalance ($\rho > 1.30$). The controller triggers **Multiplicative Decrease**:
   $$B \leftarrow \max(B_{\text{min}}, \lfloor B \times 0.65 \rfloor)$$
- **Hysteresis Stability**: Transitions to higher congestion states occur **immediately** (1 tick). However, de-escalating to a lower congestion state requires **2 consecutive non-congested intervals** ($\ge 100\text{ ms}$). This prevents rapid oscillation between states.

### 7.3 Non-Revocable Credit Withholding Invariant
When credits are granted, broker buffer capacity is reserved upfront. If congestion lowers the admission budget $B_{\text{curr}}$ below outstanding in-flight credits $W_{\text{out}}$:
1. Outstanding credits are never revoked.
2. The broker **withholds new credit grants**:
   $$\text{If } W_{\text{out}} \ge B_{\text{curr}} \implies \text{New Grants} = 0$$
3. As the receiver returns `MSG_ACK`s, $W_{\text{out}}$ decrements. Once $W_{\text{out}} < B_{\text{curr}}$, credit allocation resumes.

### 7.4 Byte-Equalized Credit Budgeting
Granting equal packet credits causes severe byte discrimination against small-packet senders. In Config D, the broker dimensions credit allowances inversely to packet wire size:
$$\text{Credit Limit}_i = \left\lfloor \frac{\text{Byte Quota Target}}{\text{Wire Frame Size}_i} \right\rfloor = \left\lfloor \frac{35,328\text{ bytes}}{\text{Wire Frame Size}_i} \right\rfloor$$
- **512B Sender (552B wire frame)**: Granted 64 credits ($35.3\text{ KiB}$).
- **4096B Sender (4136B wire frame)**: Granted 8 credits ($33.1\text{ KiB}$).
This equalizes the available byte pipeline, allowing DRR to sustain continuous byte balancing.

---

## 8. Packet Scheduling & DRR Deficit Semantics

```mermaid
flowchart TD
    subgraph FIFO ["Strict FIFO Dispatch (Packet-Blind)"]
        direction TB
        F_IN["Inspect Queue Heads"]
        F_CMP["Compare Ingress Timestamps"]
        F_DISP["Dispatch Oldest Timestamp Packet"]
        F_RESULT["1:1 Packet Alternation<br/>Small (552B) vs Large (4136B)<br/>88% Byte Allocation to Bulk Flow<br/>J = 0.6294 (Severe Inequity)"]
        F_IN --> F_CMP --> F_DISP --> F_RESULT
    end

    subgraph DRR ["Deficit Round-Robin Dispatch (Byte-Aware)"]
        direction TB
        D_IN["Visit Queue i"]
        D_CHK{"Deficit[i] ≥ Packet Bytes?"}
        D_ADD["Deficit[i] += Quantum (4136B)"]
        D_DEQ["Dequeue & Dispatch Packet<br/>Deficit[i] -= Packet Bytes"]
        D_MORE{"More Deficit for Head Packet?"}
        D_ADV["Advance to (i + 1) % N"]
        D_RESULT["7.49:1 Packet Ratio<br/>Small flow gets ~7.5 pkts per large pkt<br/>50/50 Byte Equity<br/>J = 1.0000 (Near-Perfect Parity)"]

        D_IN --> D_CHK
        D_CHK -- No --> D_ADD --> D_CHK
        D_CHK -- Yes --> D_DEQ --> D_MORE
        D_MORE -- Yes --> D_DEQ
        D_MORE -- No --> D_ADV --> D_RESULT
    end
```

### 8.1 DRR Algorithm Mechanics
Deficit Round-Robin (Shreedhar & Varghese, SIGCOMM 1995) provides byte-fair queueing with $O(1)$ complexity. Each flow maintains a deficit counter:
1. **Quantum Allocation**: The quantum $Q$ is set to $4136\text{ bytes} \ge \text{MaxFrameSize}$, ensuring any backlogged flow can dispatch at least one message per round.
2. **Deficit Accumulation**: When visiting backlogged flow $i$, if its deficit is smaller than the head packet size, the quantum is added:
   $$\text{deficit}[i] \leftarrow \text{deficit}[i] + Q$$
3. **Packet Transmission**: While $\text{deficit}[i] \ge \text{frame\_size}_{\text{head}}$, the packet is dispatched and its size deducted:
   $$\text{deficit}[i] \leftarrow \text{deficit}[i] - \text{frame\_size}_{\text{head}}$$
4. **Pointer Advancement**: When $\text{deficit}[i] < \text{frame\_size}_{\text{head}}$, the scheduler moves to the next active sender.

### 8.2 Deficit Reset Semantics: Empty Queue vs Backlog Persistence
A critical requirement of Shreedhar & Varghese DRR is the handling of idle queues:
- **Reset on Empty Queue**: Whenever a sender's queue becomes empty, its deficit counter **must be immediately reset to zero**:
  ```c
  if (!queue_sender_has_messages(qm, i)) {
      sched->deficit_counter[i] = 0;
      sched->current_rr_index = (i + 1) % MAX_SENDERS;
  }
  ```
  If deficit were preserved when empty, an idle flow could accumulate thousands of bytes of deficit over idle rounds, releasing an unfair burst upon reactivation.
- **Persistence Across Rounds During Backlog**: Conversely, while a flow remains continuously backlogged (as in Scenario 5), any unused deficit remaining after dispatching packets **persists** across round-robin cycles. This persistence allows Flow 0 (552B) to accumulate deficit until it dispatches $\approx 7.49$ packets per 1 packet of Flow 1 (4136B), achieving byte parity ($J = 1.0000$).

---

## 9. Empirical Benchmark Methodology

### 9.1 Experimental Configurations
Four configurations were evaluated across 6 scenarios:
- **Config A (Fixed Window, FIFO)**: Static per-sender credit window ($W = 16$), strict FIFO dispatch.
- **Config B (Fixed Threshold, FIFO)**: Static threshold backpressure (drop budget to 8 at 75% utilization, restore to 128 at 25%), strict FIFO dispatch.
- **Config C (Adaptive EWMA, FIFO)**: Full EWMA rate sensing ($\alpha = 0.20$), 3-state hysteresis, credit withholding, strict FIFO dispatch.
- **Config D (Adaptive EWMA, DRR)**: Full EWMA rate sensing, 3-state hysteresis, byte-equalized credit budgeting, Deficit Round-Robin dispatch.

### 9.2 Benchmark Workload Scenarios
1. **Scenario 1 (Normal Traffic)**: 3 senders transmitting 200 messages each (1024B payload, 600 msgs total). Steady load evaluating baseline throughput and latency.
2. **Scenario 2 (Sudden Bursts)**: 3 senders transmitting 200 messages in 10 bursts of 20 messages with 50 ms idle intervals. Evaluates burst absorption.
3. **Scenario 3 (Sustained Overload)**: 3 senders continuously transmitting 2048-byte payloads without pauses. Evaluates hard memory isolation limits.
4. **Scenario 4 (Fairness Exp A - Equal Offered Volume)**: 2 senders with asymmetric sizes but identical total offered byte volume ($204.8\text{ KiB}$ payload each: 400 msgs $\times$ 512B vs 50 msgs $\times$ 4096B). Evaluates complete transfer fairness.
5. **Scenario 5 (Fairness Exp B - Continuous Backlog)**: 2 senders continuously backlogged (500 msgs $\times$ 512B vs 250 msgs $\times$ 4096B) evaluated across 200 dispatches. Synchronized with a deterministic two-way POSIX pipe rendezvous barrier. Evaluates instantaneous throughput fairness.
6. **Scenario 6 (Receiver Slowdown & Recovery)**: 3 senders transmitting 200 messages each. Receiver injects an intentional 250 ms stall at message 200. Evaluates credit depletion backpressure.

### 9.3 Performance Metrics & Formulas
- **Throughput**: Delivered messages per second ($M_{\text{rate}} = N_{\text{msgs}} / \Delta t$) and wire-frame megabytes per second ($B_{\text{rate}} = V_{\text{bytes}} / (1024^2 \times \Delta t)$).
- **Latency Percentiles**: Transit latency $L_i = T_{\text{recv}, i} - T_{\text{send}, i}$ captured via `CLOCK_MONOTONIC_RAW`, sorted via `qsort()`, and evaluated for median ($p50$), 95th percentile ($p95$), and tail ($p99$).
- **Jain's Fairness Index**: Computed over all $n$ declared participating flows:
  $$J = \frac{\left( \sum_{i=1}^n B_i \right)^2}{n \sum_{i=1}^n B_i^2}$$
  where $B_i = \text{msgs}_i \times \text{frame\_size}_i$. Any starved flow contributes $0\text{ bytes}$, ensuring $n = 2$ flows with 1 starved yields $J = 0.5000$.
- **Controller Overhead**: Monotonic raw execution time of `controller_tick()` in microseconds.

---

## 10. Experimental Results & Performance Analysis

All data reported below were empirically measured across **3 independent evaluation runs ($N=3$)** on Linux 6.11 x86_64, statistically aggregated using `tests/compute_summary.c`, and recorded in `results/benchmark_summary.csv`.

### 10.1 Comprehensive Benchmark Summary Table (Mean of 3 Independent Runs)

| Scenario | Configuration | Throughput (msgs/s) | Throughput (MB/s) | Median $p50$ ($\mu$s) | Tail $p99$ ($\mu$s) | Peak Queue (msgs) | Peak Memory (KiB) | Jain Byte Fairness | Controller Overhead ($\mu$s) | Delivered (s0 / s1 / s2 msgs) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **1. Normal Traffic** | **Config A (Fixed Window, FIFO)** | 3564.82 | 3.62 | 5,207.96 | 5,615.40 | 21 | 21.33 | 1.0000 | 0.248 | 200 / 200 / 200 |
| | **Config B (Fixed Threshold, FIFO)**| 3555.58 | 3.61 | 12,039.60 | 15,057.12 | 48 | 48.00 | 1.0000 | 0.326 | 200 / 200 / 200 |
| | **Config C (Adaptive EWMA, FIFO)** | 3567.91 | 3.62 | 10,856.90 | 12,529.93 | 23 | 22.67 | 1.0000 | 0.463 | 200 / 200 / 200 |
| | **Config D (Adaptive EWMA, DRR)**  | 3559.80 | 3.61 | 10,644.95 | 11,200.72 | 39 | 39.33 | 1.0000 | 0.464 | 200 / 200 / 200 |
| **2. Sudden Bursts** | **Config A (Fixed Window, FIFO)** | 1055.40 | 1.07 | 1,563.90 | 5,414.93 | 27 | 27.33 | 1.0000 | 0.269 | 200 / 200 / 200 |
| | **Config B (Fixed Threshold, FIFO)**| 1069.45 | 1.09 | 2,182.90 | 5,491.00 | 24 | 24.00 | 1.0000 | 0.322 | 200 / 200 / 200 |
| | **Config C (Adaptive EWMA, FIFO)** | 1068.64 | 1.08 | 2,007.97 | 5,304.64 | 24 | 24.00 | 1.0000 | 0.506 | 200 / 200 / 200 |
| | **Config D (Adaptive EWMA, DRR)**  | 1070.03 | 1.09 | 3,565.85 | 7,128.05 | 33 | 33.33 | 1.0000 | 0.529 | 200 / 200 / 200 |
| **3. Sustained Overload** | **Config A (Fixed Window, FIFO)** | 3526.37 | 7.02 | 5,266.79 | 6,093.26 | 27 | 53.33 | 1.0000 | 0.210 | 200 / 200 / 200 |
| | **Config B (Fixed Threshold, FIFO)**| 3541.38 | 7.05 | 10,571.32 | 14,204.28 | 60 | 120.00 | 1.0000 | 0.387 | 200 / 200 / 200 |
| | **Config C (Adaptive EWMA, FIFO)** | 3553.59 | 7.07 | 8,558.49 | 11,608.68 | 54 | 108.00 | 1.0000 | 0.530 | 200 / 200 / 200 |
| | **Config D (Adaptive DRR)**        | 3543.96 | 7.06 | 5,215.38 | 6,284.82 | 32 | 64.00 | 1.0000 | 0.520 | 200 / 200 / 200 |
| **4. Fairness Exp A** | **Config A (Fixed Window, FIFO)** | 2970.49 | 2.69 | 1,656.26 | 3,586.19 | 16 | 64.00 | 0.9989 | 0.254 | 400 / 50 |
| *(Equal Offered Bytes)* | **Config B (Fixed Threshold, FIFO)**| 2955.97 | 2.68 | 11,473.45 | 15,708.49 | 64 | 136.00 | 0.9989 | 0.367 | 400 / 50 |
| | **Config C (Adaptive EWMA, FIFO)** | 2960.76 | 2.68 | 9,845.55 | 12,801.27 | 64 | 136.00 | 0.9989 | 0.543 | 400 / 50 |
| | **Config D (Adaptive EWMA, DRR)**  | 2974.45 | 2.70 | 7,012.14 | 7,925.03 | 59 | 32.17 | 0.9989 | 0.427 | 400 / 50 |
| **5. Fairness Exp B** | **Config A (Fixed Window, FIFO)** | 1592.43 | 3.58 | 3,540.73 | 4,919.93 | 32 | 72.00 | 0.6294 | 0.254 | 99.33 / 100.67 |
| *(Continuous Backlog)* | **Config B (Fixed Threshold, FIFO)**| 1601.44 | 2.91 | 8,179.09 | 15,272.09 | 97 | 198.67 | 0.7106 | 0.435 | 124.67 / 75.33 |
| | **Config C (Adaptive EWMA, FIFO)** | 1593.29 | 3.00 | 7,795.41 | 13,204.25 | 87 | 173.00 | 0.6950 | 0.576 | 120.67 / 79.33 |
| | **Config D (Adaptive EWMA, DRR)**  | 1603.02 | 1.48 | 7,910.30 | 8,675.57 | 62 | 44.00 | **1.0000** | 0.508 | **176.67 / 23.33** |
| **6. Receiver Slowdown** | **Config A (Fixed Window, FIFO)** | 1427.38 | 1.45 | 5,292.24 | 255,775.57 | 21 | 21.33 | 1.0000 | 0.263 | 200 / 200 / 200 |
| *(Induced 250ms Stall)* | **Config B (Fixed Threshold, FIFO)**| 1432.66 | 1.45 | 12,700.67 | 264,189.35 | 50 | 50.33 | 1.0000 | 0.457 | 200 / 200 / 200 |
| | **Config C (Adaptive EWMA, FIFO)** | 1432.62 | 1.45 | 11,529.61 | 261,816.46 | 43 | 42.67 | 1.0000 | 0.585 | 200 / 200 / 200 |
| | **Config D (Adaptive DRR)**        | 1428.64 | 1.45 | 10,760.57 | 261,979.05 | 39 | 39.00 | 1.0000 | 0.502 | 200 / 200 / 200 |

---

### 10.2 Analysis of Steady-State Throughput & Latency

![Figure 1: Throughput Comparison across 6 Scenarios](figures/throughput_comparison.png)

Under Scenario 1 (Normal Traffic), all four configurations deliver virtually identical throughput:
- **Config A**: $3564.82 \pm 8.8\text{ msgs/s}$ ($3.62\text{ MiB/s}$)
- **Config B**: $3555.58 \pm 14.5\text{ msgs/s}$ ($3.61\text{ MiB/s}$)
- **Config C**: $3567.91 \pm 6.6\text{ msgs/s}$ ($3.62\text{ MiB/s}$)
- **Config D**: $3559.80 \pm 22.9\text{ msgs/s}$ ($3.61\text{ MiB/s}$)

The throughput spread across all configurations is only $12.3\text{ msgs/s}$ ($0.34\%$), demonstrating that adaptive control does not produce higher steady-state throughput when the channel is uncongested.

![Figure 2: Latency Percentiles across Configurations](figures/latency_percentiles.png)

However, significant divergence occurs in queuing delay:
- **Config A** achieves the lowest median and tail latency ($p50 = 5.21\text{ ms}$; $p99 = 5.62\text{ ms}$) because its static per-sender window ($W = 16$) restricts queue depth to a peak of 21 messages.
- **Config B, C, D** exhibit higher tail latencies ($p99 = 11.20 - 15.06\text{ ms}$) because broader admission credits permit larger queue buffers (peak 23–48 messages), increasing transit queuing time.

---

### 10.3 Queue Occupancy and Memory Bounds

![Figure 3: Peak Queue Depth across Configurations](figures/queue_depth_comparison.png)

Across all evaluated workloads, queue depths remained strictly bounded:
- In Scenario 2 (Bursts), queues absorbed surges with peaks of $\le 33$ messages ($\le 13\%$ of global capacity).
- In Scenario 3 (Sustained Overload), peak queued memory reached $120.0\text{ KiB}$, well below the 1 MiB hard ceiling. Zero packet drops were observed.

---

### 10.4 Fairness Experiment A vs. Experiment B: Resolving the FIFO Defect

![Figure 4: Jain's Byte Fairness Index across Scenarios](figures/jains_fairness_comparison.png)

Fairness evaluation reveals the fundamental operational distinction between complete equal-volume transfers and active continuous backlog:
- **Experiment A (Equal Offered Byte Volume)**: Sender 0 offers 400 pkts $\times$ 512B ($220.8\text{ KiB}$ wire) and Sender 1 offers 50 pkts $\times$ 4096B ($206.8\text{ KiB}$ wire). When both flows run to completion, all configurations achieve $J = 0.9989$. Because both senders exhaust their offered work, complete transfers naturally exhibit high aggregate fairness regardless of scheduling policy.
- **Experiment B (Continuous Backlog)**: Both senders remain continuously backlogged throughout a 200-message evaluation window. Under FIFO (Config A, B, C), the scheduler dispatches packets 1:1, allocating 88% of channel bandwidth to the bulk flow and degrading fairness to $J = 0.6294$ (Config A) and $J = 0.6950$ (Config C).

![Figure 5: Scenario 5 Byte Distribution](figures/scenario5_byte_distribution.png)
![Figure 6: Scenario 5 Packet Ratio (Small vs Large)](figures/scenario5_packet_ratio.png)

Under Config D (DRR with byte-equalized credits), the scheduler dynamically dispatches:
- **Sender 0 (Small)**: $176.67\text{ messages}$ ($97,520.0\text{ bytes}$ mean).
- **Sender 1 (Large)**: $23.33\text{ messages}$ ($96,506.7\text{ bytes}$ mean).

The packet ratio is $176.67 / 23.33 = 7.57:1$, precisely matching the theoretical $4136 / 552 = 7.49:1$ inverse wire frame ratio. Delivered byte volumes are virtually identical ($97.5\text{ KiB}$ vs $96.5\text{ KiB}$), yielding near-perfect byte fairness:
$$J = \frac{(97,520.0 + 96,506.67)^2}{2 \times (97,520.0^2 + 96,506.67^2)} = \mathbf{1.0000} \quad (\text{exact: } 0.99997)$$

> **Scope of Finding**: DRR with byte-equalized credits demonstrates balanced byte delivery for this specific continuous-backlog workload. This finding confirms algorithm correctness under controlled test conditions, but does not constitute a proof of universal fairness across all arbitrary traffic patterns.

---

### 10.5 Receiver Stall & Backpressure Audit

![Figure 7: Receiver Stall Latency Surge](figures/receiver_stall_latency.png)

In Scenario 6, an intentional 250 ms stall at message 200 causes tail latency to surge to $\approx 262\text{ ms}$ across all configurations:
- **Backpressure via Credit Depletion**: Senders are constrained by credit allocations ($\le 16$ credits each). When the receiver stalls, ACKs cease, causing senders to exhaust their credits and block naturally on `ipc_send_msg()`.
- **Peak Queue Bounded**: Peak queue depth was capped at 21–50 messages across all runs, with 0 packet drops.
- **Audit of Controller State Transitions**: The benchmark recorded **`state_changes = 0`**. In `src/controller.c`, entering `STATE_CAUTION` or `STATE_OVERLOAD` requires queue utilization of $\ge 30\%$ (77 messages) or $\ge 50\%$ (128 messages). Because sender credits bounded in-flight messages to $\le 48$, queue occupancy never exceeded 50 messages ($< 20\%$). Therefore, backpressure was maintained entirely by credit exhaustion, and the benchmark did not trigger dynamic transitions into `STATE_OVERLOAD`.

### 10.6 Sub-Microsecond Controller Overhead
Raw nanosecond timers record controller execution time at each 50 ms tick:
- Config A: $0.210 - 0.269\ \mu\text{s/tick}$
- Config B: $0.322 - 0.457\ \mu\text{s/tick}$
- Config C: $0.463 - 0.585\ \mu\text{s/tick}$
- Config D: $0.427 - 0.529\ \mu\text{s/tick}$
Executing at 20 Hz, the controller consumes less than $0.001\%$ of a single CPU core, confirming negligible overhead in user space.

---

## 11. Robustness & Fault Tolerance

The system includes a dedicated robustness test suite (`tests/test_robustness.c`) and comprehensive error-handling logic:
1. **Rogue Sender Containment**: `bin/sender -X` bypasses credit limits and floods the broker. In `test_robustness_rogue_sender()`, the broker's hard limit caps the rogue queue at 64 messages, rejecting subsequent packets with `QUEUE_ERR_SENDER_FULL`. The broker event loop continues forwarding traffic from legitimate senders without disruption.
2. **Abrupt Client Disconnection**: If a client crashes or terminates abruptly, `recv()` returns 0 (`EOF`) or `-ECONNRESET`. In `test_robustness_abrupt_disconnect()`, the broker cleans up client state, drains remaining messages, and closes the socket descriptor without memory leakage.
3. **Half-Close Protocol**: Earlier testing revealed that senders executing `close(fd)` immediately after `MSG_STOP` caused Linux to issue a `RST` if unread `MSG_CREDIT` frames remained in socket buffers, discarding tail packets. Senders now drain incoming credits and execute `shutdown(SHUT_WR)` before closing, ensuring complete, loss-free message delivery.
4. **Deterministic Pipe Rendezvous**: Scenario 5 utilizes a two-way POSIX pipe barrier (`ready_pipe` and `sync_pipe`). Both senders register, arm credits, signal readiness to the test harness, and release simultaneously upon pipe close, eliminating race conditions.
5. **Sanitizer & Static Analysis Verification**:
   - `gcc -fanalyzer`: Zero static analysis warnings or memory leak paths.
   - `-fsanitize=address,undefined`: Full test suite (140 assertions) and robustness multi-process tests pass with zero memory leaks, buffer overflows, or undefined behaviors.

---

## 12. Limitations & Boundary Analysis

1. **Sample Size Scope**: Empirical results are averaged across **3 independent evaluation runs ($N=3$)**. While message throughput demonstrates high stability ($CV < 1\%$), instantaneous extreme-value metrics (such as peak queue depth, $CV = 20\% - 86\%$) are subject to OS scheduling variability.
2. **State Activation Headroom**: With sender credit allocations capped at 16 credits each, total in-flight packets across 3 senders cannot exceed 48 messages, keeping queue utilization below the 77-message activation threshold. The benchmark suite demonstrates credit-exhaustion backpressure rather than dynamic state machine transitions.
3. **Fairness Generalization Scope**: DRR achieved near-perfect byte fairness ($J = 1.0000$) specifically for the 512B vs 4096B continuous-backlog workload in Scenario 5. This empirical validation confirms algorithm mechanics under controlled conditions, but does not imply universal fairness across arbitrary network topologies.
4. **User-Space Copy Overhead**: The architecture requires two IPC hops (Sender $\to$ Broker $\to$ Receiver), incurring four context switches and two kernel data copies per message. While optimal for discrete message regulation, zero-copy shared memory remains faster for raw gigabyte bulk data transfers.

---

## 13. Future Work

1. **Zero-Copy Shared Memory Ring Buffers**: Coupling the adaptive credit controller with POSIX shared memory (`mmap`) or Linux `io_uring` to achieve zero-copy payload delivery while retaining credit-based backpressure.
2. **Dynamic Credit Scaling**: Dynamically scaling sender credit allowances under heavy multi-client workloads to deliberately exercise the controller's `STATE_OVERLOAD` transition path under live multi-process benchmarks.
3. **Hierarchical DRR (H-DRR)**: Extending DRR scheduling to support hierarchical bandwidth allocation across process cgroups and user sessions.
4. **eBPF In-Kernel Filtering**: Offloading rogue-sender rate limiting to eBPF socket filters (`sock_ops`), dropping protocol-violating packets directly in the kernel before reaching user-space `poll()`.

---

## 14. Conclusion

This project has designed, implemented, audited, and empirically validated an adaptive IPC backpressure and fairness control architecture in pure C for Linux. By synthesizing Sequenced Packet UNIX Domain Sockets, EWMA rate sensing, 3-state hysteresis control, byte-equalized credit budgeting, and Deficit Round-Robin scheduling, the system eliminates bufferbloat and resolves the severe byte unfairness inherent in standard FIFO dispatch.

Evaluated across 24 benchmark permutations over 3 independent runs and verified by 140 unit and robustness assertions under AddressSanitizer and UndefinedBehaviorSanitizer, the system achieves:
- Predictable steady-state forwarding ($\approx 3560\text{ msgs/s}$) with sub-microsecond controller overhead ($<0.58\ \mu\text{s/tick}$).
- Definitive resolution of FIFO byte starvation, delivering $J = \mathbf{1.0000}$ (exact: $0.99997$) under continuous asymmetric backlog.
- Deterministic credit-exhaustion backpressure bounding peak queue occupancy during receiver outages with zero dropped packets.

---

## 15. References

1. **M. Shreedhar and G. Varghese**, "Efficient fair queueing using deficit round-robin," *IEEE/ACM Transactions on Networking*, vol. 4, no. 3, pp. 375–385, June 1996.
2. **R. Jain, D.-M. Chiu, and W. R. Hawe**, "A Quantitative Measure of Fairness and Permutation for Computer Systems," *DEC Research Report TR-301*, Sept. 1984.
3. **V. Jacobson**, "Congestion avoidance and control," *ACM SIGCOMM Computer Communication Review*, vol. 18, no. 4, pp. 314–329, Aug. 1988.
4. **S. Floyd and V. Jacobson**, "Random early detection gateways for congestion avoidance," *IEEE/ACM Transactions on Networking*, vol. 1, no. 4, pp. 397–413, Aug. 1993.
5. **K. Nichols and V. Jacobson**, "Controlling Queue Delay," *Communications of the ACM*, vol. 55, no. 7, pp. 42–50, July 2012.
6. **W. R. Stevens and S. A. Rago**, *Advanced Programming in the UNIX Environment*, 3rd ed. Boston, MA: Addison-Wesley, 2013.
7. **M. Kerrisk**, *The Linux Programming Interface: A Linux and UNIX System Programming Handbook*. San Francisco, CA: No Starch Press, 2010.
8. **J. Postel**, "Transmission Control Protocol," *RFC 793*, Sept. 1981.
9. **D. Fowler, G. Noll, and P. Vo**, "Fowler–Noll–Vo Hash Function," *IETF Draft*, 2004.
10. **A. Demers, S. Keshav, and S. Shenker**, "Analysis and simulation of a fair queueing algorithm," *ACM SIGCOMM Computer Communication Review*, vol. 19, no. 4, pp. 1–12, Sept. 1989.
