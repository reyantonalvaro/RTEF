FROM alpine:3.17

RUN echo "https://dl-cdn.alpinelinux.org/alpine/v3.17/community" >> /etc/apk/repositories && \
    apk add --no-cache \
    clang \
    mold \
    ninja \
    musl-dev \
    python3 \
    py3-pip \
    bash \
    git \
    curl

WORKDIR /workspace
