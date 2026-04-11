# Atlas 引擎性能审计报告：网络层与序列化模块

> 审计日期: 2026-04-11
> 审计范围: `src/lib/network/`, `src/lib/serialization/`, `src/lib/platform/io_poller*`
> 参考基准: BigWorld Engine 架构模式

---

## 摘要

本报告对 Atlas 引擎的网络层和序列化模块进行了全面的性能审计，聚焦于**流量优化**和**高并发（10K+ 连接）**场景下的瓶颈。Atlas 的整体架构设计合理——事件驱动、消息聚合（Bundle）、UDP 可靠传输——基本沿用了 BigWorld 的经典模式，但在具体实现上存在大量影响吞吐量和延迟的问题。

### 发现汇总

| 严重级别 | 网络层 | 序列化 | 合计 |
|----------|--------|--------|------|
| **致命 (CRITICAL)** | 1 | 2 | 3 |
| **高 (HIGH)** | 7 | 4 | 11 |
| **中 (MEDIUM)** | 12 | 5 | 17 |
| **低 (LOW)** | 10 | 5 | 15 |

---

## 一、致命问题 (CRITICAL)

### C1. Windows 平台使用 select() — 连接数硬上限约 64

**文件:** `src/lib/platform/io_poller.cpp`, `src/lib/platform/io_poller_select.cpp`

Windows 平台没有 IOCP 后端，回退到 `select()`。MSVC 默认 `FD_SETSIZE = 64`，即使手动 `#define` 调大，`select()` 的 O(n) 扫描在 10K 连接时也不可接受。

```cpp
// io_poller.cpp — Windows 回退到 select
auto IOPoller::create() -> std::unique_ptr<IOPoller>
{
#if ATLAS_PLATFORM_LINUX
    return create_epoll_poller();
#else
    return create_select_poller();  // Windows 走这里
#endif
}
```

**BigWorld 对比:** BigWorld 在 Windows 上仅用于开发调试，生产环境使用 Linux + epoll。但即使如此，BigWorld 也支持更高的 `FD_SETSIZE`。Atlas 如果需要 Windows 生产部署，必须实现 IOCP 后端。

**建议:** 实现 `IOCPPoller` 作为 Windows 平台的 IO 多路复用后端。

---

### C2. BinaryWriter 无内存池支持 — 每条消息都 malloc/free

**文件:** `src/lib/serialization/binary_stream.hpp`

`BinaryWriter` 内部使用 `std::vector<std::byte>`，每次构造都走默认分配器。在 100K+ 消息/秒的吞吐下，这意味着每秒 100K+ 次堆分配和释放。

```cpp
class BinaryWriter
{
    // ...
private:
    std::vector<std::byte> buffer_;  // 每次构造都 malloc
};
```

**BigWorld 对比:** BigWorld 使用内存池/slab 分配器管理流缓冲区，固定大小（512B/1KB/4KB）的缓冲区通过 freelist 回收，热路径零 malloc。

**建议:**
- 添加 `BinaryWriter(std::span<std::byte> external_buffer)` 构造函数支持外部缓冲区
- 或使用 `pmr::monotonic_buffer_resource` + `pmr::vector<std::byte>`
- 或实现专用的 `StreamBufferPool`

---

### C3. DataSection 每节点 shared_ptr — 原子引用计数竞争

**文件:** `src/lib/serialization/data_section.hpp`

每个 `DataSection` 节点都是独立的 `std::make_shared` 分配。多线程读取实体定义时，`shared_ptr` 的原子引用计数导致**缓存行弹跳（cache-line bouncing）**。

```cpp
using Ptr = std::shared_ptr<DataSection>;

auto DataSection::add_child(std::string name) -> Ptr
{
    auto c = std::make_shared<DataSection>(std::move(name));  // 堆分配 + 控制块
    children_.push_back(c);  // 原子引用计数 +1
    return c;                // 原子引用计数 +1
}
```

一个典型的实体定义文件含 ~100 个节点 → 100 次堆分配 + 数百次原子操作。

**BigWorld 对比:** BigWorld 使用侵入式引用计数和 arena 分配，同一文件的节点从同一内存块分配，cache 命中率极高。

**建议:** 使用 `IntrusivePtr<DataSection>`（项目已有此设施）+ arena 分配器。

---

## 二、高严重性问题 (HIGH)

### H1. TCP_NODELAY 未启用 — Nagle 算法导致最高 200ms 延迟

**文件:** `src/lib/network/socket.cpp` (第 164-190 行)

`Socket::create_tcp()` 设置了非阻塞和地址重用，但**没有调用 `set_no_delay(true)`**。Nagle 算法会合并小包，引入最多 200ms 延迟，对游戏服务器来说是灾难性的。

```cpp
auto Socket::create_tcp() -> Result<Socket>
{
    // ...
    Socket sock(fd);
    sock.set_non_blocking(true);
    sock.set_reuse_addr(true);
    return sock;
    // 没有 set_no_delay(true)!
}
```

**这是整个代码库中对延迟影响最大的单一问题。**

**BigWorld 对比:** BigWorld 始终对 TCP 连接启用 TCP_NODELAY。

**修复:** 在 `create_tcp()` 和 `accept()` 返回的 socket 上调用 `set_no_delay(true)`。

---

### H2. IOCallback 每次 poll 分发时被复制 — 大量堆分配

**文件:** `src/lib/platform/io_poller_epoll.cpp`, `src/lib/platform/io_poller_select.cpp`

为了防止回调在执行时自我删除导致悬空引用，IO poller 在每次分发时**复制** `std::function`。带捕获的 lambda 通常会触发堆分配。

```cpp
// epoll 后端的分发代码
auto callback = it->second.callback;  // 复制 std::function → 堆分配
callback(fd, io_events);
```

10K 连接 × 每 tick 若干就绪事件 = 每 tick 数千次堆分配。

**建议:** 使用代数计数器（generation counter）+ 延迟删除列表，通过引用调用回调。

---

### H3. Bundle::end_message() 双缓冲复制 — 每条消息两次堆分配

**文件:** `src/lib/network/bundle.cpp` (第 32-51 行)

消息先写入 `payload_writer_`（堆分配 #1），然后复制到 `buffer_`（堆分配 #2）。变长消息还额外创建临时 `BinaryWriter` 计算长度前缀（堆分配 #3）。

```cpp
void Bundle::end_message()
{
    auto payload = payload_writer_.data();
    if (current_style_ == MessageLengthStyle::Variable)
    {
        BinaryWriter len_writer;  // 堆分配 #3: 临时 writer
        len_writer.write_packed_int(static_cast<uint32_t>(payload.size()));
        auto len_data = len_writer.data();
        buffer_.insert(buffer_.end(), len_data.begin(), len_data.end());
    }
    buffer_.insert(buffer_.end(), payload.begin(), payload.end());  // 复制
}
```

**BigWorld 对比:** BigWorld 直接将消息写入 bundle 的线缓冲区，使用"预留 + 回填"（reserve & backpatch）策略处理长度前缀，完全消除双缓冲复制。

**建议:**
- 消息直接写入 `buffer_`，在 `start_message()` 预留长度前缀空间
- 在 `end_message()` 回填实际长度
- 消除 `payload_writer_` 中间层

---

### H4. ReliableUdp::build_packet() 每包两次堆分配

**文件:** `src/lib/network/reliable_udp.cpp` (第 167-197 行)

构建可靠 UDP 包时：创建临时 `BinaryWriter`（堆分配 #1）+ 创建最终 `std::vector<std::byte>`（堆分配 #2）。包头最多 17 字节，完全可以用栈缓冲区。

**建议:** 使用 `std::array<std::byte, 32>` 栈缓冲区构建包头。

---

### H5. epoll kMaxEvents = 256 — 突发流量下需多次系统调用

**文件:** `src/lib/platform/io_poller_epoll.cpp`

在 10K+ 连接和突发流量下，每次 `epoll_wait` 最多返回 256 个事件，必须多次迭代才能处理所有就绪 fd。

**建议:** 增大到 1024-4096。

---

### H6. UDP 接收缓冲区仅 2048 字节

**文件:** `src/lib/network/network_interface.cpp` (第 344 行)

```cpp
std::array<std::byte, 2048> buf{};
```

MTU 为 1472，headroom 仅 576 字节。部分 UDP 栈可能合并报文。

**BigWorld 对比:** BigWorld 使用 8K-64K 接收缓冲区。

**建议:** 增大到至少 8192 字节。

---

### H7. condemned 通道列表 erase(begin()) 是 O(n)

**文件:** `src/lib/network/network_interface.cpp` (第 419-426 行)

在 DDoS 连接/断开洪水下，频繁 `erase(begin())` 导致 O(n) memmove。

**建议:** 使用 `std::deque` 或 swap-and-pop。

---

### H8. BinaryWriter::write\<T\>() 使用 vector::insert — 每次原语写入都检查容量

**文件:** `src/lib/serialization/binary_stream.hpp`

```cpp
template <Trivial T>
void write(T value)
{
    auto le = endian::to_little(value);
    const auto* bytes = reinterpret_cast<const std::byte*>(&le);
    buffer_.insert(buffer_.end(), bytes, bytes + sizeof(T));  // 检查容量 + 可能重分配
}
```

写入一个 4 字节整数要经过：迭代器构造 → 容量检查 → 可能重分配 → 拷贝。

**建议:** 使用游标模式：先 `ensure_capacity(sizeof(T))`，再 `memcpy(buffer_.data() + size_, &le, sizeof(T))`。

---

### H9. BinaryReader::read_string() 总是堆分配

**文件:** `src/lib/serialization/binary_stream.cpp` (第 94-111 行)

每次反序列化字符串都创建 `std::string`。没有零拷贝的 `read_string_view()` 替代方案。

**BigWorld 对比:** BigWorld 的 `BinaryIStream` 返回 `const char*` + 长度对，完全零拷贝。

**建议:** 添加 `read_string_view() -> Result<std::string_view>`。

---

### H10. DataSection::child() 是 O(n) 线性扫描

**文件:** `src/lib/serialization/data_section.cpp` (第 120-130 行)

每次按名称查找子节点都遍历整个 `children_` 向量，且返回 `shared_ptr` 触发原子引用计数操作。

**建议:** 添加 `std::unordered_map<std::string_view, size_t>` 索引实现 O(1) 查找。

---

### H11. XML/JSON 解析器三重拷贝

**文件:** `src/lib/serialization/xml_parser.cpp`, `src/lib/serialization/json_parser.cpp`

数据在内存中存在三份：文件缓冲区 → pugixml/rapidjson DOM → DataSection 树。

**BigWorld 对比:** BigWorld 仅在启动时解析 XML 并缓存 DataSection，可接受。如果 Atlas 在运行时动态重载配置，需要优化。

**建议:** 如仅启动时使用可暂缓；如运行时使用，考虑 SAX 流式解析或保持 DOM 存活直接引用字符串。

---

## 三、中等严重性问题 (MEDIUM)

| # | 文件 | 问题 | 建议 |
|---|------|------|------|
| M1 | `event_dispatcher.cpp` | `process_frequent_tasks()` 每 tick 执行 `std::erase` 压缩，即使无删除 | 增加 `dirty_` 标志 |
| M2 | `event_dispatcher.hpp` | 默认 `max_poll_wait_` 100ms，游戏服务器应 10-20ms | 改为 tick 间隔驱动 |
| M3 | `io_poller_epoll.cpp` | 使用水平触发（LT），高吞吐 UDP socket 上效率低于边缘触发（ET） | 评估 EPOLLET |
| M4 | `channel.cpp` | `reset_inactivity_timer()` 每次收包都取消/重建定时器，涉及堆分配 | 改为存储 `last_activity_time_` 懒检查 |
| M5 | `channel.hpp` | `disconnect_callback_` 为 `std::function` — 每通道一次堆分配 | 改用函数指针+context 或虚方法 |
| M6 | `network_interface.hpp` | channel map 使用 `std::unordered_map<Address, unique_ptr<Channel>>` — 每连接 3 次堆分配 | 使用 flat hash map |
| M7 | `network_interface.hpp` | `rate_trackers_` 无上限增长，DDoS 下内存泄漏 | 使用固定大小 LRU |
| M8 | `tcp_channel.cpp` | `recv_buffer_` 压缩用 `erase(begin, begin+N)` 是 O(n) memmove | 改用环形缓冲区 |
| M9 | `tcp_channel.cpp` | `do_send` 总是先拷入 `write_buffer_` 再 flush，空闲时可直接发送 | socket 就绪时直接 send |
| M10 | `reliable_udp.hpp` | `unacked_` 使用 `std::map<SeqNum, UnackedPacket>` — O(log n) 且 cache 不友好 | 使用 flat sorted vector 或 ring buffer |
| M11 | `reliable_udp.cpp` | `process_ack()` 每次 ACK 分配 `std::vector<SeqNum>` | 改用 `std::array<SeqNum, 33>` |
| M12 | `reliable_udp.cpp` | 重发定时器间隔固定不更新，RTT 变化后仍用旧间隔 | RTT 变化显著时重建定时器 |
| M13 | `reliable_udp.hpp` | 分片重组用 `vector<vector<byte>>` — 每分片组 n+1 次堆分配 | 使用单一连续缓冲区+偏移 |
| M14 | `interface_table.cpp` | 消息分发热路径使用 try/catch（与项目"无异常"原则矛盾） | 去掉 try/catch 或 benchmark |
| M15 | `interface_table.hpp` | Handler 存储为 `shared_ptr<MessageHandler>` — 不必要的原子引用计数 | 改用 `unique_ptr` |
| M16 | `data_section.cpp` | `read_bool` 分配 `std::string` 仅为做 tolower | 使用 `_stricmp`/case-insensitive 比较 |
| M17 | `json_parser.cpp` | 数组索引用 `std::to_string(i)` — 每元素一次堆分配 | 使用栈缓冲区 |

---

## 四、低严重性问题 (LOW)

| # | 文件 | 问题 |
|---|------|------|
| L1 | `address.cpp` | `to_string()` 在日志热路径中每次分配 `std::string` |
| L2 | `address.hpp` | 仅支持 IPv4，无 IPv6 |
| L3 | `network_interface.cpp` | TCP accept 时 INFO 级别日志，高连接率下日志量爆炸 |
| L4 | `udp_channel.cpp` | 无 `sendmmsg` 批量发送支持 |
| L5 | `socket.cpp` | Linux 上未使用 `accept4(SOCK_NONBLOCK)` |
| L6 | `socket.cpp` | `SO_SNDBUF`/`SO_RCVBUF` 未调优（API 存在但未调用） |
| L7 | `rtt_estimator.hpp` | 使用 double 浮点 RTT 计算，精度损失且慢于整数定点 |
| L8 | `bg_task_manager.cpp` | FrequentTask 每 tick 加锁检查，即使无完成任务 |
| L9 | `binary_stream.cpp` | `detach()` 后 writer 丢失容量，重用需重新分配 |
| L10 | `binary_stream.hpp` | 错误路径每次分配字符串，DoS 下成为放大器 |
| L11 | `data_section.cpp` | `read_string` 始终复制，缺少 `string_view` 变体 |
| L12 | `data_section.cpp` | `set_value` 不复用现有 buffer |
| L13 | `xml_parser.cpp` | 所有 name/value 字符串从 pugixml 复制出来 |
| L14 | `json_parser.cpp` | 数值先转 string 存入 DataSection 再转回数值（round-trip） |
| L15 | `json_parser.cpp` | `convert_value` 接受 `const string&` 强制调用方构造 string |

---

## 五、与 BigWorld 引擎关键差异

| 方面 | BigWorld | Atlas 现状 | 差距 |
|------|----------|-----------|------|
| **IO 多路复用** | Linux epoll, 开发用 select | Linux epoll, Windows select（无 IOCP） | Windows 不可用于生产 |
| **TCP 延迟** | 始终 TCP_NODELAY | 未设置 | 最高 200ms 额外延迟 |
| **消息打包** | 直接写入 bundle 线缓冲区，回填长度 | 双缓冲复制（payload_writer → buffer） | 2-3x 内存带宽浪费 |
| **流缓冲区分配** | 内存池/freelist | std::vector 默认分配器 | 热路径大量 malloc/free |
| **可靠 UDP** | 自研协议，内存池 | KCP 风格，std::vector 包缓冲 | 每包 2 次堆分配 |
| **DataSection** | 侵入式引用计数 + arena | std::shared_ptr + 独立堆分配 | 原子竞争 + cache miss |
| **字符串反序列化** | 零拷贝 (const char* + len) | 总是 std::string 堆分配 | 不必要的分配 |
| **socket 缓冲区** | 256K-1MB SO_RCVBUF/SO_SNDBUF | 未调优（OS 默认 8K-64K） | 高吞吐时 buffer 不足 |
| **tick 间隔** | CellApp/BaseApp 10-20ms | 默认 max_poll_wait 100ms | 响应延迟 |

---

## 六、优先修复建议（按影响排序）

### 立即修复（1-2天工作量）

1. **启用 TCP_NODELAY** — 在 `Socket::create_tcp()` 和 `accept()` 中添加 `set_no_delay(true)`。单行修改，延迟立降 200ms。
2. **调低 `max_poll_wait_` 默认值** — 从 100ms 降至 10-20ms。
3. **增大 epoll `kMaxEvents`** — 从 256 增至 1024+。
4. **增大 UDP 接收缓冲区** — 从 2048 增至 8192。
5. **调用 `SO_SNDBUF`/`SO_RCVBUF`** — 服务器 socket 设为 256KB+。

### 短期优化（1-2周工作量）

6. **Bundle 消息直写+回填** — 消除 payload_writer 双缓冲，仿 BigWorld 模式。
7. **BinaryWriter 游标写模式** — 替换 `vector::insert` 为 `memcpy` + 游标。
8. **添加 `BinaryReader::read_string_view()`** — 零拷贝字符串读取。
9. **IOPoller 回调引用调用** — 消除每次分发的 `std::function` 复制。
10. **可靠 UDP 包头栈分配** — 消除 `build_packet` 的临时堆分配。

### 中期重构（1-2月工作量）

11. **实现 IOCP 后端** — Windows 生产部署的前提。
12. **流缓冲区内存池** — 为 BinaryWriter/Bundle 提供 pool allocator。
13. **DataSection 改用 IntrusivePtr + arena** — 消除 shared_ptr 原子开销。
14. **TcpChannel 改用环形缓冲区** — 消除 O(n) recv_buffer 压缩。
15. **通道 inactivity 改为懒检查** — 消除定时器重建开销。

---

## 七、性能估算

基于上述问题，在不做任何修改的情况下，Atlas 网络层的理论瓶颈：

- **Windows:** ~64 并发连接（select FD_SETSIZE 限制）
- **Linux:** ~5K-10K 并发连接（受限于堆分配开销和 epoll batch 大小）
- **消息吞吐:** ~30K-50K msg/sec（受限于 Bundle 双缓冲 + BinaryWriter 分配模式）
- **延迟:** TCP 通道 +40-200ms（Nagle 算法）；UDP 通道正常

完成上述优化后，预期可达到：

- **Linux:** 50K-100K+ 并发连接
- **消息吞吐:** 200K-500K msg/sec
- **延迟:** TCP <1ms（LAN），UDP <5ms（LAN）
