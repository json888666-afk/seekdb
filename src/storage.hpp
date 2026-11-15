#pragma once
#include "sql.hpp"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <list>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

namespace mini {
inline void io_error(const std::string &action) {
    throw std::runtime_error(action + ": " + std::strerror(errno));
}
inline void write_at(int fd, const void *data, size_t n, uint64_t offset) {
    auto p = static_cast<const char *>(data);
    while (n) {
        ssize_t r = pwrite(fd, p, n, static_cast<off_t>(offset));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            io_error("pwrite");
        p += r;
        n -= r;
        offset += r;
    }
}
inline void read_at(int fd, void *data, size_t n, uint64_t offset) {
    auto p = static_cast<char *>(data);
    while (n) {
        ssize_t r = pread(fd, p, n, static_cast<off_t>(offset));
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            io_error("pread");
        if (!r)
            throw std::runtime_error("truncated database record");
        p += r;
        n -= r;
        offset += r;
    }
}
inline uint64_t checksum(const void *data, size_t n) {
    uint64_t h = 14695981039346656037ULL;
    auto p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}
template <class T> inline void put(std::string &s, const T &v) {
    s.append(reinterpret_cast<const char *>(&v), sizeof(v));
}
template <class T> inline T get(const char *&p, const char *end) {
    if (size_t(end - p) < sizeof(T))
        throw std::runtime_error("corrupt binary record");
    T v;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
}
struct Table {
    std::vector<Column> columns;
    int pk = -1, vector_col = -1;
    uint64_t root = 0;
    uint64_t vector_root = 0, delta_root = 0;
    std::vector<std::string> indexes;
    int column(const std::string &name) const {
        for (size_t i = 0; i < columns.size(); ++i)
            if (lower(columns[i].name) == lower(name))
                return int(i);
        throw Error("unknown_column", "unknown column: " + name);
    }
};
using Catalog = std::map<std::string, Table>;
inline std::string encode_catalog(const Catalog &tables) {
    json j = json::object();
    for (const auto &[name, t] : tables) {
        json cs = json::array();
        for (auto &c : t.columns)
            cs.push_back({{"name", c.name}, {"type", c.type}, {"size", c.size}});
        j[name] = {{"columns", cs},
                   {"pk", t.pk},
                   {"vector_col", t.vector_col},
                   {"root", t.root},
                   {"indexes", t.indexes},
                   {"vector_root", t.vector_root},
                   {"delta_root", t.delta_root}};
    }
    return j.dump();
}
inline Catalog decode_catalog(const std::string &text) {
    Catalog out;
    auto j = json::parse(text);
    for (auto it = j.begin(); it != j.end(); ++it) {
        Table t;
        auto &x = it.value();
        for (auto &c : x.at("columns"))
            t.columns.push_back({c.at("name"), c.at("type"), c.at("size")});
        t.pk = x.at("pk");
        t.vector_col = x.at("vector_col");
        t.root = x.at("root");
        t.indexes = x.at("indexes").get<std::vector<std::string>>();
        t.vector_root = x.value("vector_root", uint64_t(0));
        t.delta_root = x.value("delta_root", uint64_t(0));
        out[it.key()] = std::move(t);
    }
    return out;
}
constexpr size_t PAGE = 4096, FANOUT = 200;
struct Node {
    uint32_t count = 0, leaf = 1;
    int64_t keys[FANOUT + 1]{};
    uint64_t values[FANOUT + 1]{};
};
static_assert(sizeof(Node) <= PAGE);
struct Manifest {
    uint64_t magic = 0x31424456494e494dULL, generation = 0, catalog = 0, length = 0, end = PAGE,
             digest = 0, catalog_digest = 0;
};
class Store {
    struct Entry {
        Node page;
        bool dirty;
        std::list<uint64_t>::iterator lru;
    };
    int fd = -1, manifest_fd = -1;
    std::unordered_map<uint64_t, Entry> cache;
    std::list<uint64_t> lru;
    size_t cache_limit;
    Manifest current;
    void save_raw(uint64_t off, const Node &node) {
        std::array<char, PAGE> bytes{};
        std::memcpy(bytes.data(), &node, sizeof(node));
        write_at(fd, bytes.data(), PAGE, off);
    }
    void evict() {
        while (cache.size() >= cache_limit) {
            auto off = lru.back();
            auto it = cache.find(off);
            if (it->second.dirty)
                save_raw(off, it->second.page);
            lru.pop_back();
            cache.erase(it);
        }
    }

  public:
    uint64_t end = PAGE, boundary = PAGE;
    std::string directory;
    explicit Store(std::string dir, size_t cache_mb = 16)
        : cache_limit(std::max<size_t>(1, cache_mb * 1024 * 1024 / PAGE)),
          directory(std::move(dir)) {
        if (mkdir(directory.c_str(), 0755) < 0 && errno != EEXIST)
            io_error("mkdir data");
        fd = open((directory + "/store.bin").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0)
            io_error("open store");
        // A single writer is required by the protocol; refuse accidental concurrent processes.
        struct flock lock{};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        if (fcntl(fd, F_SETLK, &lock) < 0)
            throw std::runtime_error("data directory is already in use");
        manifest_fd = open((directory + "/MANIFEST").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (manifest_fd < 0)
            io_error("open manifest");
        struct stat ds{}, ms{};
        if (fstat(fd, &ds) < 0 || fstat(manifest_fd, &ms) < 0)
            io_error("stat database");
        bool found = false;
        for (int slot = 0; slot < 2; ++slot) {
            if (ms.st_size < off_t(slot * PAGE + sizeof(Manifest)))
                continue;
            Manifest m;
            read_at(manifest_fd, &m, sizeof(m), slot * PAGE);
            uint64_t h = m.digest;
            m.digest = 0;
            if (m.magic != current.magic || h != checksum(&m, sizeof(m)) ||
                m.end > uint64_t(ds.st_size) || m.end < PAGE || m.catalog > m.end ||
                m.length > m.end - m.catalog || m.length > 64 * 1024 * 1024)
                continue;
            if (m.length) {
                std::string s(m.length, '\0');
                read_at(fd, s.data(), s.size(), m.catalog);
                if (checksum(s.data(), s.size()) != m.catalog_digest)
                    continue;
            }
            m.digest = h;
            if (!found || m.generation > current.generation) {
                current = m;
                found = true;
            }
        }
        if (!found) {
            if (ms.st_size != 0 || ds.st_size > off_t(PAGE))
                throw std::runtime_error("no valid commit manifest");
            if (ftruncate(fd, PAGE) < 0 || fsync(fd) < 0)
                io_error("initialize store");
            commit(Catalog{});
        } else {
            end = current.end;
            if (ftruncate(fd, end) < 0)
                io_error("discard uncommitted tail");
        }
        int dfd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd < 0)
            io_error("open data directory");
        int r = fsync(dfd);
        close(dfd);
        if (r < 0)
            io_error("sync data directory");
    }
    ~Store() {
        if (fd >= 0)
            close(fd);
        if (manifest_fd >= 0)
            close(manifest_fd);
    }
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;
    Catalog catalog() const {
        if (!current.length)
            return {};
        std::string s(current.length, '\0');
        read_at(fd, s.data(), s.size(), current.catalog);
        return decode_catalog(s);
    }
    uint64_t committed_end() const {
        return current.end;
    }
    Node load(uint64_t off) {
        auto it = cache.find(off);
        if (it != cache.end()) {
            lru.splice(lru.begin(), lru, it->second.lru);
            return it->second.page;
        }
        Node node;
        read_at(fd, &node, sizeof(node), off);
        if (node.count > FANOUT || node.leaf > 1)
            throw std::runtime_error("corrupt B+ tree node");
        evict();
        lru.push_front(off);
        cache.emplace(off, Entry{node, false, lru.begin()});
        return node;
    }
    void save(uint64_t off, const Node &node) {
        auto it = cache.find(off);
        if (it != cache.end()) {
            it->second.page = node;
            it->second.dirty = true;
            lru.splice(lru.begin(), lru, it->second.lru);
        } else {
            evict();
            lru.push_front(off);
            cache.emplace(off, Entry{node, true, lru.begin()});
        }
    }
    uint64_t alloc_node(const Node &n) {
        uint64_t off = end;
        end += PAGE;
        save(off, n);
        return off;
    }
    uint64_t append(const std::string &data) {
        uint64_t off = end;
        write_at(fd, data.data(), data.size(), off);
        end += data.size();
        return off;
    }
    void rollback(uint64_t to) {
        for (auto it = cache.begin(); it != cache.end();)
            if (it->first >= to) {
                lru.erase(it->second.lru);
                it = cache.erase(it);
            } else
                ++it;
        end = to;
        if (ftruncate(fd, end) < 0)
            io_error("truncate rollback");
    }
    void commit(const Catalog &tables) {
        for (auto &[off, e] : cache)
            if (e.dirty) {
                save_raw(off, e.page);
                e.dirty = false;
            }
        std::string text = encode_catalog(tables);
        uint64_t cat = append(text);
        if (fsync(fd) < 0)
            io_error("fsync data");
        Manifest m;
        m.generation = current.generation + 1;
        m.catalog = cat;
        m.length = text.size();
        m.end = end;
        m.catalog_digest = checksum(text.data(), text.size());
        m.digest = checksum(&m, sizeof(m));
        write_at(manifest_fd, &m, sizeof(m), (m.generation % 2) * PAGE);
        if (fsync(manifest_fd) < 0)
            io_error("fsync manifest");
        current = m;
    }
    static std::string encode_row(const Table &t, const std::vector<Value> &row) {
        std::string body;
        for (size_t i = 0; i < row.size(); ++i) {
            if (t.columns[i].type == "bigint")
                put(body, std::get<int64_t>(row[i]));
            else if (t.columns[i].type == "varchar") {
                auto &s = std::get<std::string>(row[i]);
                put(body, uint32_t(s.size()));
                body += s;
            } else {
                auto &v = std::get<std::vector<float>>(row[i]);
                body.append(reinterpret_cast<const char *>(v.data()), v.size() * sizeof(float));
            }
        }
        require(body.size() <= UINT32_MAX, "value_out_of_range", "row too large");
        std::string data;
        put(data, uint32_t(body.size()));
        data += body;
        return data;
    }
    uint64_t write_row(const Table &t, const std::vector<Value> &row) {
        return append(encode_row(t, row));
    }
    std::vector<Value> read_row(const Table &t, uint64_t off) const {
        uint32_t len;
        read_at(fd, &len, sizeof(len), off);
        if (off + 4 + len > end)
            throw std::runtime_error("corrupt row length");
        std::string data(len, '\0');
        read_at(fd, data.data(), len, off + 4);
        return decode_row(t, data.data(), data.data() + len);
    }
    static std::vector<Value> decode_row(const Table &t, const char *p, const char *e) {
        std::vector<Value> row;
        row.reserve(t.columns.size());
        for (auto &col : t.columns) {
            if (col.type == "bigint")
                row.emplace_back(get<int64_t>(p, e));
            else if (col.type == "varchar") {
                auto n = get<uint32_t>(p, e);
                if (size_t(e - p) < n)
                    throw std::runtime_error("corrupt string");
                row.emplace_back(std::string(p, n));
                p += n;
            } else {
                size_t bytes = size_t(col.size) * sizeof(float);
                if (size_t(e - p) < bytes)
                    throw std::runtime_error("corrupt vector");
                std::vector<float> v(col.size);
                std::memcpy(v.data(), p, bytes);
                p += bytes;
                row.emplace_back(std::move(v));
            }
        }
        return row;
    }
    std::string read_blob(uint64_t off) const {
        uint32_t len;
        read_at(fd, &len, 4, off);
        if (off + 4 + len > end)
            throw std::runtime_error("invalid block length");
        std::string out(len, '\0');
        read_at(fd, out.data(), len, off + 4);
        return out;
    }
    uint64_t find(uint64_t root, int64_t key) {
        while (root) {
            Node n = load(root);
            auto begin = n.keys;
            auto at = std::lower_bound(begin, begin + n.count, key);
            size_t i = at - begin;
            if (n.leaf)
                return i < n.count && n.keys[i] == key ? n.values[i] : 0;
            if (i == n.count || n.keys[i] != key) {
                if (i)
                    --i;
            }
            root = n.values[i];
        }
        return 0;
    }
    struct Split {
        uint64_t left, right;
        int64_t minimum, right_min;
    };
    Split upsert_rec(uint64_t off, int64_t key, uint64_t value) {
        Node n = load(off);
        uint64_t dest = off < boundary ? 0 : off;
        size_t i = std::lower_bound(n.keys, n.keys + n.count, key) - n.keys;
        if (n.leaf) {
            if (i < n.count && n.keys[i] == key)
                n.values[i] = value;
            else {
                for (size_t j = n.count; j > i; --j) {
                    n.keys[j] = n.keys[j - 1];
                    n.values[j] = n.values[j - 1];
                }
                n.keys[i] = key;
                n.values[i] = value;
                ++n.count;
            }
        } else {
            if (i == n.count || n.keys[i] != key) {
                if (i)
                    --i;
            }
            Split s = upsert_rec(n.values[i], key, value);
            n.values[i] = s.left;
            n.keys[i] = s.minimum;
            if (s.right) {
                ++i;
                for (size_t j = n.count; j > i; --j) {
                    n.keys[j] = n.keys[j - 1];
                    n.values[j] = n.values[j - 1];
                }
                n.keys[i] = s.right_min;
                n.values[i] = s.right;
                ++n.count;
            }
        }
        uint64_t right = 0;
        int64_t right_min = 0;
        if (n.count > FANOUT) {
            Node r;
            r.leaf = n.leaf;
            size_t mid = n.count / 2;
            r.count = n.count - mid;
            for (size_t j = 0; j < r.count; ++j) {
                r.keys[j] = n.keys[mid + j];
                r.values[j] = n.values[mid + j];
            }
            n.count = mid;
            right_min = r.keys[0];
            right = alloc_node(r);
        }
        if (dest)
            save(dest, n);
        else
            dest = alloc_node(n);
        return {dest, right, n.keys[0], right_min};
    }
    uint64_t upsert(uint64_t root, int64_t key, uint64_t value) {
        if (!root) {
            Node n;
            n.count = 1;
            n.keys[0] = key;
            n.values[0] = value;
            return alloc_node(n);
        }
        auto s = upsert_rec(root, key, value);
        if (!s.right)
            return s.left;
        Node n;
        n.leaf = 0;
        n.count = 2;
        n.keys[0] = s.minimum;
        n.values[0] = s.left;
        n.keys[1] = s.right_min;
        n.values[1] = s.right;
        return alloc_node(n);
    }
    // Callback false stops iteration. Only O(tree height) pages live on the stack.
    template <class F> bool scan(uint64_t root, int64_t lo, int64_t hi, F &&callback) {
        if (!root || lo > hi)
            return true;
        Node n = load(root);
        if (n.leaf) {
            for (size_t i = 0; i < n.count; ++i)
                if (n.keys[i] >= lo && n.keys[i] <= hi && n.values[i])
                    if (!callback(n.keys[i], n.values[i]))
                        return false;
        } else {
            for (size_t i = 0; i < n.count; ++i) {
                if (i + 1 < n.count && n.keys[i + 1] <= lo)
                    continue;
                if (n.keys[i] > hi)
                    break;
                if (!scan(n.values[i], lo, hi, callback))
                    return false;
            }
        }
        return true;
    }
};
} // namespace mini
