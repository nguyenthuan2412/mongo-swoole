# Linux build of swoole-mongo with Swoole (and optional zstd/intl extensions),
# Kerberos (GSSAPI) and libmongocrypt (CSFLE / Queryable Encryption).
FROM php:8.3-cli

ARG LIBMONGOCRYPT_VERSION=1.11.0
ARG CRYPT_SHARED_VERSION=8.0.4

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $PHPIZE_DEPS zlib1g-dev libzstd-dev libicu-dev \
      libssl-dev libbrotli-dev libkrb5-dev krb5-user cmake git curl ca-certificates pkg-config \
 && docker-php-ext-install intl \
 && (yes '' | pecl install swoole-5.1.7) \
 && pecl install zstd \
 && docker-php-ext-enable swoole zstd \
 && rm -rf /var/lib/apt/lists/* /tmp/pear

# libmongocrypt (bundles its own libbson)
RUN curl -fsSL https://github.com/mongodb/libmongocrypt/archive/refs/tags/${LIBMONGOCRYPT_VERSION}.tar.gz | tar xz -C /tmp \
 && cmake -S /tmp/libmongocrypt-${LIBMONGOCRYPT_VERSION} -B /tmp/mcb -DBUILD_VERSION=${LIBMONGOCRYPT_VERSION} -DBUILD_TESTING=OFF \
      -DENABLE_ONLINE_TESTS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -Wno-dev \
 && cmake --build /tmp/mcb -j"$(nproc)" --target install \
 && ldconfig && rm -rf /tmp/mcb /tmp/libmongocrypt-*

# crypt_shared (automatic encryption query analysis) for the tests
RUN arch="$(uname -m)" && mkdir -p /opt/crypt \
 && curl -fsSL https://downloads.mongodb.com/linux/mongo_crypt_shared_v1-linux-${arch}-enterprise-ubuntu2204-${CRYPT_SHARED_VERSION}.tgz \
    | tar xz -C /opt/crypt --strip-components=1 lib/mongo_crypt_v1.so

COPY . /src
WORKDIR /src
RUN ./build.sh --with-swoole-mongo-gssapi --with-swoole-mongo-mongocrypt && make install && docker-php-ext-enable swoole_mongo \
 && php -m | grep -E 'swoole|swoole_mongo|zstd|intl' && php -r 'var_dump(class_exists("SwooleMongo\\Driver\\ClientEncryption"));'

CMD ["php", "-r", "echo 'swoole-mongo ', SWOOLE_MONGO_VERSION, PHP_EOL;"]
