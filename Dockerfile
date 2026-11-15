FROM gcc:13.3.0-bookworm AS build
WORKDIR /build
COPY third_party/ third_party/
COPY src/ src/
RUN g++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -ffp-contract=off \
    -static -o /minivectordb src/main.cpp

FROM scratch
COPY --from=build /minivectordb /usr/local/bin/minivectordb
VOLUME ["/data"]
ENTRYPOINT ["/usr/local/bin/minivectordb"]
