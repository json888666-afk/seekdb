CXX ?= g++
CXXFLAGS ?= -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -ffp-contract=off

.PHONY: all check public material clean
all: build/minivectordb

build/minivectordb: src/main.cpp src/sql.hpp src/storage.hpp src/engine.hpp src/vector_index.hpp third_party/json.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) src/main.cpp -o $@

check: all
	python3 tests/test_engine.py
	python3 tests/test_parser_views.py

material:
	python3 agent/check_material.py

public: all
	python3 protocol_check/run.py --command '["$(CURDIR)/build/minivectordb","--data-dir","{data_dir}"]' --output protocol-results/public-$$(date +%s)

clean:
	rm -f build/minivectordb
