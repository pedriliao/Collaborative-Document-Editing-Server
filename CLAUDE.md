# ZOIT Docs 面试问答全集

---

## 数据结构

**Q1. 为什么不用 `char[]` 存文档？Piece Table 解决了什么问题？**

> 用普通字符数组，每次在中间插入一个字符，需要把插入点后面的所有内容整体后移一格，这是 O(n) 操作。文档越长，每次编辑越慢。
>
> Piece Table 的思路是：文本只追加到一块 buffer，永不移动。文档结构用一个 chunk 链表描述——每个 chunk 记录"从 buffer 的哪个 offset 开始、取多少字节"。插入时把新文本追加到 buffer 末尾（O(1)），然后更新链表指针（O(1)）。代价是读取/渲染需要遍历链表，但编辑密集的场景下插入性能远好于字符数组。

---

**Q2. 你的 chunk 链表插入一个字符，具体发生了哪些步骤？**

> 以 `INSERT 5 X` 为例，文档当前是 `"abcde12345"`：
>
> 1. `append_to_buf`：把 `"X"` 追加到 buffer 末尾，返回其 offset
> 2. `malloc` 一个新 chunk，记录 offset 和 length=1
> 3. `find_chunk`：遍历链表，找到 pos=5 落在哪个 chunk 里，以及在该 chunk 内的偏移
> 4. `split_chunk`：如果 pos 落在 chunk 中间，把那个 chunk 劈成左右两半
> 5. 把新 chunk 插入链表：`new_chunk->next = right_half; left_half->next = new_chunk`
>
> buffer 里的数据始终不动，只有链表指针变了。

---

**Q3. 软删除是什么？为什么不直接把 chunk 从链表里摘掉？**

> 软删除是指删除时不从链表里摘掉 chunk，只打两个标记：`deleted = true`，`deletion_version = 当前版本`。
>
> 原因是多客户端同步需要版本感知能力。`markdown_flatten` 在重建文档字符串时，会判断 `cu->deleted && cu->deletion_version <= doc->version`——如果删除发生在当前版本之前，这个 chunk 就不可见；如果删除发生在当前版本之后，这个 chunk 还应该存在。物理删除会丢失这个历史信息，就无法重建过去任意版本的文档状态了。

---

**Q4. `markdown_flatten` 是怎么工作的？时间复杂度是多少？**

> 两次遍历 chunk 链表：
>
> - 第一遍：累加所有可见 chunk 的 length，算出总长度，`malloc` 足够大的字符串
> - 第二遍：把所有可见 chunk 对应的 buffer 内容 `memcpy` 进结果字符串
>
> 可见条件：`cu->version <= doc->version && !(cu->deleted && cu->deletion_version <= doc->version)`
>
> 时间复杂度 O(k)，k 是链表中 chunk 的总数（包括已删除的）。随着编辑次数增加，k 会持续增大，这是 Piece Table 的已知代价——历史 chunk 永远不释放。

---

**Q5. append-only buffer 满了怎么办？扩容策略是什么？**

> 在 `append_to_buf` 里处理：
>
> ```c
> size_t new_cap = ct->buf_capacity * 2;
> while (new_cap < ct->buf_size + len + 1) new_cap *= 2;
> char *new_buf = realloc(ct->buf, new_cap);
> ```
>
> 容量翻倍，如果翻倍后还不够（比如一次插入的文本特别长），继续翻倍直到够用。这是标准的动态数组扩容策略，均摊 O(1) 追加。`realloc` 可能在内存不足时返回 NULL，代码里有检查，失败时返回 `(size_t)-1`。

---

**Q6. `version` 和 `deletion_version` 字段分别用来做什么？**

> - `chunk->version`：这个 chunk 是在第几轮 timer 同步后创建的。`markdown_flatten` 只包含 `version <= doc->version` 的 chunk，防止还没提交的 chunk 提前可见
> - `chunk->deletion_version`：这个 chunk 在第几轮被删除。flatten 时用来判断"在某个历史版本，这个 chunk 是否应该出现"
>
> 两个字段合在一起，描述了一个 chunk 的"生命周期区间"：从 `version` 创建，到 `deletion_version` 消失。

---

## 并发与同步

**Q7. 服务器有几种线程？各自负责什么？**

> 四种：
>
> - **main thread**：调用 `sigwaitinfo` 阻塞等待 SIGRTMIN，每来一个客户端就调用 `make_client_thread` 创建新线程
> - **per-client thread**（每个客户端一个）：从 FIFO 读命令，`DISCONNECT/DOC?/PERM?` 立即处理，其他编辑命令打时间戳放进该客户端的私有队列
> - **timer thread**（一个）：每隔 N 毫秒执行一次 `collect_and_process_commands`，drain 所有队列、排序、执行、广播
> - **terminal thread**（一个）：监听服务器 stdin，处理 `DOC? / LOG? / QUIT` 运维命令

---

**Q8. per-client 线程和 timer 线程之间怎么传数据？用了什么同步原语？**

> 通过每个 `Client` 结构体里的动态数组队列 `cli->queue` 传数据。
>
> 保护机制是 `pthread_mutex_t queue_lock`：
> - per-client 线程写队列时：`mutex_lock → append → mutex_unlock`
> - timer 线程 drain 队列时：`mutex_lock → 复制所有命令 → queue_len=0 → mutex_unlock`
>
> 注意 timer 线程 drain 完后立刻释放锁，然后才去排序执行，所以持锁时间很短，不会长时间阻塞客户端线程写入。

---

**Q9. 两个客户端同时发命令，服务器怎么决定执行顺序？**

> 用纳秒时间戳。per-client 线程收到命令时立即调用 `get_timestamp_ns()`（`clock_gettime(CLOCK_REALTIME, ...)`）打上时间戳，存进 `Command.timestamp`。
>
> timer 线程把所有客户端的命令合并后，调用 `qsort` 按 timestamp 升序排列，然后按这个顺序逐个执行。时间戳早的命令先执行，结果确定，所有客户端收到相同顺序的广播，最终状态一致。

---

**Q10. 同一批次内的两个命令，版本检查会拒绝吗？为什么？**

> **不会。** 服务器调用 `handle_command` 时传的是 `doc->version`，而 `doc->version` 在整个批次执行期间不变——只有批次全部执行完后才调用 `markdown_increment_version`。
>
> 所以同一批次里不管有多少条命令，每条都传同一个版本号，`markdown_insert` 里的 `version != doc->version` 检查永远不会触发。两条命令都 SUCCESS，按时间戳顺序串行执行在同一份文档上。

---

**Q11. `suppress_version_bump` 这个全局 flag 是干什么用的？有没有线程安全问题？**

> 它是一个内部控制 flag，用于格式化命令内部的复合操作。比如 `markdown_bold` 需要在 start 和 end 两处分别插入 `**`，这两次插入应该属于同一个逻辑操作，用同一个版本号。把 `suppress_version_bump` 设为 `true` 后，内部调用 `markdown_insert` 时 chunk 的 version 用 `version` 而不是 `version+1`。
>
> **有线程安全问题。** 这是一个 `static bool`，没有任何保护。如果两个 per-client 线程同时触发格式化命令，flag 会互相干扰。不过实际上所有文档操作都在 timer 线程里串行执行，所以实践中不会并发触发，这个问题被架构上的串行化掩盖了。

---

**Q12. timer 线程 drain 队列时，per-client 线程同时在往队列里写，怎么保证安全？**

> 每个 client 有独立的 `queue_lock` mutex。timer 线程遍历所有 client，对每个 client 依次加锁、drain、解锁，再处理下一个。per-client 线程写队列时也要先加同一把锁。
>
> 关键设计：timer drain 时把命令复制出来，然后把 `cli->queue_len = 0`，再解锁。之后的排序和执行都在 timer 自己的局部 `merge` 数组上进行，不再持有任何 client 的锁，所以执行阶段不阻塞客户端线程继续入队。

---

## 进程间通信

**Q13. 客户端和服务器之间用什么方式通信？为什么选这个？**

> 命名管道（named FIFO，`mkfifo`）。每个客户端两条：`FIFO_C2S_<pid>`（客户端写、服务器读）和 `FIFO_S2C_<pid>`（服务器写、客户端读）。
>
> 选 FIFO 的原因：操作接口和文件完全相同（`open/read/write`），不需要额外的网络库；天然支持跨进程；用 PID 命名保证唯一性；双向通信只需开两条。代价是只能本机通信，不能跨网络，但这个项目的场景是单机多进程，够用。

---

**Q14. 连接建立的完整流程是什么？**

> ```
> 客户端                              服务器
>   kill(server_pid, SIGRTMIN)   →   sigwaitinfo 收到，得到 client_pid
>                                    mkfifo FIFO_C2S_<pid>
>                                    mkfifo FIFO_S2C_<pid>
>   sigwait(SIGRTMIN+1)          ←   kill(client_pid, SIGRTMIN+1)
>   open(FIFO_C2S, O_WRONLY)     →   open(FIFO_C2S, O_RDONLY)
>   open(FIFO_S2C, O_RDONLY)     ←   open(FIFO_S2C, O_WRONLY)
>   write("alice\n")             →   read username → 查 roles.txt
>   read role/version/doc        ←   write("write\n" + version + len + content)
> ```
>
> 两次 `open` 会阻塞直到对端也打开，这是 FIFO 的特性，自然实现了同步。

---

**Q15. 为什么用实时信号（SIGRTMIN）而不是普通信号？**

> 两个原因：
>
> 1. **不丢失**：普通信号（SIGUSR1 等）在 pending 状态时如果再来一个相同信号，第二个会被丢弃。实时信号是排队的（queued），每个都能收到，多个客户端同时连接不会丢失连接请求
> 2. **携带发送方信息**：`sigwaitinfo` 返回的 `siginfo_t` 结构里有 `si_pid` 字段，服务器直接知道是哪个进程在请求连接，不需要额外的握手来传 PID

---

**Q16. FIFO 是全双工的吗？你的设计里怎么处理的？**

> FIFO 是半双工的——数据只能单向流动（一端写、另一端读）。
>
> 解决方案：为每个客户端开两条 FIFO。`FIFO_C2S_<pid>` 专门给客户端→服务器方向，`FIFO_S2C_<pid>` 专门给服务器→客户端方向。两条管道组合成逻辑上的全双工信道，和 TCP socket 的思路一样。

---

**Q17. 客户端异常退出（没发 DISCONNECT），服务器怎么知道？**

> FIFO 的写端关闭后，读端的 `read` 会返回 0（EOF）。per-client 线程在 `while (cli->connect)` 循环里调用 `read(cli->c2s, ...)`，如果客户端进程崩溃或被 kill，它的 FIFO 写端关闭，服务器端 `read` 返回 0，`bytes = 0`，不进入处理分支，循环继续——但下一次 `read` 还是返回 0，会陷入忙等。
>
> 这是代码里的一个缺陷：没有对 `bytes == 0` 单独处理退出循环。正确做法应该是 `if (bytes <= 0) break`，然后走断开连接的清理逻辑。

---

## 版本控制

**Q18. 版本号什么时候 +1？**

> 在 `collect_and_process_commands` 的最后，所有命令执行完毕、日志记录好之后：
>
> ```c
> markdown_increment_version(doc);  // doc->version++
> version_count++;
> broadcast_version();
> ```
>
> 只有当这一轮有命令需要处理时才 +1——如果 timer 醒来发现所有队列都是空的，直接 `return`，不产生新版本，不广播，避免空广播。

---

**Q19. 客户端本地也维护了一份文档，它怎么跟服务器保持同步？**

> 客户端连接时，服务器发来完整文档内容（`length + content`），客户端用 `markdown_init` + `markdown_insert` 建立本地副本，同时记录 `local_version`。
>
> 之后 listener 线程持续读服务器广播。收到 `EDIT user cmd SUCCESS` 时，如果 `broadcast_version == doc->version`，就在本地执行同一条命令（`handle_command_local`）。收到 `END` 时，调用 `markdown_increment_version(doc)` 本地版本 +1。
>
> 这是增量同步，不是每次拉全量——只有成功的命令才应用到本地，失败的命令（`Reject ...`）忽略。

---

**Q20. `OUTDATED_VERSION` 错误在你的代码里实际上会在什么情况下触发？**

> 在服务器的 `handle_command` 里，调用 `markdown_insert(doc, doc->version, ...)` 时，传入的 version 永远等于 `doc->version`，所以服务器端**永远不会触发** `OUTDATED_VERSION`。
>
> 真正会触发的场景是客户端本地的 `handle_command_local`，传入的是 `local_version`。如果 listener 线程还没来得及处理某条广播更新 `local_version`，而又来了一条新广播要应用，`local_version` 就对不上 `doc->version`，返回 -2。这是防止客户端本地文档状态错乱的保护。

---

**Q21. 客户端刚连接时，怎么初始化本地文档状态？**

> 握手后服务器按顺序发三样东西：版本号、文档字节长度、文档内容。
>
> 客户端读完后：
> ```c
> doc = markdown_init();               // 创建空文档
> markdown_insert(doc, 0, 0, buf);     // 把收到的内容整体插入
> doc->version = local_version;        // 对齐版本号
> ```
>
> 用 version=0 插入是因为此时文档刚初始化，版本是 0，这次插入不算一个正式的编辑版本，只是初始化快照。之后的增量广播会从服务器当前版本开始接着同步。

---

## 系统设计与权衡

**Q22. 你的冲突解决策略和 OT（操作变换）有什么区别？各自适合什么场景？**

> 我的策略是**服务端权威序列化**：所有命令按时间戳排序后串行执行，执行结果是确定的，但不一定符合用户意图。比如 Alice 和 Bob 都想在位置 5 插入，结果是一个人的内容夹在另一个人的前面，没有人被拒绝，但位置"跑偏"了。
>
> OT（操作变换）在冲突时会**变换操作本身**：如果 Alice 在位置 5 插入，Bob 也在位置 5 插入，OT 会把 Bob 的操作变换成"在位置 5+Alice 插入长度"处插入，让两个操作最终效果都符合用户意图。
>
> 适用场景：我的方案实现简单，适合低并发、或冲突率低的场景；OT 适合 Google Docs 这类高并发真实协作场景，但实现复杂，尤其是多操作组合时的正确性很难保证。

---

**Q23. 时间戳排序有什么潜在问题？**

> 主要两个问题：
>
> 1. **时钟精度**：`CLOCK_REALTIME` 的精度依赖操作系统，在同一台机器上通常可以区分纳秒级别，但不能保证完全准确。两个几乎同时到达的命令时间戳可能相差极小甚至相等
> 2. **时钟跳变**：`CLOCK_REALTIME` 会受 NTP 校时影响，可能向前或向后跳。如果时钟向后跳，后来的命令时间戳反而比早来的小，排序就乱了。更好的选择是 `CLOCK_MONOTONIC`，单调递增，不受 NTP 影响，但这个项目是单机所以影响不大

---

**Q24. 如果两个命令时间戳完全相同，会怎样？**

> `compare_timestamp` 返回 0，`qsort` 对相等元素的排序是**不稳定的**——标准 C 的 `qsort` 不保证相等元素的相对顺序。也就是说，时间戳相同的两条命令，每次执行顺序可能不同，结果不确定。
>
> 解决方法是用稳定排序（如归并排序），或者在时间戳相同时加一个次级比较键（比如客户端 ID），保证排序结果唯一确定。这个项目里没处理这个边界情况。

---

**Q25. 如果 timer 间隔设得很小（比如 1ms），系统会有什么问题？设得很大呢？**

> **间隔太小**：timer 线程频繁唤醒、频繁加锁 drain 队列、频繁广播。大部分时候队列是空的，做了很多无用功，CPU 占用高，上下文切换开销大。极端情况下 timer 线程本身成为瓶颈。
>
> **间隔太大**：用户输入命令后要等很久才能看到文档更新，延迟高，实时协作体验差。如果间隔是 1 秒，理论上用户感知延迟可以达到接近 2 秒（最坏情况：刚错过一个批次）。
>
> 实践中需要根据用户数量和命令频率调优，通常几十毫秒是合理的折中。

---

**Q26. 最多支持多少个客户端同时连接？这个限制怎么来的？能怎么改？**

> 最多 32 个，来自 `server.c` 顶部的：
> ```c
> #define MAX_CLIENTS 32
> Client clients[MAX_CLIENTS];  // 静态数组
> ```
>
> 这是静态分配的固定大小数组，栈上预分配好了 32 个 Client 槽位。`make_client_thread` 里遍历找空槽，满了就找不到，新客户端无法连接。
>
> 改法：把 `clients` 改成动态数组，用 `realloc` 扩容；或者用链表管理 client 列表。同时要处理好 timer 线程遍历 client 列表时的并发安全。

---

**Q27. 服务器重启后文档数据还在吗？**

> 正常运行时不在。文档完全在内存里，服务器进程退出数据就丢了。
>
> 唯一的持久化发生在 terminal thread 处理 `QUIT` 命令时：
> ```c
> FILE *fp = fopen("doc.md", "w");
> fwrite(flat, 1, strlen(flat), fp);
> ```
> 把当前文档 flatten 后写入 `doc.md`。但这个文件只是 plain text，重启后服务器不会自动读取它恢复状态，需要手动实现加载逻辑。如果服务器崩溃（没走 `QUIT`），数据直接丢失。

---

## 内存与安全

**Q28. 客户端断开连接，服务器端哪些内存需要释放？**

> 在 `client_thread` 的清理逻辑里：
>
> ```c
> close(cli->s2c);          // 关闭 FIFO 文件描述符
> close(cli->c2s);
> unlink(fifo_c2s);         // 删除 FIFO 文件
> unlink(fifo_s2c);
>
> for (size_t i = 0; i < cli->queue_len; ++i) {
>     free(cli->queue[i].comm);  // 释放每条命令的字符串（strdup 来的）
> }
> free(cli->queue);         // 释放命令队列数组本身
>
> pthread_detach(pthread_self());  // 线程资源自动回收
> ```
>
> 注意 `Client` 结构体本身是静态数组 `clients[MAX_CLIENTS]` 的一部分，不需要 free，只需要把 `cli->connect = false` 标记为空槽可复用。

---

**Q29. `strdup` 在哪里用到了？为什么需要它？对应的 `free` 在哪里？**

> 两处：
>
> 1. **per-client 线程入队时**：`now_comm->comm = strdup(buffer)`——因为 `buffer` 是栈上的局部变量，下一次 `read` 会覆盖它。`strdup` 分配堆内存把命令字符串拷贝一份，保证命令对象的生命周期超出当次循环。对应的 `free` 在 `collect_and_process_commands` 里释放上一轮 global_queue 时，以及客户端断开时清理队列里剩余命令时
>
> 2. **`handle_command` 里**：`char *copy = strdup(cmd->comm)`——因为 `strtok` 会破坏原字符串（插入 `\0`），用 copy 操作不影响原始命令字符串（log 里还要用它）。对应的 `free(copy)` 在函数末尾

---

**Q30. buffer 扩容时用的是 `realloc`，有没有可能内存泄漏？**

> 有一个经典问题：
>
> ```c
> char *new_buf = realloc(ct->buf, new_cap);
> if (!new_buf) {
>     perror("realloc");
>     return (size_t)-1;  // ← 这里直接返回了
> }
> ct->buf = new_buf;
> ```
>
> `realloc` 失败时返回 NULL，但原来的 `ct->buf` 仍然有效、仍然分配着。如果这里写成 `ct->buf = realloc(ct->buf, new_cap)`，一旦失败 `ct->buf` 就变成 NULL，原来的内存找不到了，这才是真正的泄漏。
>
> 这个代码里的写法是正确的：用 `new_buf` 接返回值，失败时原 `ct->buf` 不动，可以继续使用或者上层自行处理。但函数返回 `-1` 后，上层 `markdown_insert` 直接返回错误，文档状态实际上已经部分改变（新文本已经在内存里，只是没有 chunk 引用它），这是一个不完全回滚的问题。
