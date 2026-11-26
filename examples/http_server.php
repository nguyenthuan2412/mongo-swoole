<?php
/**
 * Swoole HTTP server using swoole-mongo. One MongoClient per worker (create it in
 * workerStart, never before the fork). Each request runs in its own coroutine;
 * MongoDB I/O yields so a worker serves many requests concurrently.
 *
 *   php -d extension=swoole_mongo.so examples/http_server.php
 *   curl 'http://127.0.0.1:9501/users?name=ann'
 */
use Swoole\Http\Server;
use SwooleMongo\Client;

$uri = getenv('MONGO_URI') ?: 'mongodb://127.0.0.1:27119/?replicaSet=rs0';
$server = new Server('127.0.0.1', (int) (getenv('PORT') ?: 9501));
$server->set([
    'worker_num' => 2,
    'enable_coroutine' => true,
    'hook_flags' => SWOOLE_HOOK_ALL,
]);

/** @var Client|null $client */
$client = null;

$server->on('workerStart', function () use (&$client, $uri) {
    $client = new Client($uri, ['maxPoolSize' => 64]);
});

$server->on('request', function ($req, $res) use (&$client) {
    $users = $client->selectCollection('demo', 'users');
    try {
        switch ($req->server['request_uri']) {
            case '/users':
                if ($req->server['request_method'] === 'POST') {
                    $doc = json_decode($req->rawContent() ?: '{}', true) ?: [];
                    $r = $users->insertOne($doc);
                    $out = ['insertedId' => (string) $r->getInsertedId()];
                } else {
                    $filter = isset($req->get['name']) ? ['name' => $req->get['name']] : [];
                    $out = $users->find($filter, ['sort' => ['_id' => -1], 'limit' => 20])->toArray();
                }
                break;
            case '/slow':
                // a 100ms server-side query: concurrent requests do not wait for each other
                $out = $users->findOne(['$where' => 'sleep(100) || true']);
                break;
            default:
                $res->status(404);
                $out = ['error' => 'not found'];
        }
        $res->header('Content-Type', 'application/json');
        $res->end(json_encode($out));
    } catch (Throwable $e) {
        $res->status(500);
        $res->end(json_encode(['error' => $e->getMessage()]));
    }
});

$server->start();
