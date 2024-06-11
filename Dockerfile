FROM alpine:latest
WORKDIR /opt/vsearch5d
COPY . .
RUN apk add --no-cache \
        libstdc++ zlib-dev bzip2-dev \
        autoconf automake make g++ && \
    ./autogen.sh && \
    ./configure CFLAGS="-O3 -fomit-frame-pointer -finline-functions" CXXFLAGS="-O3 -fomit-frame-pointer -finline-functions" && \
    make clean && \
    make && \
    make install && \
    make clean && \
    apk del autoconf automake make g++ && \
    rm -rf /opt/vsearch5d
ENTRYPOINT ["/usr/local/bin/vsearch5d"]
