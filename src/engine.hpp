#pragma once
#include "storage.hpp"
#include "vector_index.hpp"
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <queue>
#include <set>

namespace mini {
struct Expr {
    std::string column, name, vector_text;
    int index = -1;
    bool distance = false;
    bool alias = false;
    std::vector<float> query;
    Value eval(const std::vector<Value> &row) const {
        if (distance)
            return l2(std::get<std::vector<float>>(row[index]), query);
        return row[index];
    }
    void resolve(const Table &t) {
        index = t.column(column);
        if (!distance && !alias)
            name = t.columns[index].name;
        if (distance) {
            require(t.columns[index].type == "vector", "type_error", "L2_DISTANCE requires VECTOR");
            query = Parser::vector_value(vector_text, t.columns[index].size);
        }
    }
};
inline Expr parse_expr(Parser &p) {
    Expr e;
    e.column = p.identifier();
    e.name = e.column;
    if (e.column == "l2_distance" && p.take("(")) {
        e.distance = true;
        e.column = p.identifier();
        p.expect(",");
        e.vector_text = p.vector_text();
        p.expect(")");
        e.name = "L2_DISTANCE(" + e.column + ", '" + e.vector_text + "')";
    }
    return e;
}
struct Condition {
    int column;
    std::string op;
    Value value;
};
struct Filter {
    std::vector<Condition> conditions;
    int64_t lo = INT64_MIN, hi = INT64_MAX;
    bool empty = false;
    bool matches(const std::vector<Value> &row) const {
        for (const auto &c : conditions) {
            int v = compare(row[c.column], c.value);
            bool ok = c.op == "="    ? v == 0
                      : c.op == "<"  ? v < 0
                      : c.op == "<=" ? v <= 0
                      : c.op == ">"  ? v > 0
                      : c.op == ">=" ? v >= 0
                                     : false;
            if (!ok)
                return false;
        }
        return !empty;
    }
};
inline Filter parse_filter(Parser &p, const Table &t) {
    Filter f;
    if (!p.take("where"))
        return f;
    do {
        bool paren = p.take("(");
        int col = t.column(p.identifier());
        std::string op = p.next().text;
        require(op == "=" || op == "<" || op == "<=" || op == ">" || op == ">=", "unsupported_sql",
                "unsupported comparison");
        require(t.columns[col].type != "vector", "unsupported_sql",
                "vector filtering is unsupported");
        Value val = p.literal(t.columns[col]);
        f.conditions.push_back({col, op, val});
        if (paren)
            p.expect(")");
        if (col == t.pk) {
            int64_t key = std::get<int64_t>(val);
            if (op == "=") {
                f.lo = std::max(f.lo, key);
                f.hi = std::min(f.hi, key);
            } else if (op == ">=")
                f.lo = std::max(f.lo, key);
            else if (op == "<=")
                f.hi = std::min(f.hi, key);
            else if (op == ">") {
                if (key == INT64_MAX)
                    f.empty = true;
                else
                    f.lo = std::max(f.lo, key + 1);
            } else if (op == "<") {
                if (key == INT64_MIN)
                    f.empty = true;
                else
                    f.hi = std::min(f.hi, key - 1);
            }
        }
    } while (p.take("and"));
    f.empty = f.empty || f.lo > f.hi;
    return f;
}
struct Order {
    Expr expr;
    bool desc = false;
};
struct Candidate {
    int64_t pk;
    uint64_t offset;
    std::vector<Value> keys;
};
struct Better {
    const std::vector<Order> *orders;
    bool operator()(const Candidate &a, const Candidate &b) const {
        for (size_t i = 0; i < orders->size(); ++i) {
            int c = compare(a.keys[i], b.keys[i]);
            if (c)
                return (*orders)[i].desc ? c > 0 : c < 0;
        }
        return a.pk < b.pk;
    }
};
// Unbounded ORDER BY uses sorted disk runs and bounded fan-in merges.
// LIMIT uses an in-memory O(K) heap instead.
class ExternalSort {
    struct FileClose {
        void operator()(FILE *f) const {
            if (f)
                fclose(f);
        }
    };
    using File = std::unique_ptr<FILE, FileClose>;
    struct Run {
        File file;
        uint64_t count = 0;
    };
    std::vector<Run> runs;
    std::vector<Candidate> buffer;
    size_t used = 0;
    Better better;
    std::string directory;
    File temporary() {
        std::string name = directory + "/.sort-XXXXXX";
        std::vector<char> path(name.begin(), name.end());
        path.push_back(0);
        int fd = mkstemp(path.data());
        if (fd < 0)
            io_error("create sort run");
        unlink(path.data());
        FILE *f = fdopen(fd, "w+b");
        if (!f) {
            close(fd);
            io_error("open sort run");
        }
        return File(f);
    }
    static void write(FILE *f, const Candidate &c) {
        std::string s;
        put(s, c.pk);
        put(s, c.offset);
        put(s, uint32_t(c.keys.size()));
        for (auto &key : c.keys) {
            put(s, uint8_t(key.index()));
            if (auto v = std::get_if<int64_t>(&key))
                put(s, *v);
            else if (auto v = std::get_if<double>(&key))
                put(s, *v);
            else {
                auto &text = std::get<std::string>(key);
                put(s, uint32_t(text.size()));
                s += text;
            }
        }
        uint32_t len = s.size();
        if (fwrite(&len, 4, 1, f) != 1 || fwrite(s.data(), 1, s.size(), f) != s.size())
            io_error("write sort run");
    }
    static bool read(FILE *f, Candidate &c) {
        uint32_t len;
        size_t n = fread(&len, 1, 4, f);
        if (!n && feof(f))
            return false;
        if (n != 4)
            throw std::runtime_error("read sort run");
        std::string s(len, '\0');
        if (fread(s.data(), 1, len, f) != len)
            throw std::runtime_error("read sort run");
        const char *p = s.data();
        const char *e = p + len;
        c.pk = get<int64_t>(p, e);
        c.offset = get<uint64_t>(p, e);
        uint32_t keys = get<uint32_t>(p, e);
        c.keys.clear();
        for (uint32_t i = 0; i < keys; ++i) {
            auto kind = get<uint8_t>(p, e);
            if (kind == 0)
                c.keys.emplace_back(get<int64_t>(p, e));
            else if (kind == 3)
                c.keys.emplace_back(get<double>(p, e));
            else {
                auto size = get<uint32_t>(p, e);
                if (size_t(e - p) < size)
                    throw std::runtime_error("invalid sort run");
                c.keys.emplace_back(std::string(p, size));
                p += size;
            }
        }
        return true;
    }
    template <class F> void merge(std::vector<Run> &input, F emit) {
        struct Head {
            Candidate c;
            size_t run;
        };
        auto worse = [&](const Head &a, const Head &b) { return better(b.c, a.c); };
        std::priority_queue<Head, std::vector<Head>, decltype(worse)> heap(worse);
        for (size_t i = 0; i < input.size(); ++i) {
            rewind(input[i].file.get());
            Candidate c;
            if (read(input[i].file.get(), c))
                heap.push({std::move(c), i});
        }
        while (!heap.empty()) {
            Head h = heap.top();
            heap.pop();
            emit(h.c);
            Candidate c;
            if (read(input[h.run].file.get(), c))
                heap.push({std::move(c), h.run});
        }
    }
    void reduce() {
        if (runs.size() < 32)
            return;
        Run out{temporary(), 0};
        merge(runs, [&](const Candidate &c) {
            write(out.file.get(), c);
            ++out.count;
        });
        runs.clear();
        runs.push_back(std::move(out));
    }
    void flush() {
        if (buffer.empty())
            return;
        std::sort(buffer.begin(), buffer.end(), better);
        Run out{temporary(), uint64_t(buffer.size())};
        for (auto &c : buffer)
            write(out.file.get(), c);
        buffer.clear();
        used = 0;
        runs.push_back(std::move(out));
        reduce();
    }

  public:
    ExternalSort(Better b, std::string dir) : better(b), directory(std::move(dir)) {}
    void add(Candidate c) {
        used += sizeof(Candidate) + c.keys.size() * sizeof(Value);
        for (auto &k : c.keys)
            if (auto s = std::get_if<std::string>(&k))
                used += s->size();
        buffer.push_back(std::move(c));
        if (used >= 8 * 1024 * 1024)
            flush();
    }
    template <class F> void output(F emit) {
        if (runs.empty()) {
            std::sort(buffer.begin(), buffer.end(), better);
            for (auto &c : buffer)
                emit(c);
        } else {
            flush();
            merge(runs, emit);
        }
    }
};
class Engine {
    Store store;
    Catalog tables, committed;
    bool transaction = false;
    Table &table(const std::string &name) {
        auto i = tables.find(name);
        require(i != tables.end(), "unknown_table", "unknown table: " + name);
        return i->second;
    }
    json create(Parser &p) {
        if (p.take("vector")) {
            p.expect("index");
            std::string index = p.identifier();
            p.expect("on");
            Table &t = table(p.identifier());
            p.expect("(");
            int c = t.column(p.identifier());
            p.expect(")");
            p.finish();
            require(c == t.vector_col, "type_error", "index requires vector column");
            for (auto &[name, other] : tables)
                require(std::find(other.indexes.begin(), other.indexes.end(), index) ==
                            other.indexes.end(),
                        "index_exists", "index already exists");
            t.vector_root = VectorIndex(store, t).build();
            t.delta_root = 0;
            t.indexes.push_back(index);
            return json::object();
        }
        p.expect("table");
        std::string name = p.identifier();
        require(!tables.count(name), "table_exists", "table already exists");
        p.expect("(");
        Table t;
        std::string table_pk;
        do {
            if (p.take("primary")) {
                p.expect("key");
                p.expect("(");
                require(table_pk.empty() && t.pk < 0, "invalid_schema", "multiple primary keys");
                table_pk = p.identifier();
                p.expect(")");
            } else {
                Column c;
                c.name = p.identifier(false);
                for (auto &old : t.columns)
                    require(lower(old.name) != lower(c.name), "invalid_schema", "duplicate column");
                c.type = p.identifier();
                require(c.type == "bigint" || c.type == "varchar" || c.type == "vector",
                        "unsupported_sql", "unsupported column type");
                if (c.type != "bigint") {
                    p.expect("(");
                    c.size = p.size_parameter();
                    p.expect(")");
                }
                if (c.type == "vector") {
                    require(t.vector_col < 0, "invalid_schema", "at most one vector column");
                    t.vector_col = t.columns.size();
                }
                if (p.take("primary")) {
                    p.expect("key");
                    require(t.pk < 0 && table_pk.empty() && c.type == "bigint", "invalid_schema",
                            "expected one BIGINT primary key");
                    t.pk = t.columns.size();
                }
                t.columns.push_back(c);
            }
        } while (p.take(","));
        p.expect(")");
        p.finish();
        if (!table_pk.empty())
            t.pk = t.column(table_pk);
        require(t.pk >= 0 && t.columns[t.pk].type == "bigint", "invalid_schema",
                "one BIGINT primary key is required");
        tables[name] = std::move(t);
        return json::object();
    }
    json insert(Parser &p) {
        p.expect("into");
        Table &t = table(p.identifier());
        std::vector<int> columns;
        if (p.take("(")) {
            std::set<int> seen;
            do {
                int c = t.column(p.identifier());
                require(seen.insert(c).second, "syntax_error", "duplicate insert column");
                columns.push_back(c);
            } while (p.take(","));
            p.expect(")");
            require(columns.size() == t.columns.size(), "type_error", "all columns require values");
        } else
            for (size_t i = 0; i < t.columns.size(); ++i)
                columns.push_back(i);
        p.expect("values");
        uint64_t count = 0;
        // The statement's previous root remains immutable; an error discards all newly allocated
        // pages/rows.
        do {
            p.expect("(");
            std::vector<Value> row(t.columns.size());
            for (size_t i = 0; i < columns.size(); ++i) {
                if (i)
                    p.expect(",");
                row[columns[i]] = p.literal(t.columns[columns[i]]);
            }
            p.expect(")");
            int64_t key = std::get<int64_t>(row[t.pk]);
            require(!store.find(t.root, key), "duplicate_key", "duplicate primary key");
            uint64_t off = store.write_row(t, row);
            t.root = store.upsert(t.root, key, off);
            if (!t.indexes.empty())
                t.delta_root = store.upsert(t.delta_root, key, off);
            ++count;
        } while (p.take(","));
        p.finish();
        return {{"affected_rows", count}};
    }
    json update(Parser &p) {
        Table &t = table(p.identifier());
        p.expect("set");
        std::vector<std::pair<int, Value>> assignments;
        std::set<int> seen;
        do {
            int col = t.column(p.identifier());
            require(col != t.pk, "unsupported_sql", "primary key updates are unsupported");
            require(seen.insert(col).second, "syntax_error", "duplicate assignment");
            p.expect("=");
            assignments.emplace_back(col, p.literal(t.columns[col]));
        } while (p.take(","));
        Filter f = parse_filter(p, t);
        p.finish();
        uint64_t count = 0, root = t.root;
        if (!f.empty)
            store.scan(root, f.lo, f.hi, [&](int64_t key, uint64_t off) {
                auto row = store.read_row(t, off);
                if (!f.matches(row))
                    return true;
                for (auto &[col, val] : assignments)
                    row[col] = val;
                uint64_t newoff = store.write_row(t, row);
                t.root = store.upsert(t.root, key, newoff);
                if (!t.indexes.empty())
                    t.delta_root = store.upsert(t.delta_root, key, newoff);
                ++count;
                return true;
            });
        return {{"affected_rows", count}};
    }
    json remove(Parser &p) {
        p.expect("from");
        Table &t = table(p.identifier());
        Filter f = parse_filter(p, t);
        p.finish();
        uint64_t count = 0, root = t.root;
        if (!f.empty)
            store.scan(root, f.lo, f.hi, [&](int64_t key, uint64_t off) {
                auto row = store.read_row(t, off);
                if (f.matches(row)) {
                    t.root = store.upsert(t.root, key, 0);
                    if (!t.indexes.empty())
                        t.delta_root = store.upsert(t.delta_root, key, 1);
                    ++count;
                }
                return true;
            });
        return {{"affected_rows", count}};
    }
    void select(Parser &p, const json &id) {
        std::vector<Expr> projections;
        bool star = p.take("*");
        if (!star) {
            do {
                Expr e = parse_expr(p);
                if (p.take("as")) {
                    e.name = p.identifier(false);
                    e.alias = true;
                }
                projections.push_back(std::move(e));
            } while (p.take(","));
        }
        p.expect("from");
        Table &t = table(p.identifier());
        if (star)
            for (auto &c : t.columns) {
                Expr e;
                e.column = c.name;
                e.name = c.name;
                projections.push_back(e);
            }
        for (auto &e : projections)
            e.resolve(t);
        Filter f = parse_filter(p, t);
        std::vector<Order> orders;
        if (p.take("order")) {
            p.expect("by");
            do {
                Order order;
                order.expr = parse_expr(p);
                bool alias = false;
                if (!order.expr.distance)
                    for (auto &e : projections)
                        if (lower(e.name) == order.expr.column) {
                            order.expr = e;
                            alias = true;
                            break;
                        }
                if (!alias)
                    order.expr.resolve(t);
                require(order.expr.distance || t.columns[order.expr.index].type != "vector",
                        "unsupported_sql", "cannot order by vector");
                if (p.take("desc"))
                    order.desc = true;
                else
                    p.take("asc");
                orders.push_back(std::move(order));
            } while (p.take(","));
        }
        uint64_t limit = UINT64_MAX;
        if (p.take("limit")) {
            int64_t n = p.integer();
            require(n >= 0, "value_out_of_range", "negative LIMIT");
            limit = n;
        }
        p.finish();
        json names = json::array();
        for (auto &e : projections)
            names.push_back(e.name);
        auto start = [&]() {
            std::cout << "{\"id\":" << id.dump() << ",\"ok\":true,\"columns\":" << names.dump()
                      << ",\"rows\":[";
        };
        bool first = true;
        auto emit_row = [&](const std::vector<Value> &row) {
            json result = json::array();
            for (auto &e : projections)
                result.push_back(value_json(e.eval(row)));
            if (!first)
                std::cout << ',';
            first = false;
            std::cout << result.dump(-1, ' ', false, json::error_handler_t::strict);
        };
        if (orders.empty()) {
            start();
            uint64_t count = 0;
            if (limit && !f.empty)
                store.scan(t.root, f.lo, f.hi, [&](int64_t, uint64_t off) {
                    auto row = store.read_row(t, off);
                    if (f.matches(row)) {
                        emit_row(row);
                        ++count;
                    }
                    return count < limit;
                });
        } else {
            Better better{&orders};
            std::priority_queue<Candidate, std::vector<Candidate>, Better> heap(better);
            ExternalSort sorter(better, store.directory);
            auto consider = [&](int64_t key, uint64_t off, const std::vector<Value> &row) {
                if (!f.matches(row))
                    return;
                Candidate c{key, off, {}};
                for (auto &o : orders)
                    c.keys.push_back(o.expr.eval(row));
                if (limit == UINT64_MAX)
                    sorter.add(std::move(c));
                else if (heap.size() < limit)
                    heap.push(std::move(c));
                else if (better(c, heap.top())) {
                    heap.pop();
                    heap.push(std::move(c));
                }
            };
            if (limit && !f.empty) {
                if (!t.indexes.empty() && orders[0].expr.distance && !orders[0].desc &&
                    limit != UINT64_MAX) {
                    auto threshold = [&]() {
                        return heap.size() < limit ? INFINITY
                                                   : std::get<double>(heap.top().keys[0]);
                    };
                    VectorIndex(store, t).search(orders[0].expr.query, f.lo, f.hi, threshold,
                                                 consider);
                } else
                    store.scan(t.root, f.lo, f.hi, [&](int64_t key, uint64_t off) {
                        auto row = store.read_row(t, off);
                        consider(key, off, row);
                        return true;
                    });
            }
            start();
            auto emit = [&](const Candidate &c) { emit_row(store.read_row(t, c.offset)); };
            if (limit == UINT64_MAX)
                sorter.output(emit);
            else {
                std::vector<Candidate> out;
                out.reserve(heap.size());
                while (!heap.empty()) {
                    out.push_back(heap.top());
                    heap.pop();
                }
                for (auto it = out.rbegin(); it != out.rend(); ++it)
                    emit(*it);
            }
        }
        std::cout << "]}\n" << std::flush;
    }

  public:
    explicit Engine(std::string dir, size_t cache_mb = 16)
        : store(std::move(dir), cache_mb), tables(store.catalog()), committed(tables) {}
    void shutdown() {
        if (transaction) {
            tables = committed;
            store.rollback(store.committed_end());
            transaction = false;
        }
    }
    // SELECT serializes its response directly to avoid buffering complete result sets.
    std::optional<json> execute(const std::string &sql, const std::string &mode, const json &id) {
        require(mode == "exact" || mode == "ann", "invalid_mode",
                "search_mode must be exact or ann");
        Parser p(sql);
        if (p.take("select")) {
            select(p, id);
            return std::nullopt;
        }
        if (p.take("begin")) {
            p.take("transaction");
            p.finish();
            require(!transaction, "transaction_error", "transaction already active");
            transaction = true;
            return json::object();
        }
        if (p.take("commit")) {
            p.finish();
            if (transaction) {
                store.commit(tables);
                committed = tables;
                transaction = false;
            }
            return json::object();
        }
        if (p.take("rollback")) {
            p.finish();
            shutdown();
            return json::object();
        }
        Catalog before = tables;
        uint64_t start = store.end;
        store.boundary = start;
        try {
            json result;
            if (p.take("create"))
                result = create(p);
            else if (p.take("insert"))
                result = insert(p);
            else if (p.take("update"))
                result = update(p);
            else if (p.take("delete"))
                result = remove(p);
            else
                throw Error("unsupported_sql", "unsupported SQL statement");
            if (!transaction) {
                store.commit(tables);
                committed = tables;
            }
            return result;
        } catch (const Error &) {
            tables = std::move(before);
            store.rollback(start);
            throw;
        }
    }
};
} // namespace mini
