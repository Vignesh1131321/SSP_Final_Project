# Real-World Workload Methodology

This note explains how the two application-level workloads are implemented in the benchmark suite, so the experimental design is explicit and reproducible.

## 1. Concurrent Hash Table

The hash-table benchmark compares three synchronization strategies:

- Coarse mutex: one global `std::mutex` protects the whole map.
- Striped RW lock: 64 shards, each shard guarded by `std::shared_mutex`.
- Lock-free open addressing: atomic slot ownership with CAS insertion and tombstone deletes.

### Scenario model

All hash-table scenarios are generated from the same operation engine. Each worker thread repeatedly samples an operation and a key until the time budget expires. Scenario identity is encoded in the `notes` field as `scenario=<name>`.

- `balanced_uniform`: mixed read/write/delete access with a broad keyspace.
- `read_dominant`: lookup-heavy traffic to mimic cache or metadata reads.
- `write_dominant`: update-heavy traffic to stress insertion and replacement.
- `churn_delete_heavy`: deletion-heavy traffic to stress erasure and tombstone behavior.
- `bursty_rw`: alternating read-heavy and write-heavy phases to mimic phase changes.
- `hotspot_contention`: repeated access to a small hot key subset to force contention.

### Implementation details that matter

- The striped implementation hashes keys to 64 shards.
- The lock-free table uses open addressing, `EMPTY` and `DELETED` tombstones, and CAS on slot ownership.
- The benchmark records throughput and latency per operation, then aggregates across the repetitions.

### Limitations

- There is no resize path.
- There is no memory reclamation cost for lock-free deletion.
- Hotspot locality is controlled by the key sampler, not by external traces.

## 2. Bounded Producer-Consumer Queue

The queue benchmark compares three queue strategies:

- Mutex queue: bounded deque protected by a mutex.
- Lock-free ring: bounded MPMC ring buffer with per-slot sequence numbers.
- Semaphore queue: bounded deque coordinated by counting semaphores.

### Scenario model

The workload is driven by producer and consumer threads running concurrently until the time budget expires. Scenario identity is also written into the CSV `notes` field.

- `balanced_1to1`: even producer/consumer balance.
- `read_dominant`: consumer-heavy configuration that drains the queue aggressively.
- `write_dominant`: producer-heavy configuration that fills the queue more often.
- `bursty_backpressure`: periodic burst phases that create queue pressure.
- `hotspot_small_buffer`: a smaller buffer to intensify contention and full/empty transitions.
- `fanin_fanout`: asymmetric producer/consumer mix to stress coordination imbalance.

### Implementation details that matter

- The lock-free queue requires a power-of-two capacity.
- Queue success and per-enqueue latency are measured from the producer side.
- The semaphore queue uses non-blocking try-wait paths in this benchmark so the run stays time-bounded.

### Limitations

- This benchmark captures synchronization and backpressure behavior, not end-to-end queueing latency under a traced workload.
- Burstiness is synthetic and phase-based rather than trace-replayed.

## 3. How to interpret the graphs

For publication, the scenario-specific plots should be the main figures because they show the absolute throughput curve for each implementation under each workload condition.

The performance-profile figures can be used as compact summary figures, but they should not replace the scenario plots because they hide which scenario caused a slowdown.
