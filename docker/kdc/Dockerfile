# Minimal MIT Kerberos KDC for the GSSAPI tests (realm EXAMPLE.COM)
FROM debian:bookworm-slim
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends krb5-kdc krb5-admin-server \
 && rm -rf /var/lib/apt/lists/*
COPY krb5.conf /etc/krb5.conf
COPY kdc/start.sh /start.sh
RUN chmod +x /start.sh
CMD ["/start.sh"]
