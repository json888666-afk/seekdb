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
            mini::ReqId err_id;
            try {
                auto req = mini::parse_request(line);
                err_id = req.id;
                if (req.op == "shutdown") {
                    db.shutdown();
                    std::cout << "{\"id\":";
                    req.id.write_to(std::cout);
                    std::cout << ",\"ok\":true}\n" << std::flush;
                    return 0;
                }
                auto mode = req.has_search_mode ? req.search_mode : std::string("exact");
                auto result = db.execute(req.sql, mode, req.id);
                if (result) {
                    std::string inner = result->dump();
                    std::cout << "{\"id\":";
                    req.id.write_to(std::cout);
                    std::cout << ",\"ok\":true";
                    if (inner.size() > 2) {
                        std::cout << ',';
                        std::cout << inner.substr(1, inner.size() - 2);
                    }
                    std::cout << "}\n" << std::flush;
                }
            } catch (const mini::Error &e) {
                std::cout << "{\"id\":";
                err_id.write_to(std::cout);
                std::cout << ",\"ok\":false,\"error\":{\"code\":";
                mini::write_json_string(std::cout, e.code);
                std::cout << ",\"message\":";
                mini::write_json_string(std::cout, e.what());
                std::cout << "}}\n" << std::flush;
            } catch (const std::exception &e) {
                std::cout << "{\"id\":";
                err_id.write_to(std::cout);
                std::cout << ",\"ok\":false,\"error\":{\"code\":\"invalid_request\",\"message\":";
                mini::write_json_string(std::cout, e.what());
                std::cout << "}}\n" << std::flush;
            }
        }
        db.shutdown();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
}
