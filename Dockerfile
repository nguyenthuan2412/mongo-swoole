# Linux build of swmongo with Swoole (and optional zstd/intl extensions).
FROM php:8.3-cli

RUN apt-get update \
 && apt-get install -y --no-install-recommends $PHPIZE_DEPS zlib1g-dev libzstd-dev libicu-dev libssl-dev libbrotli-dev \
 && docker-php-ext-install intl \
 && (yes '' | pecl install swoole-5.1.7) \
 && pecl install zstd \
 && docker-php-ext-enable swoole zstd \
 && rm -rf /var/lib/apt/lists/* /tmp/pear

COPY . /src
WORKDIR /src
RUN ./build.sh && make install && docker-php-ext-enable swmongo && php -m | grep -E 'swoole|swmongo|zstd|intl'

CMD ["php", "-r", "echo 'swmongo ', SWMONGO_VERSION, PHP_EOL;"]
