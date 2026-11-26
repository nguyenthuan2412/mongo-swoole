#!/bin/sh
# Creates the realm, the mongod service principal (keytab in /keytab) and a test user, then runs the KDC.
set -e
mkdir -p /etc/krb5kdc /keytab
printf '[realms]\n EXAMPLE.COM = {\n  database_name = /var/lib/krb5kdc/principal\n  acl_file = /etc/krb5kdc/kadm5.acl\n }\n' > /etc/krb5kdc/kdc.conf
if [ ! -f /var/lib/krb5kdc/principal ]; then
  kdb5_util create -s -r EXAMPLE.COM -P masterpass
fi
rm -f /keytab/mongodb.keytab /keytab/ready
kadmin.local -q "addprinc -randkey mongodb/krb-mongo@EXAMPLE.COM"
kadmin.local -q "ktadd -k /keytab/mongodb.keytab mongodb/krb-mongo@EXAMPLE.COM"
kadmin.local -q "addprinc -pw swmpass swmuser@EXAMPLE.COM"
chmod 644 /keytab/mongodb.keytab
touch /keytab/ready
exec krb5kdc -n
