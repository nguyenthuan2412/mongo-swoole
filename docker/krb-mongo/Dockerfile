# MongoDB Enterprise + the cyrus-sasl GSSAPI plugin (not shipped in the official image)
FROM mongodb/mongodb-enterprise-server:8.0-ubuntu2204
USER root
# only "main" is needed: skip the large universe/multiverse indexes
RUN printf 'deb http://ports.ubuntu.com/ubuntu-ports jammy main\ndeb http://ports.ubuntu.com/ubuntu-ports jammy-updates main\n' > /etc/apt/sources.list \
 && if [ "$(uname -m)" = x86_64 ]; then sed -i 's#ports.ubuntu.com/ubuntu-ports#archive.ubuntu.com/ubuntu#' /etc/apt/sources.list; fi \
 && apt-get -o Acquire::http::Timeout=20 -o Acquire::Retries=3 update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends libsasl2-modules-gssapi-mit \
 && rm -rf /var/lib/apt/lists/*
USER mongodb
