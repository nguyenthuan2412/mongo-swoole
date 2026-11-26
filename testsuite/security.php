<?php
/**
 * Kerberos (GSSAPI) and client-side encryption (CSFLE / Queryable Encryption). Env (empty = skipped):
 *   SWM_RS_URI           replica set (default mongodb://127.0.0.1:27119/?replicaSet=rs0)
 *   SWM_GSSAPI_URI       URI with authMechanism=GSSAPI and the principal password
 *   SWM_GSSAPI_KINIT     "principal:password": obtain a ticket with kinit, then authenticate without password
 *   SWM_CRYPT_SHARED     path of mongo_crypt_v1 (automatic encryption)
 *   SWM_MONGOCRYPTD_URI  running mongocryptd (automatic encryption without crypt_shared)
 */
declare(strict_types=1);

use SwooleMongo\Client;
use SwooleMongo\BSON\Binary;
use SwooleMongo\Driver\ClientEncryption;
use SwooleMongo\Driver\Exception\{AuthenticationException, EncryptionException};

Swoole\Runtime::enableCoroutine(SWOOLE_HOOK_ALL);
$pass = $fail = $skip = 0;
function t(string $name, ?string $env, callable $fn): void
{
    global $pass, $fail, $skip;
    if ($env !== null && !getenv($env)) { $skip++; echo "  - $name (skipped, set $env)\n"; return; }
    try { $fn(); $pass++; echo "  \033[32m✓\033[0m $name\n"; }
    catch (Throwable $e) { $fail++; echo "  \033[31m✗ $name\033[0m: ", get_class($e), ': ', $e->getMessage(), ' @', $e->getLine(), "\n"; }
}
function ok(bool $c, string $m = 'assertion failed'): void { if (!$c) throw new RuntimeException($m); }

Swoole\Coroutine\run(function () {
    $RS = getenv('SWM_RS_URI') ?: 'mongodb://127.0.0.1:27119/?replicaSet=rs0';
    $hasCrypt = class_exists(ClientEncryption::class);

    t('GSSAPI with password', 'SWM_GSSAPI_URI', function () {
        $c = new Client(getenv('SWM_GSSAPI_URI'));
        $st = $c->selectDatabase('admin')->command(['connectionStatus' => 1])->toArray()[0];
        ok($st->authInfo->authenticatedUsers[0]->user === 'swmuser@EXAMPLE.COM', 'wrong user');
        $c->selectCollection('test', 'krb')->insertOne(['k' => 1]);
        ok($c->selectCollection('test', 'krb')->countDocuments() >= 1);
    });
    t('GSSAPI with kinit ticket cache, in parallel coroutines', 'SWM_GSSAPI_KINIT', function () {
        [$principal, $password] = explode(':', getenv('SWM_GSSAPI_KINIT'), 2);
        $cache = '/tmp/swm_krb5cc';
        putenv("KRB5CCNAME=FILE:$cache");
        $p = proc_open(['kinit', $principal], [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes);
        fwrite($pipes[0], $password . "\n");
        fclose($pipes[0]);
        ok(proc_close($p) === 0, 'kinit failed');
        $host = parse_url(getenv('SWM_GSSAPI_URI'), PHP_URL_HOST);
        $uri = 'mongodb://' . rawurlencode($principal) . "@$host:27017/?authMechanism=GSSAPI&authMechanismProperties=SERVICE_NAME:mongodb";
        $wg = new Swoole\Coroutine\WaitGroup();
        $okCount = 0;
        for ($i = 0; $i < 4; $i++) {
            $wg->add();
            go(function () use ($uri, $wg, &$okCount) {
                try {
                    $c = new Client($uri);
                    if ($c->selectCollection('test', 'krb')->countDocuments() >= 1) $okCount++;
                } catch (Throwable $e) {
                    echo '    ', get_class($e), ': ', $e->getMessage(), "\n";
                }
                $wg->done();
            });
        }
        $wg->wait();
        ok($okCount === 4);
    });
    t('GSSAPI wrong password -> AuthenticationException', 'SWM_GSSAPI_URI', function () {
        $uri = str_replace(':swmpass@', ':wrong@', getenv('SWM_GSSAPI_URI'));
        try { (new Client($uri))->selectDatabase('test')->command(['ping' => 1]); ok(false); }
        catch (AuthenticationException) {}
    });

    if (!$hasCrypt) {
        echo "  - client-side encryption (skipped, swoole-mongo built without libmongocrypt)\n";
    } else {
        $kms = ['local' => ['key' => new Binary(random_bytes(96), Binary::TYPE_GENERIC)]];
        $plain = new Client($RS);
        $plain->selectCollection('swm_kv', 'datakeys')->drop();
        $plain->selectDatabase('swm_enc')->drop();
        $ce = $plain->createClientEncryption(['keyVaultNamespace' => 'swm_kv.datakeys', 'kmsProviders' => $kms]);
        $kid = null;

        t('explicit encryption + key management', null, function () use ($ce, &$kid) {
            $kid = $ce->createDataKey('local', ['keyAltNames' => ['main']]);
            ok($kid instanceof Binary && $kid->getType() === Binary::TYPE_UUID);
            $det = $ce->encrypt('secret', ['keyId' => $kid, 'algorithm' => ClientEncryption::AEAD_AES_256_CBC_HMAC_SHA_512_DETERMINISTIC]);
            ok($det->getType() === Binary::TYPE_ENCRYPTED && $ce->decrypt($det) === 'secret');
            ok($det == $ce->encrypt('secret', ['keyAltName' => 'main', 'algorithm' => ClientEncryption::AEAD_AES_256_CBC_HMAC_SHA_512_DETERMINISTIC]));
            $rnd = $ce->encrypt(['a' => 1], ['keyId' => $kid, 'algorithm' => ClientEncryption::AEAD_AES_256_CBC_HMAC_SHA_512_RANDOM]);
            ok($ce->decrypt($rnd)->a === 1);
            $ce->addKeyAltName($kid, 'other');
            ok($ce->getKeyByAltName('other')->_id == $kid);
            ok($ce->removeKeyAltName($kid, 'other') !== null && $ce->getKeyByAltName('other') === null);
            ok(count(iterator_to_array($ce->getKeys())) === 1);
            ok($ce->rewrapManyDataKey([], ['provider' => 'local'])->bulkWriteResult->modifiedCount === 1);
            $tmp = $ce->createDataKey('local');
            ok($ce->deleteKey($tmp)->deletedCount === 1 && $ce->getKey($tmp) === null);
            try { $ce->encrypt('x', ['keyAltName' => 'missing', 'algorithm' => ClientEncryption::AEAD_AES_256_CBC_HMAC_SHA_512_RANDOM]); ok(false); }
            catch (EncryptionException) {}
        });

        $auto = function (array $extra) use ($RS, $kms, &$kid) {
            $schema = ['swm_enc.people' => ['bsonType' => 'object', 'encryptMetadata' => ['keyId' => [$kid]], 'properties' => [
                'ssn' => ['encrypt' => ['bsonType' => 'string', 'algorithm' => ClientEncryption::AEAD_AES_256_CBC_HMAC_SHA_512_DETERMINISTIC]]]]];
            $c = new Client($RS, [], ['autoEncryption' => ['keyVaultNamespace' => 'swm_kv.datakeys', 'kmsProviders' => $kms,
                'schemaMap' => $schema, 'extraOptions' => $extra]]);
            $col = $c->selectCollection('swm_enc', 'people');
            $col->deleteMany([]);
            $col->insertOne(['_id' => 1, 'ssn' => '111', 'name' => 'a', 'meta' => new stdClass()]);
            $col->insertMany([['_id' => 2, 'ssn' => '222', 'name' => 'b'], ['_id' => 3, 'ssn' => '333', 'name' => 'c']]);
            $raw = (new Client($RS))->selectCollection('swm_enc', 'people')->findOne(['_id' => 1]);
            ok($raw->ssn instanceof Binary && $raw->ssn->getType() === Binary::TYPE_ENCRYPTED, 'not encrypted at rest');
            $d = $col->findOne(['ssn' => '222']);
            ok($d->name === 'b' && $d->ssn === '222', 'query on encrypted field');
            $col->updateOne(['ssn' => '333'], ['$set' => ['name' => 'cc']]);
            $all = iterator_to_array($col->find([], ['sort' => ['_id' => 1], 'batchSize' => 1]));
            ok(implode(',', array_map(fn ($x) => $x->ssn . $x->name, $all)) === '111a,222b,333cc', 'decrypt across getMore');
            ok(json_encode($all[0]->meta) === '{}', 'empty document preserved');
        };
        t('automatic encryption (crypt_shared)', 'SWM_CRYPT_SHARED', fn () => $auto(['cryptSharedLibPath' => getenv('SWM_CRYPT_SHARED'), 'cryptSharedLibRequired' => true]));
        t('automatic encryption (mongocryptd)', 'SWM_MONGOCRYPTD_URI', fn () => $auto(['mongocryptdURI' => getenv('SWM_MONGOCRYPTD_URI'), 'mongocryptdBypassSpawn' => true]));

        t('automatic decryption with bypassAutoEncryption', null, function () use ($RS, $kms, $ce, &$kid) {
            $plainColl = (new Client($RS))->selectCollection('swm_enc', 'manual');
            $plainColl->drop();
            $plainColl->insertOne(['v' => $ce->encrypt(42, ['keyId' => $kid, 'algorithm' => ClientEncryption::AEAD_AES_256_CBC_HMAC_SHA_512_RANDOM])]);
            $c = new Client($RS, [], ['autoEncryption' => ['keyVaultNamespace' => 'swm_kv.datakeys', 'kmsProviders' => $kms, 'bypassAutoEncryption' => true]]);
            ok($c->selectCollection('swm_enc', 'manual')->findOne()->v === 42);
        });

        t('Queryable Encryption: createEncryptedCollection + equality query', 'SWM_CRYPT_SHARED', function () use ($RS, $kms, $ce, $plain) {
            $db = $plain->selectDatabase('swm_enc');
            [, $ef] = $db->createEncryptedCollection('qe', $ce, 'local', null, ['encryptedFields' => ['fields' => [
                ['path' => 'card', 'bsonType' => 'string', 'keyId' => null, 'queries' => ['queryType' => 'equality']]]]]);
            ok($ef['fields'][0]['keyId'] instanceof Binary);
            ok(in_array('enxcol_.qe.esc', iterator_to_array($db->listCollectionNames()), true), 'state collection missing');
            $c = new Client($RS, [], ['autoEncryption' => ['keyVaultNamespace' => 'swm_kv.datakeys', 'kmsProviders' => $kms,
                'encryptedFieldsMap' => ['swm_enc.qe' => $ef], 'extraOptions' => ['cryptSharedLibPath' => getenv('SWM_CRYPT_SHARED')]]]);
            $q = $c->selectCollection('swm_enc', 'qe');
            $q->insertOne(['_id' => 1, 'card' => '4111']);
            $q->insertOne(['_id' => 2, 'card' => '5500']);
            ok($q->findOne(['card' => '5500'])->_id === 2);
            ok($db->selectCollection('qe')->findOne(['_id' => 1])->card->getType() === Binary::TYPE_ENCRYPTED);
            $e = $ce->encrypt('4111', ['keyId' => $ef['fields'][0]['keyId'], 'algorithm' => ClientEncryption::ALGORITHM_INDEXED, 'contentionFactor' => 0]);
            ok($e->getType() === Binary::TYPE_ENCRYPTED);
            $c->selectDatabase('swm_enc')->dropCollection('qe');
            ok(!in_array('enxcol_.qe.esc', iterator_to_array($db->listCollectionNames()), true), 'state collection not dropped');
        });
    }

    global $pass, $fail, $skip;
    echo "\n$pass passed, $fail failed, $skip skipped\n";
    if ($fail) $GLOBALS['swm_exit'] = 1;
});
exit($GLOBALS['swm_exit'] ?? 0);
