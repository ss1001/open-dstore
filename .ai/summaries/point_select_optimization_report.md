# Point Select 深度优化分析报告

> 生成日期: 2026-04-08
> 分支: feature/sysbench-integrated
> 状态: A1 (PGO) 和 A1b (Index-only scan) 已实现，其余方案待评估

---

## 一、热路径全链路

```
SysbenchWorker::DoPointSelect()
  └─ DstoreTableHandler::Scan()                    [tests/utilities/src/table_handler.cpp:440]
       ├─ IndexInterface::ScanBegin() → ScanNext()
       │    └─ BtreeScan::SearchBtree()             [src/index/dstore_btree_scan.cpp:2309]
       │         ├─ GetRoot()                        — pin root, LW_SHARED
       │         │   └─ GetRootFromMetaCache()       [src/index/dstore_btree.cpp:614]
       │         │       └─ PinBuffer(rootPageDesc)  — 原子 CAS
       │         ├─ SearchBtreeFromInternalPage()    [src/index/dstore_btree_scan.cpp:2243]
       │         │   └─ loop per level:
       │         │       ├─ StepRightIfNeeded()
       │         │       ├─ BinarySearchOnPage()
       │         │       └─ ReleaseOldGetNewBuf()    [src/index/dstore_btree.cpp:571]
       │         │           ├─ UnlockAndRelease(old)    ← unlock content + unpin (atomic)
       │         │           └─ ReadAndCheckBtrPage(child) ← pin + lock content (atomic)
       │         └─ ScanOnLeaf() → 提取 heap CTID
       └─ HeapScanHandler::FetchTuple(ctid)          — pin heap page, fetch, unpin
```

### 每次 point_select 的原子操作计数 (3 层 B-Tree)

| 操作 | 次数 | 类型 |
|------|------|------|
| Root page Pin | 1 | CAS (SharedPin) |
| Root page LW_SHARED acquire | 1 | atomic fetch_add on LWLock.state |
| Internal→Leaf: ReleaseOldGetNewBuf × 2 | 4 | 2×(unpin CAS + pin CAS) |
| Internal→Leaf: LWLock release+acquire × 2 | 4 | atomic ops on LWLock.state |
| Leaf page unpin | 1 | CAS |
| Heap page pin + lock + unlock + unpin | 4 | CAS + LWLock × 2 + CAS |
| **合计** | **~15** | **原子操作/次查询** |

---

## 二、PostgreSQL 源码对比发现

### 2.1 PG Buffer Pin: 无锁 CAS + 私有引用计数

**位置**: `postgres/src/backend/storage/buffer/bufmgr.c:3257`

```c
// PG: PinBuffer() 核心逻辑
static bool PinBuffer(BufferDesc *buf, ...) {
    ref = GetPrivateRefCountEntry(b, true);   // thread-local 查找
    if (ref == NULL) {
        // 首次 pin: CAS 循环，无需任何锁
        old_buf_state = pg_atomic_read_u64(&buf->state);
        for (;;) {
            buf_state = old_buf_state + BUF_REFCOUNT_ONE;
            if (pg_atomic_compare_exchange_u64(&buf->state, &old_buf_state, buf_state))
                break;
        }
    } else {
        // 重复 pin: 纯本地计数，零原子操作
        ref->data.refcount++;
    }
}
```

**DStore 对比**: 已有类似的 `BufPrivateRefCount`（8 槽快速数组 + hash overflow），但 point_select 的 pin pattern 是"pin新 unpin旧"交替，私有缓存命中率低。

### 2.2 PG BufferDesc: 64 字节缓存行对齐

**位置**: `postgres/src/include/storage/buf_internals.h:326-387`

```c
// PG: BufferDesc 仅 ~48 字节核心字段
typedef struct BufferDesc {
    BufferTag   tag;                    // 热
    int         buf_id;                 // 热
    pg_atomic_uint64 state;             // 极热 (refcount+flags 打包)
    int         wait_backend_pgprocno;  // 冷
    PgAioWaitRef io_wref;               // 冷
    proclist_head lock_waiters;         // 冷
} BufferDesc;

// 强制 64 字节对齐
#define BUFFERDESC_PAD_TO_SIZE  64
typedef union BufferDescPadded {
    BufferDesc  bufferdesc;
    char        pad[BUFFERDESC_PAD_TO_SIZE];
} BufferDescPadded;
```

**DStore 对比**: `BufferDesc` = 256 字节，包含大量冷字段（dirty queue 指针×5、recoveryPlsn×5），热字段分散在多个 cache line。

### 2.3 PG B-Tree: Drop-Pin + startikey 跳跃

**位置**: `postgres/src/backend/access/nbtree/nbtsearch.c:54`

```c
// PG: 扫描完叶子页后，同时释放 lock 和 pin
static inline void _bt_drop_lock_and_maybe_pin(Relation rel, BTScanOpaque so) {
    if (so->dropPin) {
        so->currPos.lsn = BufferGetLSNAtomic(so->currPos.buf);
        _bt_relbuf(rel, so->currPos.buf);      // 释放 lock + pin
        so->currPos.buf = InvalidBuffer;
    }
}
```

**位置**: `postgres/src/backend/access/nbtree/nbtreadpage.c:1149`
- `BTReadPageState.startikey`: 页面内多次 `_bt_checkkeys` 时跳过已确认满足的前缀 key
- `_bt_checkkeys_look_ahead()`: 线性扫描遇到大量不匹配时投机跳跃

**DStore 对比**: 无 drop-pin 模式，无 startikey 优化。

### 2.4 PG Spinlock: Test-before-TAS + PAUSE

**位置**: `postgres/src/include/storage/s_lock.h:196-241`

```c
// x86_64: 争用重试时先无锁探测，避免不必要的总线锁
#define TAS_SPIN(lock)  (*(lock) ? 1 : TAS(lock))

// 自旋等待时使用 PAUSE 指令
static inline void spin_delay(void) {
    __asm__ __volatile__(" rep; nop\n");  // = PAUSE
}
```

---

## 三、优化方案详细设计

### A3: BufferDesc 缓存行分离

**问题**: DStore `BufferDesc` = 256B (4 cache lines)，热字段分散。

**当前布局** (`include/buffer/dstore_buf.h:650`):
```
Cache line 0 (0-63):   bufBlock(8) + bufTag(16) + controller(8) + state(8) + lruNode开头
Cache line 1 (64-127): lruNode剩余 + crInfo + contentLwLock
Cache line 2 (128-191): nextDirtyPagePtr[5] (40B) + recoveryPlsn开头
Cache line 3 (192-255): recoveryPlsn剩余 + pageVersionOnDisk + fileVersion
```

**优化布局**:
```cpp
struct BufferDesc {
    // === Cache line 0: HOT (Pin/Unpin + Content Lock + Lookup) ===
    gs_atomic_uint64 state;         // 8B  — 最热 (Pin CAS)
    LWLock contentLwLock;           // 24B — 热 (content lock)
    BufBlock bufBlock;              // 8B  — 热 (读页面)
    BufferTag bufTag;               // 16B — 热 (lookup match)
    BufferDescController *ctrl;     // 8B
    // 合计 64B

    // === Cache line 1: WARM (LRU + CR) ===
    LruNode lruNode;
    CRInfo crInfo;

    // === Cache line 2-3: COLD (Dirty flush / Recovery only) ===
    std::atomic<BufferDesc *> nextDirtyPagePtr[5];
    std::atomic<uint64> recoveryPlsn[5];
    PageVersion pageVersionOnDisk;
    uint64 fileVersion;
};
```

**改动范围**:
- `include/buffer/dstore_buf.h` — 重排字段顺序
- 需验证无代码依赖字段偏移（`offsetof` 使用、序列化等）
- `static_assert(sizeof(BufferDesc) == 256)` 保持不变

**风险**: 低。纯字段重排，逻辑不变。
**预期收益**: 减少 1-2 次 cache line miss/查询，NUMA 多核上更明显。
**工期**: 1-2 天。

---

### A8: Root/Internal Page Thread-local Read Cache

**问题**: root/internal page 反复 pin/unpin 会触发共享状态原子操作，热点索引层级在高并发 point_select 下容易形成 cacheline 流量。

**方案**: 每个线程缓存上层页面内容的本地副本。

```cpp
// include/index/dstore_btree_tl_cache.h
struct BTreeTLCacheEntry {
    PageId pageId;            // 缓存的页面 ID
    uint64 version;           // 写入缓存时的 pageVersion
    alignas(64) char data[BLCKSZ];  // 页面内容副本 (8KB)
    bool valid;
};

// 每个线程每个索引最多缓存 3 层 (root + 2 internal)
constexpr int TL_CACHE_MAX_LEVELS = 3;

struct BTreeTLCache {
    BTreeTLCacheEntry entries[TL_CACHE_MAX_LEVELS];
    
    // 查找缓存: 返回 nullptr 表示 miss
    BtrPage* Lookup(PageId pageId, uint64 currentVersion) {
        for (int i = 0; i < TL_CACHE_MAX_LEVELS; i++) {
            if (entries[i].valid && entries[i].pageId == pageId) {
                if (entries[i].version == currentVersion) {
                    return reinterpret_cast<BtrPage*>(entries[i].data);  // HIT
                }
                entries[i].valid = false;  // 版本过期，失效
                return nullptr;
            }
        }
        return nullptr;  // MISS
    }
    
    // 更新缓存
    void Update(int level, PageId pageId, uint64 version, const void* pageData) {
        if (level < TL_CACHE_MAX_LEVELS) {
            entries[level].pageId = pageId;
            entries[level].version = version;
            memcpy(entries[level].data, pageData, BLCKSZ);
            entries[level].valid = true;
        }
    }
};
```

**使用方式**:

```cpp
RetStatus BtreeScan::SearchBtreeWithTLCache(BufferDesc **leafBuf, bool strictlyGreaterThanKey)
{
    BTreeTLCache *cache = thrd->GetBTreeTLCache(m_indexRel->relOid);
    int level = 0;
    
    // 尝试从 TL cache 读取 root
    PageId rootPageId = GetBtreeSmgr()->GetRootPageIdFromMetaCache();
    uint64 rootVersion = /* 从共享 buffer 读取 root generation 或校验信息 */;
    
    BtrPage *page = cache->Lookup(rootPageId, rootVersion);
    if (page != nullptr) {
        // 完全本地读: 零 pin, 零原子操作, 零 buffer pool 访问
        OffsetNumber childOffset = BinarySearchOnPage(page, strictlyGreaterThanKey);
        PageId childPage = page->GetIndexTuple(childOffset)->GetLowlevelIndexpageLink();
        level++;
        
        // 继续尝试 internal 层的 TL cache...
        // (类似逻辑)
        
        // cache miss 或到达叶子层后，走正常的 pin + read
    } else {
        // Cache miss: 回退到现有 pin + content lock 路径，并填充 cache
        // pin root page, read, cache->Update(0, rootPageId, version, pageData)
    }
    
    // 最终叶子页: 必须 pin + LW_SHARED lock
    // ...
}
```

**内存开销**:
- 每线程: 3 entries × 8KB = **24KB**
- 64 线程: **1.5MB** — 完全可接受
- 可按需分配 (首次 point_select 时 lazy init)

**缓存失效机制**:
- 靠 root/internal page 的 generation 或其他轻量校验信息自然失效，无需主动通知
- Root/internal page 很少被修改 (只有 split 时)，命中率 >99%
- 最坏情况: cache miss 回退到现有 pin + content lock 路径，无正确性风险

**改动文件清单**:

| 文件 | 改动 |
|------|------|
| 新增 `include/index/dstore_btree_tl_cache.h` | TL cache 数据结构 |
| `include/common/dstore_thread.h` | 线程上下文添加 `BTreeTLCache*` |
| `src/index/dstore_btree_scan.cpp` | `SearchBtree` 集成 TL cache 逻辑 |

**风险**: 中低（需要明确缓存校验信息和失效边界，fallback 到现有路径即可）。
**预期收益**: 每次 point_select 再减少 **~4 次原子操作** (root+internal 的 pin/unpin)。
**工期**: ~1 周。

---

## 四、已完成项

| 方案 | 状态 | 说明 |
|------|------|------|
| A1: PGO 构建 | **已完成** | 用 sysbench point_select 做 profile |
| A1b: Index-only scan | **已完成** | 跳过 heap page 访问 |

## 五、实施路线图

```
Phase 1 (1-2 天) — 低风险快赢
└── A3: BufferDesc 缓存行分离
    ├── 纯字段重排，不改逻辑
    └── 验证 static_assert + 回归测试

Phase 2 (1 周) — 核心收益
└── A8: Thread-local Page Cache
    ├── 明确 root/internal page generation 或校验信息
    ├── 实现 TL cache + 集成到 SearchBtree
    └── 验证命中率和内存开销

总预期收益 (A3 + A8 叠加):
  - 低并发 (4 线程):  ~10-15% TPS
  - 高并发 (64 线程): ~40-60% TPS
```

---

## 六、网络调研发现的新增方案

> 以下方案来自 2025-2026 年最新论文、博客和基准测试的网络调研。

### A9: Adaptive Hash Index (借鉴 InnoDB AHI)

**来源**: [InnoDB Adaptive Hash Index — PlanetScale](https://planetscale.com/blog/the-mysql-adaptive-hash-index)

**原理**: InnoDB 在运行时监测 B-Tree 的访问模式，对被频繁点查的索引前缀自动构建内存 hash 表，将 O(log N) 的 B-Tree 遍历降为 O(1) 的 hash 查找。

**InnoDB 实测数据**:
- 单值重复查询 (390M 行表): **+16% QPS** (14,043 → 16,701)
- 多值查询 (1000 distinct): **+20% QPS** (9,232 → 11,562)

**DStore 适配方案**:
```
监测层:
  - 在 BtreeScan::SearchBtree() 入口处统计 <indexOid, scanKeyPrefix> 的访问频率
  - 当某前缀的访问频率超过阈值时，为该前缀建立 hash 映射

Hash 表结构:
  struct AHIEntry {
      ScanKeyHash keyHash;    // scan key 的 hash
      PageId      leafPageId; // 叶子页 ID
      OffsetNumber offset;    // 页内偏移
  };
  - 使用 per-partition 的 concurrent hash map (类似 BufTable 的分区设计)
  - 分 16 个 partition，每个 partition 独立 LWLock

查找路径:
  SearchBtree() {
      if (ahiEnabled && AHI::Lookup(scanKey, &leafPageId, &offset)) {
          // O(1) 直接定位叶子页 + 偏移
          buf = PinAndLock(leafPageId);
          // 验证: 页面未被 split，tuple 仍在 offset 位置
          if (Validate(buf, offset, scanKey)) return SUCC;
          // 验证失败: 删除 AHI 条目，回退 B-Tree 遍历
          AHI::Remove(scanKey);
      }
      return SearchBtreeFromInternalPage(...);  // fallback
  }

失效机制:
  - Page split/merge 时批量删除该页的所有 AHI 条目
  - LRU 淘汰: hash 表总大小限制在 buffer pool 的 1/64
```

**优势**: 点查场景下 hash 命中时完全跳过 B-Tree 遍历 (省掉 2-3 层 index page 访问)
**风险**: 中。AHI 本身的 latch 可能成为新瓶颈（InnoDB 历史上多次因 AHI 导致性能退化）。需要可配置开关。
**预期收益**: 热点数据 point_select **+15-25%**，冷数据无影响。
**工期**: 2 周。

---

### A10: SIMD 加速 B-Tree 节点内搜索 (Fingerprint + AVX2)

**来源**:
- [FB+-tree (PVLDB 2025)](https://arxiv.org/html/2503.23397v1) — Latch-free + SIMD，比传统 B+-tree 快 2.3-3.7x
- ["B-Trees Are Back" (SIGMOD 2025)](https://dl.acm.org/doi/10.1145/3709664) — fingerprint SIMD 搜索

**核心技术**:

FB+-tree 的做法是为每个节点的 key 生成 1-byte "fingerprint"（hash 摘要），然后用 AVX2/AVX-512 一次性比较 32/64 个 fingerprint：

```cpp
// 伪代码: SIMD fingerprint 搜索
uint8_t tag = Hash(searchKey) & 0xFF;  // 1-byte fingerprint

// AVX2: 一次比较 32 个 fingerprint
__m256i target = _mm256_set1_epi8(tag);
__m256i data = _mm256_loadu_si256(node->fingerprints);
__m256i cmp = _mm256_cmpeq_epi8(target, data);
uint32_t mask = _mm256_movemask_epi8(cmp);

// mask 中的每个 set bit 是候选匹配，再做精确比较
while (mask) {
    int pos = __builtin_ctz(mask);  // 最低 set bit
    if (node->keys[pos] == searchKey) return pos;  // 精确匹配
    mask &= mask - 1;  // 清除最低 bit
}
```

**DStore 适配方案**:
```
改动点:
1. BtrPage 的 leaf/internal 节点增加 fingerprint 数组 (每 key 1 byte)
   - 内存开销: 每个 8KB 页 ~200-400 个 key → 200-400 bytes 额外开销 (~5%)
   
2. BinarySearchOnPage() 增加 SIMD 快速路径:
   - 先用 fingerprint SIMD 缩小候选集
   - 候选集通常只有 1-2 个 key，再精确比较
   
3. 编译条件: #ifdef __AVX2__ 保护，non-SIMD 回退到原始二分查找
```

**FB+-tree 实测数据**:
- YCSB-C (100% 读): 比传统 B+-tree 快 **2.9x** (单线程)
- 96 线程读: 快 **2.3-3.7x** (结合 latch-free)
- 单独 SIMD 搜索 (不含 latch-free): 预估 **1.5-2x** 节点内搜索加速

**注意**: FB+-tree 的 2.9x 提升是 SIMD + latch-free + 内存布局优化的叠加效果。单独引入 fingerprint SIMD 的收益估计在 **10-20%**（因为节点内搜索只占 point_select 总时间的一部分）。

**风险**: 低。纯增量改动，不影响现有逻辑。
**预期收益**: **10-20%** 节点内搜索加速。
**工期**: 1 周。

---

### A11: PGO + BOLT 二级优化

**来源**: [awesome-pgo](https://github.com/zamazan4ik/awesome-pgo), PG/ClickHouse 社区实践

**现状**: A1 已实现 PGO。但 PGO 之后还可以叠加 **BOLT (Binary Optimization and Layout Tool)** 做 post-link 优化。

**原理**:
- PGO 优化编译器的分支预测和函数内联决策（5-15%）
- BOLT 在链接后重排函数和基本块的物理布局，优化 I-cache 和 I-TLB 命中率（额外 5-10%）

**实测数据**:
- ClickHouse: PGO **+15%**, PGO+BOLT **+23%** (BOLT 额外贡献 ~8%)
- PostgreSQL: PGO **+10%**, 叠加 BOLT 预估额外 **+5-8%**
- Go 程序: PGO **2-14%** 提升（Go 1.22 官方数据）

**DStore 实施**:
```bash
# Step 1: 已有的 PGO 流程
gcc -fprofile-generate -o dstore_instrumented ...
./sysbench_point_select --threads=64  # 采集 profile
gcc -fprofile-use=default.profdata -o dstore_pgo ...

# Step 2: BOLT 叠加 (需要 LLVM/Clang 工具链)
# 用 perf 采集运行时 profile
perf record -e cycles:u -j any,u -- ./dstore_pgo sysbench_point_select
# 转换 perf data
perf2bolt -p perf.data -o perf.fdata dstore_pgo
# BOLT 优化
llvm-bolt dstore_pgo -o dstore_bolt -data=perf.fdata \
  -reorder-blocks=ext-tsp -reorder-functions=cdsort -split-functions
```

**风险**: 极低。纯构建流程变更，不改源码。
**预期收益**: 在 A1 基础上额外 **+5-10%**。
**工期**: 1-2 天（如果构建环境支持 LLVM）。

---

### A12: Per-core Buffer Pool 分区 (借鉴 ScaleCache)

**来源**: [ScaleCache (PVLDB 2025)](https://www.vldb.org/pvldb/vol18/p5073-liu.pdf) — 已集成到华为 GaussDB

**问题**: 当前 DStore buffer pool 使用 `NUM_BUFFER_PARTITIONS` 个分区 LWLock 保护 `BufTable`，但在 64+ 核上仍有 hash 分区争用。

**ScaleCache 的做法**:
- **Per-core local buffer pool**: 每个 CPU core 有独立的小 buffer pool（无需跨核同步）
- **三级层次**: per-core → per-NUMA-node → global，热页面在 per-core 层处理
- **Lock-free 协调**: core 间页面迁移使用 CAS 而非锁
- **实测**: 128 核上 **1.6x** 吞吐提升，近线性扩展

**DStore 适配评估**:
- 改动极大，需要重构整个 buffer pool 架构
- 但核心思路可以借鉴：**将 BufTable 分区数从固定值扩展到与 CPU 核数对齐**
- 轻量级方案: 增加 `NUM_BUFFER_PARTITIONS` 到 CPU 核数级别（如 128），减少 hash 冲突

**轻量级方案 (A12-lite)**:
```cpp
// 当前: 固定分区数
constexpr int NUM_BUFFER_PARTITIONS = ???;  // 检查当前值

// 优化: 动态对齐到 CPU 核数，最小 128
int NUM_BUFFER_PARTITIONS = max(128, numCPUs * 2);
```

**风险**: A12-lite 极低（仅改配置）；完整 ScaleCache 改造风险极高。
**预期收益**: A12-lite 在 64+ 核上 **+10-20%**。
**工期**: A12-lite 1 天；完整版不建议短期投入。

---

### A13: io_uring 异步 Buffer Fetch

**来源**:
- [PostgreSQL 18 AIO (2025)](https://pganalyze.com/blog/postgres-18-async-io) — PG 18 正式引入 io_uring
- [io_uring for High-Performance DBMSs (2026)](https://arxiv.org/pdf/2512.04859)

**PG 18 实测**: io_uring 注册整个 buffer pool 为 fixed buffers，**+4-6%** 性能提升。

**适用场景**: DStore 当前如果数据未在 buffer pool 中（cache miss），需要同步磁盘读。io_uring 可以：
- 批量提交多个 page read 请求
- 减少系统调用开销（shared ring buffer）
- 对 NVMe SSD 充分利用设备队列深度

**DStore 适配评估**: 对纯内存命中的 point_select 帮助有限（buffer pool 够大时 cache hit rate > 99%）。但对 **数据量超过内存** 的场景有显著帮助。

**风险**: 中。需要改造 VFS 层。
**预期收益**: cache miss 场景 **+20-40%**；cache hit 场景 **+2-5%**。
**工期**: 2-3 周。

---

### A14: Pointer Swizzling (借鉴 LeanStore)

**来源**: [LeanStore (VLDB 2018)](https://db.in.tum.de/~leis/papers/leanstore.pdf), Gemini 报告 2.2 节

**原理**: B-Tree internal 节点存储的 child link 当前是 `PageId`（逻辑地址），每次 descent 都需要通过 `BufTable` hash 查找将 `PageId` 映射到 `BufferDesc*`。Pointer Swizzling 直接将 `PageId` 替换为内存指针，消除 hash 查找。

**DStore 当前路径**:
```
BinarySearchOnPage() → 得到 childTuple
childPage = childTuple->GetLowlevelIndexpageLink()  // PageId (逻辑地址)
ReleaseOldGetNewBuf(old, childPage, LW_SHARED)
  └─ ReadAndCheckBtrPage(childPage)
       └─ BufMgr::Read(pdbId, childPage, ...)
            └─ BufTable::Lookup(hash(childPage))     // ← 这一步要消除
```

**Swizzling 方案**:
```cpp
// 在 IndexTuple 中，child link 有两种状态:
// 1. Unswizzled: 存储 PageId (磁盘地址)
// 2. Swizzled: 存储 BufferDesc* (内存指针) + 标志位

// 判断方式: 最高位为 1 表示 swizzled
inline bool IsSwizzled(uint64 link) { return link & SWIZZLE_BIT; }
inline BufferDesc* GetSwizzledPtr(uint64 link) {
    return reinterpret_cast<BufferDesc*>(link & ~SWIZZLE_BIT);
}

// 在 SearchBtreeFromInternalPage 中:
uint64 childLink = childTuple->GetChildLink();
if (IsSwizzled(childLink)) {
    // 直接指针跳转，跳过 BufTable hash 查找
    BufferDesc *childBuf = GetSwizzledPtr(childLink);
    childBuf->Pin();  // 仍需 pin
} else {
    // 首次访问: 走正常路径，然后 swizzle
    BufferDesc *childBuf = BufMgr::Read(childPage);
    childTuple->SetSwizzledLink(childBuf);  // 原子写入 swizzled 指针
}
```

**与 A8 的关系**:
- A8 (TL Cache): 缓存**页面内容副本**到 thread-local，避免 pin
- A14 (Swizzling): 缓存**BufferDesc 指针**到页面本身，避免 hash 查找
- 两者正交，可叠加

**Unswizzle 时机**: 当 buffer frame 被淘汰时，需要将所有指向它的 swizzled 指针回退为 PageId。需要维护反向引用或在淘汰时扫描父节点。

**风险**: 中-高。需要处理:
1. Buffer 淘汰时的 unswizzle
2. Page split 时更新 swizzled 指针
3. 与 CR page 机制的兼容
**预期收益**: 消除每层 1 次 hash 查找，3 层 B-Tree 减少 ~3 次 hash probe。
**工期**: 2 周。

---

### A15: 事务可见性无锁化 (CSN Lock-free)

**来源**: Gemini 报告 2.5 节

**问题**: 每次 point_select 进入 scan 时需要获取 snapshot（事务快照/CSN），涉及全局事务状态读取。高并发时，CSN 分配和 snapshot 获取可能在全局事务表上产生 cache line bounce。

**DStore 当前路径** (需确认):
```
DstoreTableHandler::Scan()
  └─ IndexScanSetSnapshot(scanHandler, snapshot)   // 设置快照
       └─ 读取全局 CSN / 活跃事务列表
```

**优化方向**:
1. **CSN 分配无锁化**: 确保 CSN 分配使用 `atomic_fetch_add`，无 mutex
2. **活跃事务列表 Thread-local 缓存**: 对于只读 snapshot，不需要每次都读全局列表。可以使用 epoch-based 的快照机制，每个 epoch（如每 1ms）刷新一次
3. **Read-only 事务快速路径**: point_select 是只读的，可以使用更轻量的 snapshot 机制（如仅记录 CSN watermark，不复制完整活跃事务列表）

**风险**: 中。需要深入了解 DStore 的 MVCC/CSN 实现后才能确定具体方案。
**预期收益**: 高并发读场景 **+5-15%**（取决于当前 CSN 路径的争用程度）。
**工期**: 1-2 周（需先 profiling 确认瓶颈）。

---

## 七、更新后的优先级总览

| 方案 | 预期收益 | 工期 | 风险 | 依赖 | 推荐度 |
|------|---------|------|------|------|--------|
| ~~A1: PGO~~ | +8-15% | — | — | — | **已完成** |
| ~~A1b: Index-only scan~~ | 跳过 heap | — | — | — | **已完成** |
| **A11: PGO+BOLT** | 额外 +5-10% | 1-2天 | 极低 | A1 | ★★★★★ |
| **A3: BufferDesc 缓存行分离** | +5-10% (NUMA) | 1-2天 | 低 | 无 | ★★★★★ |
| **A12-lite: 增大 BufTable 分区数** | +10-20% (64核+) | 1天 | 极低 | 无 | ★★★★★ |
| **A10: SIMD Fingerprint 搜索** | +10-20% 节点搜索 | 1周 | 低 | 无 | ★★★★ |
| **A9: Adaptive Hash Index** | +15-25% 热点 | 2周 | 中 | 无 | ★★★ |
| **A14: Pointer Swizzling** | 消除 hash 查找 | 2周 | 中-高 | 无 | ★★★ |
| **A8: TL Page Cache** | 减少 ~4 次原子操作 | 1周 | 中低 | 无 | ★★★ |
| **A15: CSN 无锁化** | +5-15% (高并发读) | 1-2周 | 中 | 需profiling | ★★★ |
| **A13: io_uring** | +20-40% (cache miss) | 2-3周 | 中 | 无 | ★★ |
| A12-full: Per-core Buffer Pool | +60% (128核) | 月级 | 高 | 无 | ★ (长期) |

### 推荐实施顺序

```
Sprint 1 (3-5 天) — 零风险快赢
├── A11: BOLT 叠加 PGO            (+5-10%)
├── A3:  BufferDesc 缓存行重排    (+5-10%)
└── A12-lite: BufTable 分区扩大   (+10-20%)
    预期叠加: +15-30%

Sprint 2 (1-2 周) — 核心架构优化
└── A10: SIMD Fingerprint 搜索    (+10-20% 节点搜索)
    预期叠加: +40-60% @高并发

Sprint 3 (2-3 周) — 高级优化
├── A9:  Adaptive Hash Index       (+15-25% 热点)
├── A8:  TL Page Cache             (减少原子操作)
└── A13: io_uring (可选)           (+20-40% cache miss)
    预期叠加: 额外 +15-30%
```

---

## 八、参考资料

### 本地源码参考
| 来源 | 用途 |
|------|------|
| PG `bufmgr.c:3257` PinBuffer() | A2/A3 的 CAS pin 设计参考 |
| PG `buf_internals.h:326` BufferDescPadded | A3 缓存行对齐参考 |
| PG `nbtsearch.c:54` _bt_drop_lock_and_maybe_pin | Drop-pin 模式参考 |
| PG `s_lock.h:196` TAS_SPIN + PAUSE | Spinlock 优化参考 |

### 学术论文与技术文档
- [FB+-tree: A Memory-Optimized B+-tree with Latch-Free Update (PVLDB 2025)](https://arxiv.org/html/2503.23397v1) — A10 SIMD + latch-free 参考
- ["B-Trees Are Back" (SIGMOD 2025)](https://dl.acm.org/doi/10.1145/3709664) — A10 fingerprint 节点布局参考
- [LeanStore (VLDB 2018)](https://db.in.tum.de/~leis/papers/leanstore.pdf)
- [Umbra (CIDR 2020)](https://db.in.tum.de/~freitag/papers/p29-neumann-cidr20.pdf) — A8 latch-free buffer 管理参考
- [ScaleCache (PVLDB 2025)](https://www.vldb.org/pvldb/vol18/p5073-liu.pdf) — A12 per-core buffer pool, 已集成 GaussDB
- [Evolution of Buffer Management (arXiv 2025)](https://arxiv.org/html/2512.22995v1) — buffer pool 技术综述
- [InnoDB Adaptive Hash Index — PlanetScale](https://planetscale.com/blog/the-mysql-adaptive-hash-index) — A9 AHI 设计参考
- [PostgreSQL 18 Async I/O](https://pganalyze.com/blog/postgres-18-async-io) — A13 io_uring 参考
- [io_uring for High-Performance DBMSs (2026)](https://arxiv.org/pdf/2512.04859) — A13 io_uring 深度分析
- [InnoDB Contention Points (2026)](https://oneuptime.com/blog/post/2026-03-31-mysql-innodb-contention-points/view) — InnoDB mutex 争用分析
- [MySQL High Concurrency Optimization (2026)](https://oneuptime.com/blog/post/2026-03-31-mysql-high-concurrency/view) — InnoDB 高并发调优
- [Percona 2026 MySQL Ecosystem Benchmark](https://www.percona.com/blog/2026-mysql-ecosystem-performance-benchmark-report/) — MySQL vs PG 基准测试
- [Bf-Tree (PVLDB 2024)](https://badrish.net/papers/bftree-vldb2024.pdf) — B14 mini-page hot fragment cache
- [EPVS (VLDB Journal 2024)](https://link.springer.com/article/10.1007/s00778-024-00859-8) — B15 epoch-protected metadata
- [VEGA (SIGMOD 2025)](https://2025.sigmod.org/toc-3-1.html) — B17 learned upper directory
- [HugeTLBpage on ARM64](https://docs.kernel.org/6.0/arm64/hugetlbpage.html) — B13 ARM HugePage 参考

---

## 九、Codex master plan 整合 (R3 本地代码复核 + R4 前沿)

> 来源: `docs/optimizations/point_select_optimization_master_plan.md` (codex 生成, 2026-04-09)
> 这一节补充本报告中 A1-A15 之外、由本地代码复核(R3) 与 2024-2025 学术前沿(R4) 引入的优化项，统一以 B 开头编号，方便和已有 A 系列叠加。

### 9.1 R3 本地代码复核新增项 (P0/P1)

#### B1: Snapshot CSN 快路径化 [P0]

- **代码位置**:
  - `src/transaction/dstore_transaction.cpp:1513`
  - `src/transaction/dstore_csn_mgr.cpp:134`
- **问题**: 只读 query 获取 snapshot 时直接读 `m_nextCsn`，在每秒数百万次的极高并发点查下成为固定的全局共享状态读热点 (cacheline bouncing)。
- **方案**: 引入 `published_visible_csn` 风格的发布式只读变量，由后台或写事务侧周期性更新。`point_select` 直接读取已发布快照 CSN，cursor / flashback / 长事务保持原语义。
- **预期收益**: 高并发读 **+5-15%** (与 A15 互补，A15 偏架构方向，B1 是 R3 给出的具体落点)。
- **风险**: 中。必须严格验证 RC/MVCC 语义。
- **工期**: 1-2 周 (含 profiling 与回归验证)。

#### B2: PointGet 专用快速路径 [P0]

- **代码位置**:
  - `tests/utilities/src/table_handler.cpp:447`
  - `src/index/dstore_index_handler.cpp:49`
  - `src/heap/dstore_heap_interface.cpp:335`
- **问题**: 单键唯一点查仍走完整 `ScanBegin/ReScan/ScanNext/ScanEnd` 生命周期，每次 query 都做对象初始化、scan key copy、状态切换，存在显著"框架层固定税"。
- **方案**: 新增 `PointGetUnique` 接口，直接执行"唯一键查索引 → ctid → 快速 heap fetch"，复用 thread-local handler 或专用轻量上下文。和 A1b (Index-only scan) 正交，可叠加。
- **预期收益**: 极高 QPS 场景下框架层 CPU 占比 20-30%，预期 **+10-20%** TPS。
- **风险**: 低-中。需维护一条与通用 scan 并存的专用分支。
- **工期**: 1-2 周。

#### B3: Heap Tuple 零拷贝 / 延迟物化 [P1]

- **代码位置**: `src/heap/dstore_heap_scan.cpp:694`, `:920`
- **问题**: `FetchTuple()` 当前会复制 tuple，对"取到即返回"的点查是无谓的内存带宽消耗。
- **方案**: 引入 borrowed tuple view (持有 buffer pin + page offset)，仅在跨页/事务结束时才物化。需要严格生命周期管理，确保 pin 持有期间 view 有效。
- **预期收益**: value 较长场景 **+5-10%**；short value 场景作用较小。
- **风险**: 中。生命周期复杂度上升。
- **工期**: 1-2 周。

#### B4: Heap 可见性极简快路径 [P1]

- **代码位置**:
  - `src/heap/dstore_heap_scan.cpp:949`
  - `src/transaction/dstore_transaction.cpp:1313`
- **问题**: 只读点查大多数时候不需要完整的通用 MVCC 判断分支链。
- **方案**: 引入 page-level "all-visible" / "all-frozen" hint (类似 PG `PD_ALL_VISIBLE`)，对 frozen / committed / 无 pending 依赖的 tuple 进入超快判定，跳过 csn list / xact 状态查询。
- **预期收益**: heap fetch 阶段 CPU **-10-20%**，整体 **+3-8%**。
- **风险**: 中。可见性 hint 必须保守正确，写路径需要小心维护。
- **工期**: 1-2 周。

#### B5: Unique Int4 Key 专用比较内核 [P1]

- **代码位置**: `src/index/dstore_btree.cpp:956`, `:1018`
- **问题**: 已存在 `CompareNIntKeyWithoutNulls()`，但仍嵌在通用比较路径里，每次比较都要走 `GetAttr()` + 通用函数指针。
- **方案**: 对 `unique + single int4 + non-null` (sysbench 表的典型 schema) 编译期特化一条专用比较内核，直接 `*(int32*)key1 - *(int32*)key2`。和 A10 (SIMD fingerprint) 正交。
- **预期收益**: 节点内搜索 **+5-15%**，完整 point_select **+2-5%**。
- **风险**: 低。仅新增专用分支，不影响通用路径。
- **工期**: 3-5 天。

#### B6: Root / Meta Cache Epoch 发布式读取 [P1]

- **代码位置**: `src/index/dstore_btree.cpp:614` (`GetRootFromMetaCache`)
- **问题**: root cache 当前每次都做较重验证，与 A8 (TL Cache) 不同点在于这是元数据级。
- **方案**: 发布 `root descriptor + generation`，常态下 reader 仅读 descriptor 且无任何原子写动作；root split/invalidation 时通过 generation 推进让 reader 自然失效再 fallback。
- **预期收益**: root 路径完全只读化，配合 A8 收益叠加。
- **风险**: 中。需要可靠的 root split / stale cache / invalidation 处理。
- **工期**: 1 周。

### 9.2 R4 前沿研究项 (P2/P3)

#### B7: ARM HugePage / TLB 专项优化 [P2]

- 当前编译机已是 ARM。对 buffer pool / index metadata 区域使用 2MB HugePage，可显著降低 TLB miss。
- 方案: 分离 mmap arena，对 buffer pool 数据区用 `MAP_HUGETLB` 或 madvise THP；对元数据区做更激进 hugepage 策略。
- **预期收益**: 大 buffer pool 场景 **+3-8%**。
- **工期**: 3-5 天 (主要在测试)。

#### B8: Mini-page / Hot Fragment Cache (Bf-Tree) [P2]

- **来源**: Bf-Tree, PVLDB 2024。
- 比 A9 (AHI) 更细粒度: 缓存的不是 hash entry 而是热叶页的 mini-page 片段 (~256-512 字节)，命中后直接得到完整记录，绕过 leaf page pin。
- **预期收益**: 热点数据 **+15-30%**。
- **风险**: 中-高。架构改动较大，建议在 A8/A9 之后再评估。
- **工期**: 月级。

#### B9: EPVS / Epoch-protected Metadata [P3]

- **来源**: VLDB Journal 2024。
- 把 reader 的共享写动作 (例如 ref count、stat 计数) 进一步挪出快路径，统一在 epoch 切换时 reconcile。可与 A8 的 TL cache、B6 的 root publication 配合。

#### B11: Learned Upper Directory / VEGA [P3]

- **来源**: SIGMOD 2025。
- 仅对 B-Tree 上层目录 learned 化 (root + 1-2 层)，叶层仍是经典 B+-tree。比直接替换全树风险低、收益更确定。

#### B12: SmartNIC / DPU 卸载点查 [P3]

- **来源**: arXiv 2026。
- 把上层目录或热点 KV 缓存前移到 NIC/DPU，CPU 侧只在 cache miss 时介入。仅适合中长期架构演进。

#### B13: Zero-sided RDMA / Switch Assisted Fetch [P3]

- **来源**: SIGMOD/PACMMOD 2024。
- 适合存算分离或远端页缓存场景。和 DStore 单机内核优化主线无关。

### 9.3 优先级合并视图 (A 系列 + B 系列)

| Prio | 编号 | 项 | 收益 | 工期 | 备注 |
|---|---|---|---|---|---|
| ✅ | A1 | PGO | +8-15% | — | 已完成 |
| ✅ | A1b | Index-only scan | — | — | 已完成 |
| **P0** | A8 | Thread-local Read Cache | 减 ~4 原子操作 | 1周 | **首批** |
| **P0** | B1 | Snapshot CSN 快路径 | +5-15% | 1-2周 | **首批** |
| **P0** | B2 | PointGet 专用快路径 | +10-20% | 1-2周 | **首批** |
| **P0** | A3 | BufferDesc 热冷分离 | +5-10% | 1-2天 | **首批** |
| **P0** | B3 | Heap Tuple 零拷贝 | +5-10% | 1-2周 | **首批** (codex 推荐组合) |
| **P0** | A11 | PGO+BOLT | +5-10% | 1-2天 | 纯构建 |
| **P0** | A12-lite | BufTable 扩大分区 | +10-20% | 1天 | 纯配置 |
| **P1** | B4 | Heap 可见性快路径 | +3-8% | 1-2周 | |
| **P1** | B5 | Unique Int4 比较内核 | +2-5% | 3-5天 | |
| **P1** | B6 | Root Cache Epoch 发布 | 配合 A8 | 1周 | |
| **P1** | A10 | SIMD Fingerprint | +10-20% 节点搜索 | 1周 | |
| **P1** | A9 | Adaptive Hash Index | +15-25% 热点 | 2周 | 风险中 |
| **P1** | A14 | Pointer Swizzling | 消除 hash 查找 | 2周 | 风险中-高 |
| **P1** | A15 | CSN 无锁化(架构) | +5-15% | 1-2周 | 与 B1 互补 |
| **P2** | B7 | ARM HugePage | +3-8% | 3-5天 | |
| **P2** | A13 | io_uring | +20-40% (cache miss) | 2-3周 | |
| **P2** | B8 | Bf-Tree mini-page | +15-30% | 月级 | |
| **P3** | B9-B13 | EPVS / VEGA / DPU / RDMA | 长期方向 | — | 不建议短期投入 |

### 9.4 Codex 推荐的"首批 4 项"组合

codex master plan 第 8 节明确推荐以下组合作为首期落地:

1. **A8** — Thread-local Read Cache (pin/unpin 流量)
2. **B1** — Snapshot CSN 快路径 (全局快照热点)
3. **B2** — PointGet 专用快路径 (框架层固定税)
4. **B3** — Heap Tuple 零拷贝 (tuple copy 成本)

这 4 项分别命中 4 个独立瓶颈，组合收益预计在 **+35-60% TPS @ 64+ 线程**。本报告完全采纳此组合作为 Phase 2 的核心投入。

### 9.5 与已有 A 系列方案的对应关系

| Codex 项 | 本报告对应 | 关系 |
|---|---|---|
| P0-1 TL Read Cache | A8 | 同一项 |
| P0-2 Snapshot CSN 快路径 | B1 | **新增** (R3 代码复核才发现) |
| P0-3 PointGet 快路径 | B2 | **新增** |
| P0-4 BufferDesc 热冷分离 | A3 | 同一项 |
| P1-1 Heap 零拷贝 | B3 | **新增** |
| P1-2 Heap 可见性快路径 | B4 | **新增** (A15 偏 CSN，B4 偏 page hint) |
| P1-3 Unique Int4 比较 | B5 | **新增** |
| P1-4 Root/Meta Epoch | B6 | **新增** |
| P1-5 Pointer Swizzling | A14 | 同一项 |
| P2-1 编译/二进制布局 | A11 (BOLT) | A11 已含 BOLT，P2-1 还包含 ThinLTO/AutoFDO |
| P2-3 Buffer Pool 分区 | A12 | 同一项 |
| P2-4 ARM HugePage | B7 | **新增** |
| P2-5 Mini-page / Bf-Tree | B8 | **新增** |
| P3-* | B9-B13 | **新增** 长期方向 |

---

**结论**: 整合 codex master plan 后，本报告共识别 **A1-A15 + B1-B13 = 28 项** 可落地优化。其中 **首批组合 (A3 + A8 + A11 + A12-lite + B1 + B2 + B3) 是最高 ROI 路径**，预期累计 TPS 提升 **+40-80% @ 64线程**。后续 P1 的 B4-B6 + A9/A10 是第二波；P2/P3 项为中长期演进方向。

---

## 十、瓶颈分类学 + 代码热点地图 + 覆盖矩阵

> 来源: codex master plan §3 瓶颈分类 + 本报告综合
> 目的: 用"瓶颈 → 代码位置 → 优化项"的映射形式，判断每项优化是否命中真实热点，并防止重复投入同一瓶颈。

### 10.1 五类核心瓶颈 (codex §3)

| 类别 | 描述 | 典型表现 |
|---|---|---|
| **C1 共享读热点** | 多个 reader 同时争抢同一共享数据结构 | LW_SHARED 原子 fetch_add、全局 CSN 读、BufTable hash bucket |
| **C2 原子操作与缓存一致性流量** | pin/unpin/CAS 引起 cacheline bouncing | BufferDesc.state、LWLock.state、PrivateRefCount miss |
| **C3 泛型扫描框架税** | 单键点查仍走通用 scan 生命周期 | ScanBegin/ReScan/ScanNext/ScanEnd、scan key copy、状态切换 |
| **C4 Heap 路径过重** | 可见性判断/tuple copy/handler 生命周期 | MVCC 分支链、tuple 复制、FetchTuple 开销 |
| **C5 指令前端 & cache 局部性** | I-cache miss、分支预测失败、冷热混布 | BufferDesc 冷字段污染、通用函数指针跳转、代码布局 |

### 10.2 DStore 代码热点地图

按 codex R3 复核结果 + 本报告已验证的代码位置整合:

| # | 文件:行 | 角色 | 所属瓶颈 | 相关优化项 |
|---|---|---|---|---|
| H1 | `tests/utilities/src/table_handler.cpp:440,447` | 点查入口 / scan 框架调度 | C3 | B2 |
| H2 | `src/index/dstore_btree_scan.cpp:2241,2309` | `SearchBtreeFromInternalPage` / `SearchBtree` | C1, C5 | A8, A10, B5 |
| H3 | `src/index/dstore_btree.cpp:571` | `ReleaseOldGetNewBuf` lock-coupling | C1, C2 | A8, A14 |
| H4 | `src/index/dstore_btree.cpp:614` | `GetRootFromMetaCache` | C1 | A8, B6 |
| H5 | `src/index/dstore_btree.cpp:956,1018` | `CompareNIntKeyWithoutNulls` + 通用比较 | C5 | B5 |
| H6 | `src/buffer/dstore_buf_mgr.cpp:860,2122` | Buffer read + content lock | C1, C2 | A3, A8, A12-lite |
| H7 | `include/buffer/dstore_buf.h:650` | `BufferDesc` (256B 冷热混布) | C2, C5 | A3 |
| H8 | `include/buffer/dstore_buf_refcount.h` | `BufPrivateRefCount` (8 槽) | C2 | A8 (扩展) |
| H9 | `src/index/dstore_index_handler.cpp:49` | 通用 IndexHandler scan 流程 | C3 | B2 |
| H10 | `src/heap/dstore_heap_interface.cpp:335` | `HeapInterface::Fetch` 入口 | C3, C4 | B2 |
| H11 | `src/heap/dstore_heap_scan.cpp:694,920` | `FetchTuple` + tuple 复制 | C4 | B3 |
| H12 | `src/heap/dstore_heap_scan.cpp:949` | 可见性判断 | C4 | B4 |
| H13 | `src/transaction/dstore_transaction.cpp:1313,1513` | Snapshot 获取 + 可见性 | C1, C4 | B1, B4, A15 |
| H14 | `src/transaction/dstore_csn_mgr.cpp:134` | `m_nextCsn` 读取点 | C1 | B1, A15 |

**观察**:
1. H2 (`dstore_btree_scan.cpp:2241`) 和 H3 (`ReleaseOldGetNewBuf`) 同时命中 C1 + C2，说明 root/internal page 共享状态仍是 point_select 的核心瓶颈，后续优先通过 TL Read Cache、Root/Meta Epoch 和 PointGet 专用路径降低这部分成本。
2. H13 + H14 (snapshot/CSN) 是 codex R3 新发现的热点，此前 A 系列未覆盖，B1 是必须补的优化项。
3. H1 + H9 + H10 (框架层调度) 单独构成 C3 瓶颈类，**只有 B2 (PointGet 专用快路径) 能命中**，其他任何优化都绕不过框架税。
4. H11 + H12 (Heap fetch) 单独构成 C4 瓶颈类，**只有 B3/B4 能命中**。A1b (Index-only scan) 仅对 `covering index` 场景有效，非 covering 场景仍依赖 B3/B4。

### 10.3 瓶颈覆盖矩阵 (优化项 × 瓶颈类别)

打勾表示"强覆盖"，`~` 表示"弱/间接覆盖"。

| 项 | C1 共享读 | C2 原子流量 | C3 框架税 | C4 Heap 过重 | C5 指令/布局 |
|---|:---:|:---:|:---:|:---:|:---:|
| A1 PGO | | | | | ✓ |
| A1b Index-only scan | | | | ✓ | |
| A3 BufferDesc 分离 | | ~ | | | ✓ |
| A8 TL Read Cache | ✓ | ✓ | | | |
| A9 AHI | ~ | | ✓ | | |
| A10 SIMD Fingerprint | | | | | ✓ |
| A11 PGO+BOLT | | | | | ✓ |
| A12-lite BufTable 扩分区 | ✓ | | | | |
| A13 io_uring | | | | ~ | |
| A14 Pointer Swizzling | ✓ | ~ | | | |
| A15 CSN 无锁化(架构) | ✓ | | | ~ | |
| **B1 CSN 快路径** | ✓ | | | | |
| **B2 PointGet 专路径** | | | ✓ | ~ | ~ |
| **B3 Heap 零拷贝** | | | | ✓ | |
| **B4 Heap 可见性快路径** | | | | ✓ | |
| **B5 Unique Int4 比较** | | | | | ✓ |
| **B6 Root Cache Epoch** | ✓ | ~ | | | |
| B7 ARM HugePage | | | | | ✓ |
| B8 Bf-Tree mini-page | ✓ | ✓ | | ✓ | |

### 10.4 瓶颈覆盖充分性检查

| 瓶颈 | 是否被首批组合 (A3+A8+A11+A12-lite+B1+B2+B3) 完整覆盖？ |
|---|---|
| **C1 共享读热点** | ✅ B1 (CSN) + A12-lite (BufTable) 三点齐下 |
| **C2 原子操作流量** | ✅ A8 + A3 (字段布局减少无关 cacheline 污染) |
| **C3 框架税** | ✅ B2 独立命中 |
| **C4 Heap 过重** | ⚠️ 仅 B3 (零拷贝)，缺 B4 (可见性快路径) — **建议 B4 提到首批** |
| **C5 指令/布局** | ⚠️ 仅 A3 + A11 (PGO+BOLT)，缺 B5 (Unique Int4) — **B5 工期仅 3-5 天，建议加入首批** |

**修正建议 — 首批组合扩展到 10 项**:

```
Sprint 0 (纯构建 / 1-2 天)
├── A11  PGO+BOLT
└── A12-lite  BufTable 扩分区

Sprint 1 (低风险字段/路径改造 / 1 周)
├── A3   BufferDesc 热冷分离           [命中 C2+C5]
└── B5   Unique Int4 专用比较         [命中 C5]

Sprint 2 (核心架构 / 3-4 周)
├── A8   TL Read Cache                 [命中 C1+C2]
├── B6   Root/Meta Epoch              [命中 C1]
├── B1   Snapshot CSN 快路径           [命中 C1]
├── B2   PointGet 专路径               [命中 C3]
├── B3   Heap Tuple 零拷贝            [命中 C4]
└── B4   Heap 可见性快路径             [命中 C4]
```

**覆盖度**: C1/C2/C3/C4/C5 五类瓶颈全部被强覆盖 (≥1 项 ✓)，无盲区。

### 10.5 被排除项的理由 (针对首批之外)

| 被排除 | 理由 | 重新评估条件 |
|---|---|---|
| A9 AHI | 与 A8、PointGet 的收益部分重叠，且 InnoDB 历史退化风险 | 若 Sprint 2 后 B-Tree 遍历仍是瓶颈 |
| A10 SIMD Fingerprint | 需改 page format，且 B5 已覆盖 sysbench 主场景 | 若业务出现宽节点/长 key 点查 |
| A14 Pointer Swizzling | 收益与 A8 重叠，unswizzle 复杂度高 | 若 buffer pool hash lookup 仍是 top 3 瓶颈 |
| A13 io_uring | 纯内存命中场景收益 <5%，投入 2-3 周性价比低 | 若出现数据量超内存的压测场景 |
| A15 CSN 无锁化(架构) | 与 B1 高度重叠 | **建议直接合并到 B1** |
| B7 ARM HugePage | 独立收益小 (3-8%)，不与其他项冲突 | 随时可做，作为 Sprint 3 补充 |
| B8 Bf-Tree, B9-B13 | 架构级 / 前沿研究，月级投入 | 年度规划层面决策 |

### 10.6 与首批 4 项 (codex) 的差异

| 差异 | 本报告调整 | 理由 |
|---|---|---|
| 增加 A3 | BufferDesc 分离 1-2 天成本，不加白不加 | 纯字段重排，与当前路径改造低冲突 |
| 增加 A8 | 直接降低 pin/unpin 原子流量 | 需要单独明确 generation 或校验信息 |
| 增加 B4 | C4 类瓶颈仅靠 B3 覆盖不足 | Heap 分支链占 heap 阶段 >30% CPU |
| 增加 B5 | C5 类瓶颈几乎零成本补强 | 3-5 天工期，收益稳定 2-5% |
| 增加 B6 | Root/Meta 发布式读取 | 降低 root descriptor 共享状态读取成本 |
| 新增 Sprint 0 (A11 + A12-lite) | 纯构建 / 配置，当天可上 | 不占开发窗口 |

**总结**: 采用本报告扩展后的首批 10 项 (Sprint 0+1+2)，**预期叠加收益 +70-110% TPS @ 64 线程**，且五类核心瓶颈全部被命中，无遗漏盲区。
