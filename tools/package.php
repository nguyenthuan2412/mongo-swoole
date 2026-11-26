<?php
/**
 * Builds the PECL package: php tools/package.php  ->  swoole-mongo-<version>.tgz
 * (writes package.xml, embeds library/*.php into swm_library.h and runs "pecl package").
 */
$root = dirname(__DIR__);
chdir($root);
preg_match('/PHP_SWOOLE_MONGO_VERSION "([^"]+)"/', file_get_contents('php_swoole_mongo.h'), $m);
$version = $m[1] ?? exit("version not found\n");
passthru(PHP_BINARY . ' tools/gen_library.php', $rc);
if ($rc) exit($rc);

$roles = [
    'src' => ['config.m4', 'php_swoole_mongo.h', 'swm_library.h', 'swoole_mongo.c', 'swm_bson.c', 'swm_client.c', 'swm_crypt.c', 'swm_driver.c', 'swm_util.c',
        'tools/gen_library.php', ...glob('library/*.php')],
    'doc' => ['README.md', 'README.vi.md', 'LICENSE', 'docs/guide.md', 'docs/guide.vi.md', 'examples/http_server.php'],
    'test' => glob('testsuite/*.php'),
];
$files = '';
foreach ($roles as $role => $list) {
    foreach ($list as $f) {
        if (!is_file($f)) exit("missing $f\n");
        $files .= sprintf("      <file name=\"%s\" role=\"%s\"/>\n", $f, $role);
    }
}
$date = date('Y-m-d');
$xml = <<<XML
<?xml version="1.0" encoding="UTF-8"?>
<package packagerversion="1.10.0" version="2.0" xmlns="http://pear.php.net/dtd/package-2.0"
         xmlns:tasks="http://pear.php.net/dtd/tasks-1.0" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://pear.php.net/dtd/tasks-1.0 http://pear.php.net/dtd/tasks-1.0.xsd http://pear.php.net/dtd/package-2.0 http://pear.php.net/dtd/package-2.0.xsd">
  <name>swoole_mongo</name>
  <channel>pecl.php.net</channel>
  <summary>Non-blocking MongoDB driver for PHP and Swoole coroutines</summary>
  <description>
    A MongoDB driver written in C whose socket I/O yields to other Swoole coroutines.
    API compatible with ext-mongodb + mongodb/mongodb under the SwooleMongo namespace.
    Includes SCRAM/X509/AWS/OIDC/Kerberos auth, transactions, change streams, GridFS,
    CSFLE / Queryable Encryption (libmongocrypt) and the high level library embedded in the .so.
  </description>
  <lead>
    <name>nguyenthuan2412</name>
    <user>nguyenthuan2412</user>
    <email>nguyenthuan2412@users.noreply.github.com</email>
    <active>yes</active>
  </lead>
  <date>$date</date>
  <version>
    <release>$version</release>
    <api>$version</api>
  </version>
  <stability>
    <release>beta</release>
    <api>beta</api>
  </stability>
  <license uri="https://opensource.org/licenses/MIT">MIT</license>
  <notes>See https://github.com/nguyenthuan2412/mongo-swoole</notes>
  <contents>
    <dir name="/">
$files    </dir>
  </contents>
  <dependencies>
    <required>
      <php><min>8.1.0</min></php>
      <pearinstaller><min>1.10.0</min></pearinstaller>
    </required>
    <optional>
      <extension><name>swoole</name></extension>
    </optional>
  </dependencies>
  <providesextension>swoole_mongo</providesextension>
  <extsrcrelease>
    <configureoption name="with-swoole-mongo-gssapi" default="auto" prompt="Enable Kerberos (GSSAPI) auth? [auto/no/DIR]"/>
    <configureoption name="with-swoole-mongo-mongocrypt" default="auto" prompt="Enable client-side encryption with libmongocrypt? [auto/no/DIR]"/>
  </extsrcrelease>
</package>

XML;
file_put_contents('package.xml', $xml);
passthru('pecl package package.xml', $rc);
exit($rc);
