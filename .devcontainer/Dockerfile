FROM node:18

RUN apt-get update && export DEBIAN_FRONTEND=noninteractive && \
    apt-get -y install build-essential socat htop xxd colordiff

RUN curl -L https://github.com/facebook/zstd/releases/download/v1.5.2/zstd-1.5.2.tar.gz | tar xvz && \
    mv /zstd-* /zstd && \
    cd /zstd && \
    make -j4 && \
    make -j4 install

RUN git clone https://github.com/axboe/liburing.git /liburing && \
    cd /liburing && \
    ./configure && \
    make -j4 && \
    make -j4 install
