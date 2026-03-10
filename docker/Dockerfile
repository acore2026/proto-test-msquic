FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG OPENSSL_VERSION=3.5.0
ARG OPENSSL_PREFIX=/opt/openssl-sctp
ARG MSQUIC_REF=main

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    cmake \
    curl \
    git \
    g++ \
    gcc \
    libatomic1 \
    libsctp-dev \
    make \
    ninja-build \
    perl \
    pkg-config \
    tar \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp

RUN curl -fsSLO "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz" \
    && tar -xzf "openssl-${OPENSSL_VERSION}.tar.gz" \
    && cd "openssl-${OPENSSL_VERSION}" \
    && ./Configure linux-x86_64 enable-sctp --prefix="${OPENSSL_PREFIX}" --openssldir="${OPENSSL_PREFIX}/ssl" \
    && make -j"$(nproc)" \
    && make install_sw

ENV PATH="${OPENSSL_PREFIX}/bin:${PATH}"
ENV PKG_CONFIG_PATH="${OPENSSL_PREFIX}/lib64/pkgconfig:${OPENSSL_PREFIX}/lib/pkgconfig"
ENV LD_LIBRARY_PATH="${OPENSSL_PREFIX}/lib64:${OPENSSL_PREFIX}/lib"

RUN git clone --depth 1 --branch "${MSQUIC_REF}" https://github.com/microsoft/msquic.git /src/msquic

WORKDIR /src/msquic

RUN cmake -S . -B build \
    -G Ninja \
    -DQUIC_TLS_LIB=openssl \
    -DQUIC_ENABLE_LOGGING=OFF \
    -DQUIC_USE_EXTERNAL_OPENSSL=ON \
    -DQUIC_OPENSSL_INCLUDE_DIR="${OPENSSL_PREFIX}/include" \
    -DQUIC_OPENSSL_LIB_DIR="${OPENSSL_PREFIX}/lib64" \
    -DQUIC_BUILD_TEST=OFF \
    -DQUIC_BUILD_TOOLS=OFF \
    -DQUIC_BUILD_PERF=OFF \
    && cmake --build build --parallel

WORKDIR /src/app
COPY . /src/app

RUN cmake -S . -B build \
    -G Ninja \
    -DMSQUIC_ROOT=/src/msquic \
    -DOPENSSL_ROOT_DIR="${OPENSSL_PREFIX}" \
    && cmake --build build --parallel

RUN ./scripts/gen-cert.sh certs

FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive
ARG OPENSSL_PREFIX=/opt/openssl-sctp

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    iproute2 \
    libsctp1 \
    procps \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/app/build/msquic-loadtest /usr/local/bin/msquic-loadtest
COPY --from=builder /src/app/certs /opt/msquic-loadtest/certs
COPY --from=builder /src/msquic/build/bin/Release/libmsquic.so.2.6.0 /usr/local/lib/libmsquic.so.2.6.0
COPY --from=builder ${OPENSSL_PREFIX} ${OPENSSL_PREFIX}
COPY docker/entrypoint.sh /usr/local/bin/msquic-loadtest-entrypoint

RUN ln -s libmsquic.so.2.6.0 /usr/local/lib/libmsquic.so.2 \
    && ln -s libmsquic.so.2 /usr/local/lib/libmsquic.so

ENV PATH="${OPENSSL_PREFIX}/bin:${PATH}"
ENV LD_LIBRARY_PATH="/usr/local/lib:${OPENSSL_PREFIX}/lib64:${OPENSSL_PREFIX}/lib"

WORKDIR /opt/msquic-loadtest

ENTRYPOINT ["/usr/local/bin/msquic-loadtest-entrypoint"]
CMD ["help"]
