FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y \
    build-essential cmake ninja-build git curl zip unzip tar pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Install vcpkg
RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg && \
    /opt/vcpkg/bootstrap-vcpkg.sh
ENV VCPKG_ROOT=/opt/vcpkg

WORKDIR /src
COPY vcpkg.json vcpkg-configuration.json CMakeLists.txt CMakePresets.json ./
COPY include/ include/
COPY tests/ tests/

RUN cmake --preset vcpkg -DCMAKE_BUILD_TYPE=Release
RUN cmake --build build

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y libstdc++6 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/test_freelist /usr/local/bin/test_freelist
COPY --from=build /src/build/bench_freelist /usr/local/bin/bench_freelist
ENTRYPOINT ["test_freelist"]
