<?php
declare(strict_types=1);

namespace SwooleMongo;

use SwooleMongo\Driver\Manager;
use SwooleMongo\Driver\Monitoring\Subscriber;
use SwooleMongo\Driver\ReadConcern;
use SwooleMongo\Driver\ReadPreference;
use SwooleMongo\Driver\Session;
use SwooleMongo\Driver\WriteConcern;
use SwooleMongo\Exception\InvalidArgumentException;
use SwooleMongo\Exception\UnsupportedException;
use SwooleMongo\Model\BSONArray;
use SwooleMongo\Model\BSONDocument;
use SwooleMongo\Model\DatabaseInfoIterator;
use SwooleMongo\Operation\DropDatabase;
use SwooleMongo\Operation\ListDatabaseNames;
use SwooleMongo\Operation\ListDatabases;
use SwooleMongo\Operation\Watch;

/**
 * Mirrors MongoDB\Client.
 *
 * Every operation is coroutine friendly: with Swoole runtime hooks enabled
 * (Co\run() enables them, or 'hook_flags' => SWOOLE_HOOK_ALL for servers)
 * socket I/O yields to other coroutines.
 */
class Client
{
    public const DEFAULT_URI = 'mongodb://127.0.0.1/';

    private static array $defaultTypeMap = ['array' => BSONArray::class, 'document' => BSONDocument::class, 'root' => BSONDocument::class];

    private Manager $manager;
    private ReadConcern $readConcern;
    private ReadPreference $readPreference;
    private string $uri;
    private array $typeMap;
    private WriteConcern $writeConcern;

    public function __construct(?string $uri = null, array $uriOptions = [], array $driverOptions = [])
    {
        $driverOptions += ['typeMap' => self::$defaultTypeMap];
        if (!is_array($driverOptions['typeMap'])) {
            throw InvalidArgumentException::invalidType('"typeMap" driver option', $driverOptions['typeMap'], 'array');
        }
        $this->uri = $uri ?? self::DEFAULT_URI;
        $this->typeMap = $driverOptions['typeMap'];
        unset($driverOptions['typeMap'], $driverOptions['builderEncoder'], $driverOptions['codec']);
        if (isset($driverOptions['autoEncryption']['keyVaultClient']) && $driverOptions['autoEncryption']['keyVaultClient'] instanceof self) {
            $driverOptions['autoEncryption']['keyVaultClient'] = $driverOptions['autoEncryption']['keyVaultClient']->getManager();
        }
        $this->manager = new Manager($this->uri, $uriOptions, $driverOptions);
        $this->readConcern = $this->manager->getReadConcern();
        $this->readPreference = $this->manager->getReadPreference();
        $this->writeConcern = $this->manager->getWriteConcern();
    }

    public function __debugInfo(): array
    {
        return ['manager' => $this->manager, 'uri' => $this->uri, 'typeMap' => $this->typeMap];
    }

    public function __get(string $databaseName): Database
    {
        return $this->selectDatabase($databaseName);
    }

    public function __toString(): string
    {
        return $this->uri;
    }

    public function addSubscriber(Subscriber $subscriber): void
    {
        $this->manager->addSubscriber($subscriber);
    }

    public function removeSubscriber(Subscriber $subscriber): void
    {
        $this->manager->removeSubscriber($subscriber);
    }

    /** MongoDB 8.0+ client-level bulk write across namespaces. */
    public function bulkWrite(ClientBulkWrite $bulk, array $options = []): \SwooleMongo\Driver\BulkWriteCommandResult
    {
        if ($bulk->getManager() !== $this->manager) {
            throw new InvalidArgumentException('$bulk was not created with this client\'s manager');
        }
        if (!isset($options['writeConcern']) && !Functions::isInTransaction($options)) $options['writeConcern'] = $this->writeConcern;
        if (isset($options['writeConcern']) && $options['writeConcern']->isDefault()) unset($options['writeConcern']);
        $server = Functions::selectServer($this->manager, $options);
        return $server->executeBulkWriteCommand($bulk->bulkWriteCommand, array_intersect_key($options, ['session' => 1, 'writeConcern' => 1, 'timeoutMS' => 1]));
    }

    public function createClientEncryption(array $options): \SwooleMongo\Driver\ClientEncryption
    {
        if (!class_exists(\SwooleMongo\Driver\ClientEncryption::class, false)) {
            throw UnsupportedException::clientEncryptionNotSupported();
        }
        if (isset($options['keyVaultClient'])) {
            if ($options['keyVaultClient'] instanceof self) {
                $options['keyVaultClient'] = $options['keyVaultClient']->getManager();
            } elseif (!$options['keyVaultClient'] instanceof Manager) {
                throw InvalidArgumentException::invalidType('"keyVaultClient" option', $options['keyVaultClient'], [self::class, Manager::class]);
            }
        }
        return $this->manager->createClientEncryption($options);
    }

    public function dropDatabase(string $databaseName, array $options = []): array|object
    {
        if (!isset($options['typeMap'])) $options['typeMap'] = $this->typeMap;
        if (!isset($options['writeConcern']) && !Functions::isInTransaction($options)) $options['writeConcern'] = $this->writeConcern;
        return (new DropDatabase($databaseName, $options))->execute(Functions::selectServer($this->manager, $options));
    }

    public function getManager(): Manager { return $this->manager; }
    public function getReadConcern(): ReadConcern { return $this->readConcern; }
    public function getReadPreference(): ReadPreference { return $this->readPreference; }
    public function getTypeMap(): array { return $this->typeMap; }
    public function getWriteConcern(): WriteConcern { return $this->writeConcern; }

    public function listDatabaseNames(array $options = []): \Iterator
    {
        return (new ListDatabaseNames($options))->execute(Functions::selectServer($this->manager, $options));
    }

    public function listDatabases(array $options = []): DatabaseInfoIterator
    {
        return (new ListDatabases($options))->execute(Functions::selectServer($this->manager, $options));
    }

    public function selectCollection(string $databaseName, string $collectionName, array $options = []): Collection
    {
        $options += ['typeMap' => $this->typeMap];
        return new Collection($this->manager, $databaseName, $collectionName, $options);
    }

    public function getCollection(string $databaseName, string $collectionName, array $options = []): Collection
    {
        return $this->selectCollection($databaseName, $collectionName, $options);
    }

    public function selectDatabase(string $databaseName, array $options = []): Database
    {
        $options += ['typeMap' => $this->typeMap];
        return new Database($this->manager, $databaseName, $options);
    }

    public function getDatabase(string $databaseName, array $options = []): Database
    {
        return $this->selectDatabase($databaseName, $options);
    }

    public function startSession(array $options = []): Session
    {
        return $this->manager->startSession($options);
    }

    public function watch(array $pipeline = [], array $options = []): ChangeStream
    {
        $inTx = Functions::isInTransaction($options);
        if (!isset($options['readPreference']) && !$inTx) $options['readPreference'] = $this->readPreference;
        if (!isset($options['readConcern']) && !$inTx && !$this->readConcern->isDefault()) $options['readConcern'] = $this->readConcern;
        if (!isset($options['typeMap'])) $options['typeMap'] = $this->typeMap;
        $server = Functions::selectServer($this->manager, $options);
        return (new Watch($this->manager, null, null, $pipeline, $options))->execute($server);
    }
}
