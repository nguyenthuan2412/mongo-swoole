# Installs swoole-mongo the way users do, on a clean php image: with PECL (from the package) and with PIE (from the source tree).
#   php tools/package.php && docker build -f docker/install-test/Dockerfile .
FROM php:8.3-cli AS base
RUN apt-get update && apt-get install -y --no-install-recommends $PHPIZE_DEPS zlib1g-dev libssl-dev libbrotli-dev git unzip \
 && (yes '' | pecl install swoole-5.1.7) && docker-php-ext-enable swoole && rm -rf /var/lib/apt/lists/* /tmp/pear

FROM base AS pecl
COPY swoole_mongo-*.tgz /tmp/
RUN pecl install /tmp/swoole_mongo-*.tgz < /dev/null && docker-php-ext-enable swoole_mongo \
 && php --ri swoole_mongo && php -r 'var_dump(SWOOLE_MONGO_VERSION, class_exists("SwooleMongo\\Client"));'

FROM base AS pie
COPY --from=composer:2 /usr/bin/composer /usr/bin/composer
ADD https://github.com/php/pie/releases/latest/download/pie.phar /usr/local/bin/pie
RUN chmod +x /usr/local/bin/pie
COPY . /src
RUN cd /src && git init -q 2>/dev/null; git -C /src config user.email ci@example.com && git -C /src config user.name ci \
 && git -C /src add -A >/dev/null && git -C /src commit -qm snapshot >/dev/null || true
RUN pie repository:add path /src && pie install nguyenthuan2412/swoole-mongo:@dev \
 && php --ri swoole_mongo && php -r 'var_dump(SWOOLE_MONGO_VERSION, class_exists("SwooleMongo\\Client"));'
