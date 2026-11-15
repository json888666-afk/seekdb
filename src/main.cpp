#include "engine.hpp"
#include <csignal>
#include <optional>

int main(int argc, char **argv) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    std::string directory = "/data";
    size_t cache_mb = 16;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data-dir" && i + 1 < argc)
            directory = argv[++i];
        else if (a == "--cache-mb" && i + 1 < argc)
            cache_mb = std::stoull(argv[++i]);
        else {
            std::cerr << "usage: minivectordb [--data-dir DIRECTORY] [--cache-mb N]\n";
            return 2;
        }
    }
    try {
        mini::Engine db(directory, cache_mb);
        std::cout << "{\"type\":\"ready\"}\n" << std::flush;
        std::string line;
        while (std::getline(std::cin, line)) {
            mini::json id = nullptr;
            try {
                // nlohmann preserves integer IDs and rejects non-finite JSON tokens.
                auto req = mini::json::parse(line);
                mini::require(req.is_object(), "invalid_request", "expected object");
                if (req.contains("id"))
                    id = req["id"];
                mini::require(req.contains("id") && req.contains("op") && req["op"].is_string(),
                              "invalid_request", "id/op required");
                auto op = req["op"].get<std::string>();
                if (op == "shutdown") {
                    db.shutdown();
                    std::cout << mini::json({{"id", id}, {"ok", true}}).dump() << '\n'
                              << std::flush;
                    return 0;
                }
                mini::require(op == "execute" && req.contains("sql") && req["sql"].is_string(),
                              "invalid_request", "execute requires SQL string");
                mini::require(!req.contains("search_mode") || req["search_mode"].is_string(),
                              "invalid_request", "invalid search mode");
                auto result =
                    db.execute(req["sql"], req.value("search_mode", std::string("exact")), id);
                if (result) {
                    (*result)["id"] = id;
                    (*result)["ok"] = true;
                    std::cout << result->dump() << '\n' << std::flush;
                }
            } catch (const mini::Error &e) {
                std::cout << mini::json({{"id", id},
                                         {"ok", false},
                                         {"error", {{"code", e.code}, {"message", e.what()}}}})
                                 .dump()
                          << '\n'
                          << std::flush;
            } catch (const mini::json::exception &e) {
                std::cout << mini::json(
                                 {{"id", id},
                                  {"ok", false},
                                  {"error", {{"code", "invalid_request"}, {"message", e.what()}}}})
                                 .dump()
                          << '\n'
                          << std::flush;
            }
        }
        db.shutdown();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
}
