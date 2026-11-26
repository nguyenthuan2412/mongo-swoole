// $external user for the Kerberos principal
db.getSiblingDB('$external').createUser({user: 'swmuser@EXAMPLE.COM', roles: [{role: 'readWrite', db: 'test'}]});
