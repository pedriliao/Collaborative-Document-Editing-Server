# ZOIT Docs 面试准备文档

---

## 一、项目目标

> "这是一个单机多进程的协作文档编辑服务器，允许多个客户端同时连接、对同一份文档做编辑，服务器保证所有客户端最终看到一致的文档状态。"

核心挑战：多个客户端并发编辑同一份文档，如何保证最终一致性，如何高效处理并发命令。

---

## 二、为什么选多线程，而不是多进程

### 需求推导

- 多个客户端同时连接，每个客户端随时可能发命令
- 服务器不能因为在等一个客户端而卡住另一个

### 方案对比

| 方案 | 问题 |
|------|------|
| 单进程单线程 | 阻塞在一个客户端的 `read` 上，其他客户端无法响应 |
| 多进程（fork） | fork 出的子进程是父进程内存的完整拷贝，两者内存从此独立，共享文档状态困难，需要额外引入共享内存 + 信号量 |
| **多线程（pthread）** | 同一进程内所有线程天然共享 `doc` 全局变量，同步成本低，直接选这个 |

### fork 为什么不行（详细解释）

```
父进程 (doc*)  ──fork──→  子进程A (doc的完整拷贝)
                 └──fork──→  子进程B (doc的另一个拷贝)
```

子进程A修改了文档，子进程B完全不知道，因为它操作的是自己那份独立的拷贝。要共享同一份文档，必须用 `shm_open` + `sem_t`，复杂度大幅上升。

### 多线程是真并发还是像 Python GIL 一样

C 的 `pthread` 是**真并发**，没有 GIL。多个 per-client 线程可以在不同 CPU 核上同时跑，同时从各自的 FIFO 读数据，同时往各自的队列写命令。

但**文档操作是串行的**——这是设计决策，timer 线程一次只执行一个命令，文档永远不会被并发修改，不需要给文档加锁。这个设计叫**生产者-消费者模型**，读命令（并发）和执行命令（串行）分离。

---

## 三、服务器线程架构

服务器进程内有四类线程：

```
main thread       ← 阻塞等待 SIGRTMIN，每来一个客户端就创建一个新线程
per-client thread ← 每个客户端一个，从 FIFO 读命令，打时间戳，放进队列
timer thread      ← 每隔 N 毫秒，合并所有队列、排序、串行执行、广播结果
terminal thread   ← 监听服务器 stdin，处理 DOC?/LOG?/QUIT 运维命令
```

关键代码（`server.c:302`）：

```c
pthread_create(&cli->thread, NULL, client_thread, cli);
```

---

## 四、进程间通信（IPC）

### 选择：Named FIFO + 实时信号

**为什么不用 socket？**
项目是单机多进程，不需要跨网络。FIFO 的 `open/read/write` 接口和文件完全相同，不需要额外网络库，简单够用。如果需要跨网络多机协作，则换 TCP socket。

**为什么用实时信号（SIGRTMIN）而不是普通信号（SIGUSR1）？**

| 特性 | 普通信号 | 实时信号 |
|------|----------|----------|
| 多个同类信号pending | 第二个被丢弃 | 排队，每个都能收到 |
| 携带发送方信息 | 不携带 | `siginfo_t.si_pid` 直接知道是谁发的 |

### 握手流程（类比 TCP 三次握手）

```
TCP 三次握手          这个项目的握手
─────────────────     ──────────────────────────────────────────
SYN              ≈    客户端 kill(server, SIGRTMIN)
SYN-ACK          ≈    服务器 mkfifo + kill(client, SIGRTMIN+1)
ACK              ≈    双方 open() 互相阻塞，两端都打开才解除（FIFO 机制自动保证）
```

区别：TCP 三次握手解决丢包和序列号同步；这里的握手借 FIFO `open` 的阻塞特性实现双端同步，不需要显式发 ACK 包。

完整握手代码流程：

```c
// 1. 客户端发信号，服务器从 sigwaitinfo 得到 si_pid = client_pid
kill(server_pid, SIGRTMIN);

// 2. 服务器建管道，通知客户端
mkfifo("FIFO_C2S_<pid>", 0666);
mkfifo("FIFO_S2C_<pid>", 0666);
kill(client_pid, SIGRTMIN + 1);

// 3. 双方各自 open，互相阻塞直到对端也打开
int fd_c2s = open(fifo_c2s, O_RDONLY);   // 服务器
int fd_c2s = open(fifo_c2s, O_WRONLY);   // 客户端
// 两端都 open 后，阻塞自动解除，连接建立
```

---

## 五、线程间同步机制

### 每个客户端一个队列，一个锁

```c
typedef struct Client {
    Command *queue;          // 动态数组队列
    size_t queue_len;
    size_t queue_cap;
    pthread_mutex_t queue_lock;  // 保护队列的独立锁
    ...
} Client;
```

### 写队列（per-client 线程）

```c
pthread_mutex_lock(&cli->queue_lock);
cli->queue[cli->queue_len++] = *now_comm;   // 入队
pthread_mutex_unlock(&cli->queue_lock);      // 持锁时间极短
```

### 读队列（timer 线程）

```c
// 对每个 client 依次：
pthread_mutex_lock(&cli->queue_lock);
// 把命令复制到本地 merge[]
cli->queue_len = 0;             // 清空队列
pthread_mutex_unlock(&cli->queue_lock);  // 立即解锁

// 排序和执行都在锁外进行，不阻塞客户端线程继续入队
qsort(merge, merge_len, sizeof(Command), compare_timestamp);
for (size_t i = 0; i < merge_len; i++) handle_command(&merge[i]);
```

---

## 六、完整调用链路

```
1. 用户输入 "INSERT 5 Hello"
         ↓
2. per-client 线程 read(cli->c2s) 收到
         ↓
3. 打时间戳：get_timestamp_ns() → 纳秒级时间戳
         ↓
4. lock → 入队 → unlock（持锁极短）
         ↓
   [线程继续等下一条命令，不阻塞]

5. N毫秒后，timer 线程醒来：
   ├── 对每个 client：lock → 复制命令到 merge[] → queue_len=0 → unlock
   ├── qsort(merge, 按时间戳升序)
   ├── for 每条命令：handle_command() → 修改 doc → 记录日志
   ├── markdown_increment_version(doc)   // version++
   └── broadcast_version()              // 广播给所有客户端
```

---

## 七、底层数据结构

### 三层嵌套结构

```
document
├── version: uint64_t          ← 当前版本号，每轮 timer 执行后 +1
└── ct: ChunkTable
    ├── buf: char*             ← 只追加的大内存块，所有文字都存在这里
    ├── buf_size: size_t       ← 已用字节数
    ├── buf_capacity: size_t   ← 总分配字节数（满了就 *2 扩容）
    └── head: chunk*           ← 链表头指针，按文档顺序串联
```

### chunk 结构

```c
typedef struct chunk {
    size_t offset;           // 在 buf 里从第几个字节开始
    size_t length;           // 取几个字节
    uint64_t version;        // 哪个版本创建的
    struct chunk *next;      // 下一个 chunk（链表）
    bool deleted;            // 是否被软删除
    uint64_t deletion_version; // 哪个版本被删除的
} chunk;
```

### 为什么不用 char[] 直接存文档

用普通字符数组，每次在中间插入一个字符，需要把插入点后面的所有内容整体后移，是 O(n) 操作。文档越长，每次编辑越慢。

Piece Table 的思路：文本只追加到 buffer，永不移动。文档结构用 chunk 链表描述，每个 chunk 记录"从 buffer 哪个 offset 开始、取多少字节"。插入时追加到 buffer 末尾 O(1)，更新链表指针 O(1)。

---

## 八、文档删改操作详解

### INSERT（插入）

> "插入不移动任何已有内存。新文本追加到 `buf` 末尾，新建一个 chunk 记录 offset 和 length。如果插入点在某个 chunk 中间，把那个 chunk 劈成左右两半，新 chunk 插在中间。"

```
插入 "X" 到位置5，buf 原来是 "Hello World"

buf:  [ H e l l o   W o r l d X ]   ← X 追加到末尾，offset=11

链表: [0,5,"Hello"] → [11,1,"X"] → [6,5,"World"] → NULL
                            ↑
                        新建的 chunk，插在中间
```

### DEL（删除）——软删除

> "不从链表里摘掉 chunk，只打两个标记：`deleted=true`，`deletion_version=当前版本`。buf 里的内容永远不动。"

```c
chunk->deleted = true;
chunk->deletion_version = doc->version;
```

**为什么不物理删除？** 多客户端同步需要版本感知。`markdown_flatten` 渲染时判断：

```c
// 可见条件：创建版本早于当前，且没被删除
cu->version <= doc->version
&& !(cu->deleted && cu->deletion_version <= doc->version)
```

物理删除会丢失历史信息，无法重建过去任意版本的文档状态。

### 格式化命令（BOLD、ITALIC 等）——本质是两次 INSERT

> "以 BOLD 为例，在 start 位置插入 `**`，在 end 位置插入 `**`。内部用 `suppress_version_bump` flag 让两次插入用同一个版本号，不触发两次版本递增，作为同一个逻辑操作提交。"

### flatten（渲染文档）

> "遍历两次 chunk 链表：第一遍统计所有可见 chunk 的总长度，malloc 一个字符串；第二遍 memcpy 每个可见 chunk 对应的 buf 内容进去。时间复杂度 O(k)，k 是链表总长度（含历史 chunk）。"

---

## 九、客户端本地文档同步

### 加入时初始化

握手完成后，服务器按顺序发三样东西，客户端顺序读：

```
服务器发           客户端收
──────────         ─────────────────────────────
"write\n"     →   role = "write"
"5\n"         →   local_version = 5
"11\n"        →   doc_len = 11
"Hello World" →   read 循环，读满 11 字节进 buf[]
```

建立本地文档（`client.c:470-473`）：

```c
doc = markdown_init();            // 建一个空文档
markdown_insert(doc, 0, 0, buf); // 把收到的全文一次性插入位置0
doc->version = local_version;    // 版本号对齐服务器当前版本
```

### 后续增量同步

listener 线程持续读 `FIFO_S2C`，收到广播后：

```
服务器广播                      客户端 listener 线程处理
──────────────────────────      ──────────────────────────────────────
VERSION 5                  →    broadcast_version = 5
EDIT alice INSERT 3 X      →    SUCCESS + broadcast_version==doc->version
  SUCCESS                  →    → handle_command_local(INSERT 3 X) 本地重放
EDIT bob DEL 0 2           →    Reject → 忽略，本地不动
  Reject UNAUTHORISED
END                        →    markdown_increment_version(doc)，local version: 5→6
```

**关键设计**：客户端不拉全量文档，只收 SUCCESS 的命令在本地重放。`broadcast_version == doc->version` 保证按序应用，不乱序。

### 客户端两个线程分工

```
main 线程        ← 读用户 stdin，写到 FIFO_C2S 发给服务器
listener 线程    ← 读 FIFO_S2C 收服务器广播，更新本地 doc
```

两者完全独立，main 线程发命令不等结果，listener 线程异步收广播，这就是能实时看到别人编辑的原因。

---

## 十、冲突解决策略

### 当前方案：时间戳排序串行执行

per-client 线程收到命令时立即打纳秒时间戳，timer 线程把所有客户端命令合并后按时间戳 `qsort` 升序，然后串行执行。结果确定，所有客户端收到相同顺序的广播，最终状态一致。

**潜在问题：**

1. **时钟精度**：`CLOCK_REALTIME` 可能受 NTP 校时影响向前或向后跳，更好的选择是 `CLOCK_MONOTONIC`（单调递增）
2. **时间戳相等**：`qsort` 对相等元素的排序不稳定，结果不确定。修复方法：加次级比较键（如客户端 ID）

### 对比 OT（操作变换）

| | 当前方案（时间戳串行） | OT（操作变换） |
|--|--|--|
| 冲突结果 | 确定，但位置可能跑偏 | 符合用户意图 |
| 实现复杂度 | 低 | 高（多操作组合正确性难保证） |
| 适用场景 | 低并发、冲突率低 | Google Docs 级高并发协作 |

---

## 十一、可以升级的地方

### 1. 通信层换 TCP Socket（影响最大）

> "现在用 FIFO 只能单机通信。换成 TCP socket 后，客户端可以在任意机器上连接，只需要知道 IP 和端口。通信接口几乎一样（`read/write`），改动成本相对可控。"

### 2. 客户端上限从静态数组改为动态

```c
// 现在：栈上固定 32 个槽位
Client clients[MAX_CLIENTS];

// 改为：动态链表或 realloc 扩容数组
```

> "上限只取决于系统文件描述符数量，而不是硬编码的 32。"

### 3. 异常断开检测（现存 bug）

`server.c` 的 per-client 线程，`read` 返回 0 时没有 break，客户端崩溃会导致线程忙等：

```c
// 修复：
if (bytes <= 0) break;  // 然后走断开连接的清理逻辑
```

### 4. Timer 加条件变量，减少空转

> "现在 timer 固定间隔唤醒，不管有没有命令。加 `pthread_cond_t`，有命令入队时立刻唤醒 timer，空闲时再等固定间隔，降低 CPU 空转。"

### 5. Chunk 链表 GC，防止无限增长

> "历史 chunk 永远不释放，链表只增不减，`flatten` 越来越慢。可以定期把所有可见 chunk 合并成一个大 chunk，丢弃历史 chunk，链表长度归零。代价是丢失无限回溯历史的能力，按需取舍。"

### 6. 持久化（WAL）

> "现在服务器重启数据全丢。升级方向：每次版本提交后把操作日志 append 到磁盘（Write-Ahead Log），重启后重放日志恢复状态，即使崩溃也不丢数据。"

### 7. 冲突解决升级为 OT/CRDT

> "时间戳串行在并发冲突时位置会跑偏。引入 OT 可以让并发编辑结果更符合用户意图，但实现复杂度最高，是长期演进方向。"

---

## 十二、面试一句话总结

> "服务器进程内用四类 POSIX 线程实现并发：每个客户端一个线程负责读命令、打时间戳入队；一个 timer 线程定期合并所有队列、按时间戳排序后串行执行，保证所有客户端看到一致顺序。客户端和服务器之间用命名管道通信，连接握手用实时信号避免普通信号的丢失问题。文档用 Piece Table 实现，所有编辑只追加 buffer、改链表指针，删除用软删除保留版本历史。客户端维护本地文档副本，通过增量广播重放 SUCCESS 命令保持同步，不需要每次拉全量。"
