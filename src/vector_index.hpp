#pragma once
#include "storage.hpp"

namespace mini {
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
    template <class Threshold, class Emit>
    void visit(uint64_t off, const std::vector<float> &query, int64_t lo, int64_t hi,
               Threshold &threshold, Emit &emit) {
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
                    (!table.delta_root || !store.find(table.delta_root, key))) {
                    // 精确剪枝：distance 超过当前第 K 名就不必构造整行 Value。
                    const char *vector = reinterpret_cast<const char *>(
                        Store::row_vector(table, p));
                    if (l2(vector, query, dim) <= threshold()) {
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
                    visit(child.off, query, lo, hi, threshold, emit);
        }
    }

  public:
    VectorIndex(Store &s, const Table &t) : store(s), table(t), dim(t.columns[t.vector_col].size) {}
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
    template <class Threshold, class Emit>
    void search(const std::vector<float> &query, int64_t lo, int64_t hi, Threshold threshold,
                Emit emit) {
        if (table.vector_root)
            visit(table.vector_root, query, lo, hi, threshold, emit);
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
