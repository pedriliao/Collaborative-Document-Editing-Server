# ZOIT Docs - Collaborative Document Editing Server

A multiclient document editing server implemented in C that supports real-time collaboration. The system uses POSIX threads and FIFO-based communication to handle concurrent command queues, and features a piece-table-like chunk structure with version control to manage edits efficiently and broadcast changes consistently across clients.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                     Server Process                       │
│                                                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐              │
│  │  Client   │  │  Client   │  │  Timer   │              │
│  │ Thread 1  │  │ Thread 2  │  │  Thread  │              │
│  │           │  │           │  │          │              │
│  │ cmd queue │  │ cmd queue │  │ periodic │              │
│  │ (mutex)   │  │ (mutex)   │  │  sync    │              │
│  └─────┬─────┘  └─────┬─────┘  └────┬─────┘              │
│        │              │              │                    │
│        └──────────────┼──────────────┘                    │
│                       │                                   │
│              ┌────────▼────────┐                          │
│              │  Global Queue   │                          │
│              │  (sorted by     │                          │
│              │   timestamp)    │                          │
│              └────────┬────────┘                          │
│                       │                                   │
│              ┌────────▼────────┐                          │
│              │    Document     │                          │
│              │  (Chunk-based   │                          │
│              │   Piece Table)  │                          │
│              └─────────────────┘                          │
└─────────────────────────────────────────────────────────┘
         ▲                              ▲
         │ FIFO_C2S/S2C                 │ FIFO_C2S/S2C
         │                              │
    ┌────┴─────┐                  ┌─────┴────┐
    │ Client 1 │                  │ Client 2 │
    │ (editor) │                  │ (editor) │
    └──────────┘                  └──────────┘
```

### Key Design Decisions

1. **Piece-Table-Like Chunk Structure**: Instead of a simple character array (O(n) shifts per insert), the document uses a linked list of chunks referencing an append-only buffer. This provides O(1) insertions and supports efficient version-aware operations.

2. **Soft Deletion with Versioning**: Chunks are never physically removed. They are marked as deleted with a `deletion_version`, enabling flattening at any committed version for synchronization.

3. **FIFO-Based IPC**: Each client communicates via two named FIFOs (`FIFO_C2S_<pid>` and `FIFO_S2C_<pid>`), using real-time signals (`SIGRTMIN`/`SIGRTMIN+1`) for connection handshaking.

4. **Timestamp-Based Global Ordering**: Commands from concurrent clients are tagged with nanosecond timestamps and globally sorted before execution, ensuring deterministic results regardless of thread scheduling.

## Project Structure

```
.
├── Makefile                 # Build configuration
├── libs/
│   ├── document.h           # Core data structures (chunk, ChunkTable, document)
│   ├── command.h            # Client, Command, and CommandLog structures
│   └── markdown.h           # Public API for the document engine
├── source/
│   ├── markdown.c           # Document engine implementation (piece table, formatting)
│   ├── server.c             # Server: multi-threaded client management & sync
│   └── client.c             # Client: FIFO connection, local doc mirror, user I/O
├── tests/
│   └── test_markdown.c      # Comprehensive unit tests (60+ test cases)
└── roles.txt                # User permission file (username → read/write)
```

## Build

```bash
# Build server and client
make

# Build and run tests
make test

# Clean build artifacts
make clean
```

**Requirements**: GCC with C11 support, POSIX threads (`-lpthread`), Linux environment (named FIFOs, real-time signals).

## Usage

### Start the Server

```bash
./server <TIME_INTERVAL_MS>
```

- `TIME_INTERVAL_MS`: Synchronization interval in milliseconds
- The server prints its PID on startup: `Server PID: <pid>`

### Connect a Client

```bash
./client <server_pid> <username>
```

- `server_pid`: PID printed by the server
- `username`: Must be listed in `roles.txt`

### roles.txt Format

```
bob     write
eve     read
ryan    write
```

### Supported Commands

| Command | Description |
|---------|-------------|
| `INSERT <pos> <content>` | Insert text at cursor position |
| `DEL <pos> <count>` | Delete characters from position |
| `NEWLINE <pos>` | Insert newline |
| `HEADING <level> <pos>` | Insert heading (level 1-3) |
| `BOLD <start> <end>` | Bold formatting |
| `ITALIC <start> <end>` | Italic formatting |
| `CODE <start> <end>` | Inline code formatting |
| `BLOCKQUOTE <pos>` | Blockquote formatting |
| `ORDERED_LIST <pos>` | Ordered list item |
| `UNORDERED_LIST <pos>` | Unordered list item |
| `HORIZONTAL_RULE <pos>` | Horizontal rule |
| `LINK <start> <end> <url>` | Hyperlink |
| `DISCONNECT` | Graceful disconnect |
| `DOC?` | Print document |
| `PERM?` | Show permission |
| `LOG?` | Show command log |

## Testing

The test suite covers:

- **Lifecycle**: Document initialization and cleanup
- **Edit commands**: Insert (beginning, middle, end), delete (partial, full, beyond-end)
- **Formatting**: All 10 formatting commands with edge cases
- **Version control**: Multi-version progression, outdated version rejection, pre-commit flattening
- **Ordered lists**: Auto-renumbering on insert, delete, and newline split
- **Error handling**: Out-of-bounds positions, invalid ranges, wrong versions
- **Memory safety**: Runs clean under AddressSanitizer (ASAN)

```bash
make test
# Output: === Results: N passed, 0 failed ===
```

## Technical Highlights

- **Zero-copy buffer**: All text lives in a single append-only buffer; chunks reference it via (offset, length) pairs
- **Lock-free reads**: Document flattening is read-only and does not require locks
- **Concurrent command queuing**: Per-client mutex-protected queues drained atomically by the timer thread
- **Signal-based connection**: Uses POSIX real-time signals for race-free client registration

---

# ZOIT Docs - 协作文档编辑服务器

一个用 C 语言实现的多客户端文档编辑服务器，支持实时协作。系统使用 POSIX 线程和基于 FIFO 的通信来处理并发命令队列，采用类似 piece-table 的 chunk 结构配合版本控制来高效管理编辑操作，并在所有客户端之间一致地广播变更。

## 架构设计

### 核心设计决策

1. **类 Piece-Table 的 Chunk 结构**：不使用简单的字符数组（每次插入需要 O(n) 的移动），文档使用链表结构的 chunk 引用一个只追加的缓冲区。这提供了 O(1) 的插入操作，并支持高效的版本感知操作。

2. **带版本控制的软删除**：Chunk 永远不会被物理删除。它们通过 `deletion_version` 标记为已删除，使得系统可以在任何已提交的版本上展平文档，用于同步。

3. **基于 FIFO 的进程间通信**：每个客户端通过两个命名管道（`FIFO_C2S_<pid>` 和 `FIFO_S2C_<pid>`）通信，使用实时信号（`SIGRTMIN`/`SIGRTMIN+1`）进行连接握手。

4. **基于时间戳的全局排序**：来自并发客户端的命令使用纳秒级时间戳标记，并在执行前全局排序，确保无论线程调度如何都能产生确定性结果。

## 项目结构

```
.
├── Makefile                 # 构建配置
├── libs/
│   ├── document.h           # 核心数据结构（chunk, ChunkTable, document）
│   ├── command.h            # Client, Command 和 CommandLog 结构体
│   └── markdown.h           # 文档引擎公共 API
├── source/
│   ├── markdown.c           # 文档引擎实现（piece table，格式化）
│   ├── server.c             # 服务器：多线程客户端管理与同步
│   └── client.c             # 客户端：FIFO 连接，本地文档镜像，用户 I/O
├── tests/
│   └── test_markdown.c      # 全面的单元测试（60+ 测试用例）
└── roles.txt                # 用户权限文件（用户名 → read/write）
```

## 构建

```bash
# 构建服务器和客户端
make

# 构建并运行测试
make test

# 清理构建产物
make clean
```

**依赖**：支持 C11 的 GCC，POSIX 线程（`-lpthread`），Linux 环境（命名管道，实时信号）。

## 使用方法

### 启动服务器

```bash
./server <同步间隔毫秒数>
```

### 连接客户端

```bash
./client <服务器PID> <用户名>
```

### 支持的命令

| 命令 | 说明 |
|------|------|
| `INSERT <pos> <content>` | 在光标位置插入文本 |
| `DEL <pos> <count>` | 从指定位置删除字符 |
| `NEWLINE <pos>` | 插入换行符 |
| `HEADING <level> <pos>` | 插入标题（1-3级） |
| `BOLD <start> <end>` | 加粗格式化 |
| `ITALIC <start> <end>` | 斜体格式化 |
| `CODE <start> <end>` | 行内代码格式化 |
| `BLOCKQUOTE <pos>` | 引用格式化 |
| `ORDERED_LIST <pos>` | 有序列表项 |
| `UNORDERED_LIST <pos>` | 无序列表项 |
| `HORIZONTAL_RULE <pos>` | 水平分割线 |
| `LINK <start> <end> <url>` | 超链接 |
| `DISCONNECT` | 优雅断开连接 |

## 测试

测试套件覆盖：

- **生命周期**：文档初始化和清理
- **编辑命令**：插入（开头、中间、末尾），删除（部分、全部、超出末尾）
- **格式化**：全部 10 种格式化命令及边界情况
- **版本控制**：多版本推进、过期版本拒绝、提交前展平
- **有序列表**：插入、删除和换行分割时的自动重编号
- **错误处理**：越界位置、无效范围、错误版本
- **内存安全**：在 AddressSanitizer (ASAN) 下干净运行

```bash
make test
# 输出: === Results: N passed, 0 failed ===
```

## 技术亮点

- **零拷贝缓冲区**：所有文本存储在单一的只追加缓冲区中；chunk 通过 (offset, length) 对引用它
- **无锁读取**：文档展平操作是只读的，不需要锁
- **并发命令队列**：每个客户端的互斥锁保护队列由定时器线程原子性地排空
- **基于信号的连接**：使用 POSIX 实时信号实现无竞争的客户端注册
