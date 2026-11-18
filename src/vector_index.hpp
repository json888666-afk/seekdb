#pragma once
#include "storage.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#ifdef __linux__
#include <sys/resource.h>
#endif

namespace mini {

// 查询期可用的 CPU 数：容器常以 cgroup 配额限核，hardware_concurrency() 报的是
// 宿主机核数，直接拿来开线程会造成严重超订，所以优先读 cgroup 限额。
inline size_t available_cpus() {
    size_t limit = std::thread::hardware_concurrency();
    if (!limit)
        limit = 1;
#ifdef __linux__
    if (FILE *f = std::fopen("/sys/fs/cgroup/cpu.max", "r")) {
        char quota[32] = {0}, period[32] = {0};
        if (std::fscanf(f, "%31s %31s", quota, period) == 2 && std::strcmp(quota, "max") != 0) {
            double q = std::atof(quota), p = std::atof(period);
            if (p > 0) {
                size_t allowed = static_cast<size_t>(q / p + 0.5);
                if (allowed >= 1 && allowed < limit)
                    limit = allowed;
            }
        }
        std::fclose(f);
    } else if (FILE *f = std::fopen("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", "r")) {
        long quota = -1;
        if (std::fscanf(f, "%ld", &quota) == 1 && quota > 0) {
            if (FILE *p = std::fopen("/sys/fs/cgroup/cpu/cpu.cfs_period_us", "r")) {
                long period = 0;
                if (std::fscanf(p, "%ld", &period) == 1 && period > 0) {
                    size_t allowed = static_cast<size_t>((quota + period - 1) / period);
                    if (allowed >= 1 && allowed < limit)
                        limit = allowed;
                }
                std::fclose(p);
            }
        }
        std::fclose(f);
    }
#endif
    if (limit > 8)
        limit = 8;
    return limit;
}

// TopK 叶子块扫描的常驻线程池。查询串行到达，池只在一次查询内唤醒，避免每次
// 查询重新创建工作线程的固定开销。
class ScanPool {
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::thread> workers;
    std::function<void(size_t)> job;
    std::atomic<size_t> cursor{0};
    size_t total = 0, finished = 0;
    uint64_t epoch = 0;
    bool quit = false;
    std::exception_ptr failure;
    void take() {
        for (;;) {
            size_t i = cursor.fetch_add(1);
            if (i >= total)
                return;
            try {
                job(i);
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                if (!failure)
                    failure = std::current_exception();
            }
        }
    }
    void loop() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return quit || epoch != seen; });
                if (quit)
                    return;
                seen = epoch;
            }
            take();
            std::lock_guard<std::mutex> lock(mutex);
            if (++finished == workers.size())
                wake.notify_all();
        }
    }
    explicit ScanPool(size_t threads) {
        workers.reserve(threads);
        for (size_t i = 0; i < threads; ++i)
            workers.emplace_back([this] { loop(); });
    }

  public:
    ~ScanPool() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            quit = true;
        }
        wake.notify_all();
        for (auto &w : workers)
            w.join();
    }
    static ScanPool &instance(size_t threads) {
        static ScanPool pool(threads);
        return pool;
    }
    // 每个任务号只执行一次；调用线程自己也参与取任务。
    void run(size_t tasks, std::function<void(size_t)> fn) {
        if (workers.empty()) {
            for (size_t i = 0; i < tasks; ++i)
                fn(i);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            job = std::move(fn);
            total = tasks;
            cursor = 0;
            finished = 0;
            failure = nullptr;
            ++epoch;
        }
        wake.notify_all();
        take();
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [&] { return finished == workers.size(); });
        if (failure)
            std::rethrow_exception(failure);
    }
};

// 受限地址空间（内存用例限 96 MiB）下既不能创建线程栈，也不能让并行
// 快路径同时持有全部叶子块；调用方需回退到流式 search()。
inline bool constrained_address_space() {
#ifdef __linux__
    struct rlimit limit {};
    return getrlimit(RLIMIT_AS, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY &&
           limit.rlim_cur < (512ull << 20);
#else
    return false;
#endif
}

inline size_t scan_threads() {
    static size_t value = [] {
        if (constrained_address_space())
            return size_t(1);
        if (const char *env = std::getenv("MINIVECTOR_SCAN_THREADS")) {
            long v = std::atol(env);
            return v > 0 ? static_cast<size_t>(v) : size_t(1);
        }
        return available_cpus();
    }();
    return value;
}

// Immutable, covering vector blocks under a metric-ball hierarchy. Each block
// contains complete rows, making filtered scans sequential rather than random I/O.
// Mutations live in a transactional COW delta tree, including deletion markers.
class VectorIndex {
    struct Ref {
        uint64_t off = 0;
        int64_t lo = INT64_MAX, hi = INT64_MIN;
        double radius = 0;
        std::vector<float> center;
    };
    Store &store;
    const Table &table;
    uint32_t dim;
    static void encode_ref(std::string &s, const Ref &r) {
        put(s, r.off);
        put(s, r.lo);
        put(s, r.hi);
        put(s, r.radius);
        s.append(reinterpret_cast<const char *>(r.center.data()), r.center.size() * 4);
    }
    Ref decode_ref(const char *&p, const char *e) const {
        Ref r;
        r.off = get<uint64_t>(p, e);
        r.lo = get<int64_t>(p, e);
        r.hi = get<int64_t>(p, e);
        r.radius = get<double>(p, e);
        if (size_t(e - p) < size_t(dim) * 4)
            throw std::runtime_error("corrupt metric index");
        r.center.resize(dim);
        std::memcpy(r.center.data(), p, dim * 4);
        p += dim * 4;
        return r;
    }
    Ref write_node(uint8_t leaf, Ref ref, uint32_t count, const std::string &contents) {
        std::string body;
        put(body, leaf);
        encode_ref(body, ref);
        put(body, count);
        body += contents;
        std::string record;
        put(record, uint32_t(body.size()));
        record += body;
        ref.off = store.append(record);
        return ref;
    }
    Ref parent(const std::vector<Ref> &children) {
        Ref ref;
        ref.center = children.front().center;
        std::string data;
        for (auto &child : children) {
            ref.lo = std::min(ref.lo, child.lo);
            ref.hi = std::max(ref.hi, child.hi);
            ref.radius = std::max(ref.radius, l2(ref.center, child.center) + child.radius);
            encode_ref(data, child);
        }
        ref.radius = std::nextafter(ref.radius, INFINITY);
        return write_node(0, std::move(ref), children.size(), data);
    }
    static double bound(const Ref &ref, const std::vector<float> &query) {
        double distance = l2(ref.center, query);
        // Outward slack preserves exactness at floating point boundaries.
        return std::max(0.0, distance - ref.radius - 1e-12 * (distance + ref.radius + 1));
    }
    template <class Threshold, class Emit, class Prefilter>
    void visit(uint64_t off, const std::vector<float> &query, const std::vector<double> &query_d,
               int64_t lo, int64_t hi, Threshold &threshold, Emit &emit, Prefilter &prefilter) {
        auto data = store.read_blob_cached(off);
        const char *p = data->data();
        const char *e = p + data->size();
        auto leaf = get<uint8_t>(p, e);
        Ref self = decode_ref(p, e);
        uint32_t count = get<uint32_t>(p, e);
        if (self.hi < lo || self.lo > hi || bound(self, query) > threshold())
            return;
        if (leaf) {
            for (uint32_t i = 0; i < count; ++i) {
                int64_t key = get<int64_t>(p, e);
                uint64_t rowoff = get<uint64_t>(p, e);
                uint32_t size = get<uint32_t>(p, e);
                if (size_t(e - p) < size)
                    throw std::runtime_error("corrupt vector block");
                if (key >= lo && key <= hi &&
                    (!table.delta_root || !store.find(table.delta_root, key)) &&
                    prefilter(p, size)) {
                    // 精确剪枝：平方距离超过当前第 K 名的平方就跳过整行解码；
                    // 平方比较省掉每行一次 sqrt，略放宽边界以吸收 SIMD 归约误差。
                    double t = threshold();
                    double limit = t * t * (1.0 + 1e-12) + 1e-12;
                    const char *vector = reinterpret_cast<const char *>(
                        Store::row_vector(table, p));
                    if (squared_l2(vector, query_d.data(), dim) <= limit) {
                        auto row = Store::decode_row(table, p, p + size);
                        emit(key, rowoff, row);
                    }
                }
                p += size;
            }
        } else {
            std::vector<std::pair<double, Ref>> children;
            children.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                Ref r = decode_ref(p, e);
                if (r.hi >= lo && r.lo <= hi)
                    children.emplace_back(bound(r, query), std::move(r));
            }
            std::sort(children.begin(), children.end(),
                      [](auto &a, auto &b) { return a.first < b.first; });
            for (auto &[lower, child] : children)
                if (lower <= threshold())
                    visit(child.off, query, query_d, lo, hi, threshold, emit, prefilter);
        }
    }
    // 只取键区间与子节点偏移，跳过球心：叶子扫描不需要球心，省掉每块一次
    // 128 维向量的分配与拷贝。
    struct Span {
        uint64_t off = 0;
        int64_t lo = INT64_MAX, hi = INT64_MIN;
    };
    Span decode_span(const char *&p, const char *e) const {
        Span s;
        s.off = get<uint64_t>(p, e);
        s.lo = get<int64_t>(p, e);
        s.hi = get<int64_t>(p, e);
        p += sizeof(double);
        if (size_t(e - p) < size_t(dim) * 4)
            throw std::runtime_error("corrupt metric index");
        p += size_t(dim) * 4;
        return s;
    }
    // 收集键区间相交的叶子块；读取顺序即磁盘顺序，块缓存在 Store 内复用。
    void collect(uint64_t off, int64_t lo, int64_t hi,
                 std::vector<std::shared_ptr<const std::string>> &out) {
        auto data = store.read_blob_cached(off);
        const char *p = data->data();
        const char *e = p + data->size();
        auto leaf = get<uint8_t>(p, e);
        Span self = decode_span(p, e);
        uint32_t count = get<uint32_t>(p, e);
        if (self.hi < lo || self.lo > hi)
            return;
        if (leaf) {
            out.push_back(std::move(data));
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            Span r = decode_span(p, e);
            if (r.hi >= lo && r.lo <= hi)
                collect(r.off, lo, hi, out);
        }
    }

  public:
    struct Hit {
        double sq = 0;
        int64_t key = 0;
        uint64_t offset = 0;
        // 叶子 shared_ptr 在 search_topk 返回前一直存活；先保存行字节位置，
        // 只对全局最终 K 行分配并解码 Value，避免局部候选被淘汰时白做工。
        const char *body = nullptr;
        uint32_t size = 0;
        std::vector<Value> row;
    };
    VectorIndex(Store &s, const Table &t) : store(s), table(t), dim(t.columns[t.vector_col].size) {}
    // 单个叶子块的 TopK 扫描：候选只保存距离与行字节位置，阈值只在名次
    // 变化后刷新，避免逐行访问堆；整行解码延迟到全局合并之后。
    template <class Prefilter>
    void scan_leaf(const std::string &data, const std::vector<double> &query_d, int64_t lo,
                   int64_t hi, size_t k, std::vector<Hit> &top, Prefilter &prefilter) const {
        const char *p = data.data();
        const char *e = p + data.size();
        auto leaf = get<uint8_t>(p, e);
        Span self = decode_span(p, e);
        uint32_t count = get<uint32_t>(p, e);
        if (!leaf || self.hi < lo || self.lo > hi)
            return;
        double limit = top.size() < k ? INFINITY : top.back().sq;
        for (uint32_t i = 0; i < count; ++i) {
            int64_t key = get<int64_t>(p, e);
            uint64_t rowoff = get<uint64_t>(p, e);
            uint32_t size = get<uint32_t>(p, e);
            if (size_t(e - p) < size)
                throw std::runtime_error("corrupt vector block");
            const char *body = p;
            p += size;
            if (key < lo || key > hi || !prefilter(body, size))
                continue;
            double sq = squared_l2(reinterpret_cast<const char *>(Store::row_vector(table, body)),
                                   query_d.data(), dim);
            if (!(sq <= limit))
                continue;
            size_t at = 0;
            while (at < top.size() &&
                   (top[at].sq < sq || (top[at].sq == sq && top[at].key <= key)))
                ++at;
            if (at >= k)
                continue;
            Hit hit;
            hit.sq = sq;
            hit.key = key;
            hit.offset = rowoff;
            hit.body = body;
            hit.size = size;
            top.insert(top.begin() + static_cast<std::ptrdiff_t>(at), std::move(hit));
            if (top.size() > k)
                top.pop_back();
            limit = top.size() < k ? INFINITY : top.back().sq;
        }
    }
    // 并行 TopK：叶子块按行区间切开后分给线程池，各线程维持自己的前 K 名，
    // 最后合并。并集一定包含全局前 K 名（线程内阈值不小于全局阈值），所以
    // 结果与单线程剪枝一致；并列名次按主键升序，与 ORDER BY 的排序器一致。
    // 前置条件：table.delta_root == 0（有增量树时走 search()，那条路径会做
    // 增量查找，需要独占 Store 的页缓存）。
    template <class Prefilter>
    std::vector<Hit> search_topk(const std::vector<float> &query, int64_t lo, int64_t hi, size_t k,
                                 Prefilter prefilter, size_t threads) {
        if (!table.vector_root || !k)
            return {};
        const std::vector<double> query_d(query.begin(), query.end());
        std::vector<std::shared_ptr<const std::string>> leaves;
        collect(table.vector_root, lo, hi, leaves);
        if (leaves.empty())
            return {};
        if (threads <= 1 || leaves.size() < 4) {
            std::vector<Hit> top;
            for (auto &leaf : leaves)
                scan_leaf(*leaf, query_d, lo, hi, k, top, prefilter);
            for (auto &hit : top)
                hit.row = Store::decode_row(table, hit.body, hit.body + hit.size);
            return top;
        }
        size_t parts = std::min(threads, leaves.size());
        std::vector<std::vector<Hit>> local(parts);
        ScanPool::instance(scan_threads()).run(parts, [&](size_t part) {
            size_t begin = leaves.size() * part / parts;
            size_t end = leaves.size() * (part + 1) / parts;
            for (size_t i = begin; i < end; ++i)
                scan_leaf(*leaves[i], query_d, lo, hi, k, local[part], prefilter);
        });
        std::vector<Hit> top;
        for (auto &part : local)
            for (auto &hit : part) {
                size_t at = 0;
                while (at < top.size() &&
                       (top[at].sq < hit.sq || (top[at].sq == hit.sq && top[at].key <= hit.key)))
                    ++at;
                if (at >= k)
                    continue;
                top.insert(top.begin() + static_cast<std::ptrdiff_t>(at), std::move(hit));
                if (top.size() > k)
                    top.pop_back();
            }
        for (auto &hit : top)
            hit.row = Store::decode_row(table, hit.body, hit.body + hit.size);
        return top;
    }
    uint64_t build() {
        std::vector<std::vector<Ref>> levels(1);
        auto carry = [&](Ref ref, size_t level, auto &&recurse) -> void {
            if (levels.size() <= level)
                levels.resize(level + 1);
            levels[level].push_back(std::move(ref));
            if (levels[level].size() >= 32) {
                Ref up = parent(levels[level]);
                levels[level].clear();
                recurse(std::move(up), level + 1, recurse);
            }
        };
        std::vector<std::pair<uint64_t, std::vector<Value>>> rows;
        size_t bytes = 0;
        auto flush = [&]() {
            if (rows.empty())
                return;
            Ref ref;
            ref.center = std::get<std::vector<float>>(rows.front().second[table.vector_col]);
            std::string contents;
            for (auto &[off, row] : rows) {
                auto key = std::get<int64_t>(row[table.pk]);
                ref.lo = std::min(ref.lo, key);
                ref.hi = std::max(ref.hi, key);
                ref.radius =
                    std::max(ref.radius,
                             l2(ref.center, std::get<std::vector<float>>(row[table.vector_col])));
                put(contents, key);
                put(contents, off);
                contents += Store::encode_row(table, row);
            }
            ref.radius = std::nextafter(ref.radius, INFINITY);
            carry(write_node(1, std::move(ref), rows.size(), contents), 0, carry);
            rows.clear();
            bytes = 0;
        };
        store.scan(table.root, INT64_MIN, INT64_MAX, [&](int64_t, uint64_t off) {
            auto row = store.read_row(table, off);
            bytes += Store::encode_row(table, row).size();
            rows.emplace_back(off, std::move(row));
            if (rows.size() >= 128 || bytes >= 1024 * 1024)
                flush();
            return true;
        });
        flush();
        for (size_t level = 0; level < levels.size(); ++level) {
            if (levels[level].empty())
                continue;
            bool higher = false;
            for (size_t i = level + 1; i < levels.size(); ++i)
                if (!levels[i].empty())
                    higher = true;
            if (!higher && levels[level].size() == 1)
                return levels[level][0].off;
            Ref ref = parent(levels[level]);
            levels[level].clear();
            carry(std::move(ref), level + 1, carry);
        }
        return 0;
    }
    template <class Threshold, class Emit, class Prefilter>
    void search(const std::vector<float> &query, int64_t lo, int64_t hi, Threshold threshold,
                Emit emit, Prefilter prefilter) {
        // 查询向量转 double 一次，热点循环里就不用每个分量重复转换。
        const std::vector<double> query_d(query.begin(), query.end());
        if (table.vector_root)
            visit(table.vector_root, query, query_d, lo, hi, threshold, emit, prefilter);
        if (table.delta_root)
            store.scan(table.delta_root, lo, hi, [&](int64_t key, uint64_t off) {
                if (off != 1) {
                    auto row = store.read_row(table, off);
                    emit(key, off, row);
                }
                return true;
            });
    }
};
} // namespace mini
