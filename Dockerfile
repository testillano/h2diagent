# =============================================================================
# h2diagent multi-stage Dockerfile
# =============================================================================
# C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
# All dependency versions are declared as ARGs here (single source of truth).
#
# Stages:
#   deps      - All third-party libraries compiled and installed
#   build     - Project compilation
#   unit-test - Lightweight image for running unit tests
#   runtime   - Production image with only the binary
#
# Usage:
#   docker build --target deps      -t h2diagent_builder .
#   docker build --target build     -t h2diagent_build .
#   docker build --target unit-test -t h2diagent_ut .
#   docker build --target runtime   -t h2diagent .
# =============================================================================

FROM ubuntu:24.04 AS deps
LABEL maintainer="testillano"
LABEL description="Docker image with all dependencies to build h2diagent"

WORKDIR /code/build

# ---------------------------------------------------------------------------
# Dependency versions (single source of truth)
# ---------------------------------------------------------------------------
ARG make_procs=4
ARG build_type=Release

ARG boost_ver=1.84.0
ARG ert_logger_ver=v1.1.1
ARG nlohmann_json_ver=v3.12.0
ARG pboettch_jsonschemavalidator_ver=2.4.0
ARG jupp0r_prometheuscpp_ver=v1.3.0
ARG civetweb_civetweb_ver=v1.16
ARG ert_metrics_ver=v1.3.0
ARG ert_queuedispatcher_ver=v1.1.0
ARG testillano_nghttp2_ver=v1.3.0
ARG nghttp2_ver=1.64.0
ARG nghttp2_asio_ver=main
ARG ert_http2comm_ver=v2.4.1
ARG ert_diametercodec_ver=v1.0.2
ARG ert_diametercomm_ver=v1.0.5
ARG google_test_ver=v1.11.0

# ---------------------------------------------------------------------------
# System packages
# ---------------------------------------------------------------------------
RUN apt-get update && apt-get install -y \
    wget zip unzip tar bzip2 patch \
    make cmake g++ autoconf automake libtool pkg-config \
    libssl-dev zlib1g-dev libcurl4-openssl-dev \
    libsctp-dev \
    doxygen graphviz \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# ===========================================================================
# BOOST
# ===========================================================================
RUN set -x && \
    boost_tar=boost_$(echo ${boost_ver} | tr '.' '_').tar.gz && \
    wget -O ${boost_tar} https://boostorg.jfrog.io/artifactory/main/release/${boost_ver}/source/${boost_tar} && \
    file ${boost_tar} | grep -q gzip || \
    (rm -f ${boost_tar} && wget -O ${boost_tar} https://sourceforge.net/projects/boost/files/boost/${boost_ver}/${boost_tar}) && \
    tar xvf ${boost_tar} && cd boost*/ && \
    ./bootstrap.sh && ./b2 -j${make_procs} variant=release \
      --with-system --with-thread --with-coroutine --with-context \
      install && \
    cd .. && rm -rf * && \
    set +x

# ===========================================================================
# ERT_LOGGER
# ===========================================================================
RUN set -x && \
    wget https://github.com/testillano/logger/archive/${ert_logger_ver}.tar.gz && \
    tar xvf ${ert_logger_ver}.tar.gz && cd logger-*/ && \
    cmake -DERT_LOGGER_BuildExamples=OFF -DCMAKE_BUILD_TYPE=${build_type} . && \
    make -j${make_procs} && make install && \
    cd .. && rm -rf * && \
    set +x

# ===========================================================================
# NLOHMANN JSON
# ===========================================================================
RUN set -x && \
    wget https://github.com/nlohmann/json/archive/refs/tags/${nlohmann_json_ver}.tar.gz && \
    tar xvf ${nlohmann_json_ver}.tar.gz && cd json-*/ && mkdir build && cd build && \
    cmake -DJSON_BuildTests=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 .. && \
    make -j${make_procs} install && \
    cd ../.. && rm -rf * && \
    set +x

# ===========================================================================
# PBOETTCH JSON-SCHEMA-VALIDATOR
# ===========================================================================
RUN set -x && \
    wget https://github.com/pboettch/json-schema-validator/archive/${pboettch_jsonschemavalidator_ver}.tar.gz && \
    tar xvf ${pboettch_jsonschemavalidator_ver}.tar.gz && cd json-schema-validator*/ && mkdir build && cd build && \
    cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 .. && \
    make -j${make_procs} && make install && \
    cd ../.. && rm -rf * && \
    set +x

# ===========================================================================
# PROMETHEUS-CPP + CIVETWEB
# ===========================================================================
RUN set -x && \
    wget https://github.com/jupp0r/prometheus-cpp/archive/refs/tags/${jupp0r_prometheuscpp_ver}.tar.gz && \
    tar xvf ${jupp0r_prometheuscpp_ver}.tar.gz && cd prometheus-cpp*/3rdparty && \
    wget https://github.com/civetweb/civetweb/archive/refs/tags/${civetweb_civetweb_ver}.tar.gz && \
    tar xvf ${civetweb_civetweb_ver}.tar.gz && mv civetweb-*/* civetweb && cd .. && \
    mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=${build_type} -DENABLE_TESTING=OFF .. && \
    make -j${make_procs} && make install && \
    cd ../.. && rm -rf * && \
    set +x

# ===========================================================================
# ERT_METRICS
# ===========================================================================
RUN set -x && \
    wget https://github.com/testillano/metrics/archive/${ert_metrics_ver}.tar.gz && \
    tar xvf ${ert_metrics_ver}.tar.gz && cd metrics-*/ && \
    cmake -DERT_METRICS_BuildExamples=OFF -DCMAKE_BUILD_TYPE=${build_type} . && \
    make -j${make_procs} && make install && \
    cd .. && rm -rf * && \
    set +x

# ===========================================================================
# PATCHES (from testillano/nghttp2 repo)
# ===========================================================================
RUN set -x && \
    wget https://github.com/testillano/nghttp2/archive/${testillano_nghttp2_ver}.tar.gz && \
    tar xf ${testillano_nghttp2_ver}.tar.gz && \
    mv nghttp2-*/deps/patches /patches && \
    rm -rf nghttp2-* ${testillano_nghttp2_ver}.tar.gz && \
    set +x

# ===========================================================================
# NGHTTP2 (tatsuhiro library)
# ===========================================================================
RUN set -x && \
    wget https://github.com/nghttp2/nghttp2/releases/download/v${nghttp2_ver}/nghttp2-${nghttp2_ver}.tar.bz2 && \
    tar xf nghttp2-${nghttp2_ver}.tar.bz2 && cd nghttp2-${nghttp2_ver}/ && \
    for patch in $(ls /patches/nghttp2/${nghttp2_ver}/*.patch 2>/dev/null); do patch -p1 < ${patch}; done && \
    ./configure --disable-shared --enable-python-bindings=no && make -j${make_procs} install && \
    cd .. && rm -rf * && \
    set +x

# ===========================================================================
# NGHTTP2-ASIO
# ===========================================================================
RUN set -x && \
    wget https://github.com/nghttp2/nghttp2-asio/archive/refs/heads/${nghttp2_asio_ver}.zip && \
    unzip ${nghttp2_asio_ver}.zip && cd nghttp2-asio-${nghttp2_asio_ver} && \
    for patch in $(ls /patches/nghttp2-asio/${nghttp2_asio_ver}/*.patch 2>/dev/null); do patch -p1 < ${patch}; done && \
    autoreconf -i && automake && autoconf && \
    ./configure --enable-shared=false && make -j${make_procs} install && \
    cd .. && rm -rf * && \
    set +x

# ===========================================================================
# ERT_QUEUEDISPATCHER
# ===========================================================================
RUN set -x && \
    wget https://github.com/testillano/queuedispatcher/archive/${ert_queuedispatcher_ver}.tar.gz && \
    tar xvf ${ert_queuedispatcher_ver}.tar.gz && cd queuedispatcher-*/ && \
    cmake -DERT_QUEUEDISPATCHER_BuildExamples=OFF -DCMAKE_BUILD_TYPE=${build_type} . && \
    make -j${make_procs} && make install && \
    cd .. && rm -rf * && \
    set +x

# ===========================================================================
# ERT_HTTP2COMM
# ===========================================================================
RUN set -x && \
    wget https://github.com/testillano/http2comm/archive/${ert_http2comm_ver}.tar.gz && \
    tar xvf ${ert_http2comm_ver}.tar.gz && cd http2comm-*/ && \
    cmake -DCMAKE_BUILD_TYPE=${build_type} . && make -j${make_procs} && make install && \
    cd .. && rm -rf * && \
    set +x

# ===========================================================================
# GOOGLE TEST FRAMEWORK (before diametercodec/diametercomm which build their UTs)
# ===========================================================================
RUN set -x && \
    wget https://github.com/google/googletest/archive/refs/tags/release-$(echo ${google_test_ver} | cut -c2-).tar.gz && \
    tar xvf release-$(echo ${google_test_ver} | cut -c2-).tar.gz && cd googletest-release*/ && \
    cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 . && make -j${make_procs} install && \
    cd .. && rm -rf * && \
    set +x

# ===========================================================================
# ERT_DIAMETERCODEC
# ===========================================================================
RUN set -x && \
    wget https://github.com/testillano/diametercodec/archive/${ert_diametercodec_ver}.tar.gz && \
    tar xvf ${ert_diametercodec_ver}.tar.gz && cd diametercodec-*/ && \
    cmake -DERT_DIAMETERCODEC_BuildExamples=OFF -DERT_DIAMETERCODEC_BuildTests=OFF -DCMAKE_BUILD_TYPE=${build_type} . && \
    make -j${make_procs} && make install && \
    cd .. && rm -rf * && \
    set +x

# ===========================================================================
# ERT_DIAMETERCOMM
# ===========================================================================
RUN set -x && \
    wget https://github.com/testillano/diametercomm/archive/${ert_diametercomm_ver}.tar.gz && \
    tar xvf ${ert_diametercomm_ver}.tar.gz && cd diametercomm-*/ && \
    cmake -DERT_DIAMETERCOMM_BuildTests=OFF -DCMAKE_BUILD_TYPE=${build_type} . && make -j${make_procs} && make install && \
    cd .. && rm -rf * && \
    set +x

# =============================================================================
# Stage: build
# =============================================================================
FROM deps AS build

ARG make_procs=4
ARG build_type=Release

COPY . /code
WORKDIR /code

RUN cmake -DCMAKE_BUILD_TYPE=${build_type} . && make -j${make_procs}

# =============================================================================
# Stage: unit-test
# =============================================================================
# Stage: runtime
# =============================================================================
FROM ubuntu:24.04 AS runtime

ARG build_type=Release

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libsctp1 curl \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

COPY --from=build /code/build/${build_type}/bin/h2diagent /opt/h2diagent

EXPOSE 3868 8080 8085
# EXPOSE 8074  # admin port (future)

ENTRYPOINT ["/opt/h2diagent"]
CMD []
