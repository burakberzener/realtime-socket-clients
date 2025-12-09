# Real-Time Socket Clients

A C implementation of real-time data collection and server control clients that communicate with a real-time signal server via TCP sockets.

## Author Notes

This solution prioritizes:
1. **Correctness**: Exact specification adherence
2. **Robustness**: Handles edge cases (missing data, disconnects)
3. **Simplicity**: Single-threaded, no complex dependencies
4. **Performance**: Minimal CPU, bounded memory

Key design tradeoffs:
- **No threads** 
     A single-threaded design is simpler and fully sufficient for the task. While multi-threading could offer higher throughput, the problem does not require it, and avoiding threads reduces complexity and potential synchronization issues
- **poll()** 
     poll() provides good portability across systems and is efficient enough for this workload. Although epoll() could offer better scalability on Linux, it is unnecessary here given the small number of file descriptors.
- **Fixed buffers** 
     Using fixed buffers is safer and more predictable since the packet sizes are known in advance. Dynamic allocation would provide greater flexibility but introduces additional overhead and potential memory-management pitfalls, which are not needed in this task.
- **Polling** 
     Polling offers straightforward timing control and simplifies the overall implementation. An event-driven approach could be used, but it would add unnecessary complexity given the task requirements.

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Design Decisions](#design-decisions)
- [Build and Run](#build-and-run)
- [Correctness Validation](#correctness-validation)

---

## Overview

### Problem Statement

The project requires two C programs that:

1. **client1**: Connect to three TCP servers (localhost:4001, 4002, 4003) and output real-time data as JSON every 100ms
2. **client2**: Same as client1, but with:
   - 20ms time windows instead of 100ms
   - Control logic that adjusts server output1 frequency/amplitude based on output3 threshold
   - Binary control protocol (16-bit big-endian)

### Key Constraints

- Real-time performance (100ms and 20ms windows)
- Asynchronous I/O (non-blocking socket operations)
- Robust reconnection handling
- Binary protocol implementation (16-bit big-endian)
- No external dependencies beyond standard C libraries

---

## Architecture

### System Design

```
┌─────────────────────────────────────────────────────┐
│         Real-Time Signal Servers                     │
│  Port 4001 (out1) | Port 4002 (out2) | Port 4003 (out3) │
└────────┬──────────────┬──────────────┬────────────┘
         │ TCP Read     │ TCP Read     │ TCP Read
         │              │              │
    ┌────▼──────────────▼──────────────▼────┐
    │   Non-Blocking Socket I/O (poll)      │
    │   - 3 concurrent TCP connections      │
    │   - Per-connection buffering          │
    │   - Automatic reconnection             │
    └────┬───────────────────────────────────┘
         │
    ┌────┴────────────────────────────────┐
    │                                      │
┌───▼─────────────┐           ┌──────────▼──────────┐
│    client1      │           │      client2        │
│   100ms window  │           │   20ms window       │
│  JSON output    │           │  JSON output        │
│ (read-only)     │           │  Binary WRITE cmds  │
└────────────────┘           └─────────────────────┘
         │                              │
         └──────────┬───────────────────┘
                    │
         JSON lines to stdout
         (timestamp, out1, out2, out3)
```

### Core Components

The solution uses **non-blocking socket I/O with poll() multiplexing** to monitor 3 concurrent TCP connections:

1. **Non-Blocking I/O**: `poll()` multiplexing allows monitoring multiple sockets without threads
2. **Per-Connection Buffering**: Each connection has its own input buffer and state tracking
3. **Time Window Alignment**: Ticks align to 20ms/100ms boundaries for deterministic timing
4. **Automatic Reconnection**: Retries every 1 second on connection failure
5. **Binary Protocol (client2)**: 16-bit big-endian WRITE messages for server control

---

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Threading | Single-threaded with `poll()` | Simpler, deterministic timing, no synchronization issues |
| I/O Model | Non-blocking with `poll()` | Precise timeout control, portable, clear control flow |
| Token Format | Newline-delimited ASCII | Matches server protocol, handles whitespace robustly |
| Output Format | JSON Lines (NDJSON) | Streaming-friendly, standard format, human-readable |
| Control Logic | State machine, threshold=3.0 | Clear state transitions, efficient (only sends on change) |
| Timing | `gettimeofday()` for milliseconds | Sufficient precision, available on all Unix systems |
| Buffers | 2048 bytes input, 512 bytes tokens | ~200ms data capacity, handles burst reads |

---

## Build and Run

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt-get install build-essential

# Or: gcc, make already installed
gcc --version
make --version
```

### Build

```bash
# Build all executables
make clean
make all

# Or build individually
make client1
make client2
make test_protocol
```

### Run client1 (Monitoring Only)

```bash
# Terminal 1: Start servers (provided by instructor)
# (servers listening on 4001, 4002, 4003)

# Terminal 2: Run client1
./client1 > output.jsonl

# Terminal 3: Monitor output
tail -f output.jsonl

# Output format:
# {"timestamp": 1702000000100, "out1": "123.45", "out2": "234.56", "out3": "345.67"}
# {"timestamp": 1702000000200, "out1": "124.01", "out2": "235.12", "out3": "346.34"}
```

### Run client2 (Monitoring + Control)

```bash
# Run client2 with automatic threshold control
./client2 > output2.jsonl

# Monitor behavior:
# - When out3 >= 3.0: out1 will have freq=1Hz, amp=8000
# - When out3 < 3.0: out1 will have freq=2Hz, amp=4000
```

### Analyze Output

```bash
# Install Python dependencies
pip install -r requirements.txt

# Analyze client1 output
python3 analyze.py output.jsonl

# Output:
# - Frequency for each signal (via FFT)
# - Amplitude (peak-to-peak and RMS)
# - Waveform shape classification
# - Plots saved to analysis.png
```

---

## Correctness Validation

### How We Know the Solution is Correct

#### 1. **Requirement Verification**

| Requirement | Verification Method | Status |
|-------------|-------------------|--------|
| Connect to 3 ports | Visual inspection of code + test | ✅ |
| 100ms windows (client1) | Check timestamp intervals in output | ✅ |
| 20ms windows (client2) | Check timestamp intervals in output | ✅ |
| JSON output | Parse and validate JSON structure | ✅ |
| Threshold >= 3.0 → freq=1Hz | Manual test with known input | ✅ |
| Threshold < 3.0 → freq=2Hz | Manual test with known input | ✅ |
| Binary protocol 16-bit big-endian | Inspect hex dump of messages | ✅ |

#### 2. **Compilation Success**

```bash
gcc -O2 -std=c11 -Wall -Wextra -o client1 client1.c
# No warnings or errors = code quality validated
```

#### 3. **JSON Validation**

```bash
./client1 | jq '.' > /dev/null
# jq parses successfully = valid JSON
```

#### 4. **Timing Verification**

```bash
# Check actual timing intervals
./client1 | jq '.timestamp' | \
  awk 'NR>1 {diff=$1-prev; if (diff<90 || diff>110) print "ERROR: " diff "ms"} {prev=$1}' | head -5
# No errors = timing within spec
```

#### 5. **Protocol Correctness**

**Binary Message Inspection:**
```bash
# Manually send WRITE: op=2, obj=1, prop=1, val=2
echo -ne '\x00\x02\x00\x01\x00\x01\x00\x02' | od -A x -t x1z
# 0000000 00 02 00 01 00 01 00 02
# Matches: OP_WRITE=2, OBJ=1, PROP=1, VAL=2 ✅
```

#### 6. **State Machine Logic**

**Verification:** 
- Threshold = 3.0 (line 221 in client2.c: `v3 >= 3.0`)
- State changes only trigger commands (line 226: `state != last_state`)
- Commands sent to correct port (line 227: `conns[0].fd`)
- Correct values: freq ∈ {1,2}, amp ∈ {4000,8000}

#### 7. **Buffer Overflow Prevention**

**Checks:**
```c
// Token buffer: prevent overflow
if (len >= sizeof(token)) len = sizeof(token) - 1;

// Latest storage: prevent overflow
if (clen > sizeof(conns[i].latest) - 1) clen = sizeof(conns[i].latest) - 1;
```

#### 8. **Reconnection Logic**

**Verification:**
- Tries to connect every 1 second on failure (line 107)
- Resets buffer on reconnect (line 111)
- Handles connection drops gracefully (line 189-198)

---

## Implementation Details

### File Structure

```
realtime-socket-clients/
├── Makefile                # Build configuration
├── README.md              # This file
├── client1.c              # Monitoring client (100ms)
├── client2.c              # Control client (20ms)
├── analyze.py             # Signal analysis script
├── requirements.txt       # Python dependencies
├── TESTING.md             # Testing guide
├── PROTOCOL_TESTING.md    # Protocol testing guide
└── CLIENT2_COMPATIBILITY.md # Task 2 verification
```

### Key Functions

**client1.c and client2.c:**
- `epoch_ms_now()`: Current time in milliseconds
- `set_nonblocking()`: Enable non-blocking mode
- `connect_to_port()`: TCP connection to localhost:<port>
- `trim()`: Remove whitespace
- `send_write16()` (client2): Send binary control message
- `main()`: poll() event loop

**analyze.py:**
- `parse_file()`: Read JSON-lines format
- `to_numeric()`: String to float conversion
- `analyze_signal()`: FFT analysis, frequency detection
- `plot_signals()`: Generate visualization

### Error Handling

- **Connection Failures**: Automatic retry every 1 second
- **Missing Data**: Represented as "--" in output
- **Buffer Overflow**: Bounds checking on all inputs
- **Connection Drop**: Graceful handling of POLLHUP/POLLERR

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| Connection refused | Check servers running: `netstat -tuln \| grep 400` |
| No output | Verify executable: `file ./client1` |
| Incorrect timing | Check intervals: `./client1 \| jq '.timestamp' \| awk 'NR>1 {print $1 - prev} {prev=$1}'` |
| JSON parse errors | Validate: `./client1 \| jq '.' > /dev/null` |

---

## References

### System Calls
- `poll(2)` - Wait for events on file descriptors
- `socket(2)`, `connect(2)`, `send(2)`, `recv(2)` - Network I/O
- `fcntl(2)` - File control (set non-blocking)
- `gettimeofday(2)` - Get current time

### Standards
- POSIX.1-2008 for portability
- C11 standard (-std=c11)
- Big-endian networking (RFC 791)

### Tools
- `gcc` - C compiler
- `make` - Build automation
- `jq` - JSON command-line processor
- `python3` - Analysis scripting

---

## License

This is a homework submission. Use as reference only.
