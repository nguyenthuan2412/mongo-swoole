<?php
declare(strict_types=1);

namespace SwooleMongo;

use SwooleMongo\Driver\CursorInterface;
use SwooleMongo\Driver\Manager;
use SwooleMongo\Driver\ReadConcern;
use SwooleMongo\Driver\ReadPreference;
use SwooleMongo\Driver\WriteConcern;
use SwooleMongo\Exception\InvalidArgumentException;
use SwooleMongo\GridFS\Bucket;
use SwooleMongo\Model\BSONArray;
use SwooleMongo\Model\BSONDocument;
use SwooleMongo\Model\CollectionInfoIterator;
use SwooleMongo\Operation\Aggregate;
use SwooleMongo\Operation\CreateCollection;
use SwooleMongo\Operation\DatabaseCommand;
use SwooleMongo\Operation\DropCollection;
use SwooleMongo\Operation\DropDatabase;
use SwooleMongo\Operation\ListCollectionNames;
use SwooleMongo\Operation\ListCollections;
use SwooleMongo\Operation\ModifyCollection;
use SwooleMongo\Operation\RenameCollection;
use SwooleMongo\Operation\Watch;

/** Mirrors MongoDB\Database. */
class Database
{
    private static array $defaultTypeMap = ['array' => BSONArray::class, 'document' => BSONDocument::class, 'root' => BSONDocument::class];

    private ReadConcern $readConcern;
    private ReadPreference $readPreference;
    private array $typeMap;
    private WriteConcern $writeConcern;

    public function __construct(private Manager $manager, private string $databaseName, array $options = [])
    {
        if ($databaseName === '') throw new InvalidArgumentException('$databaseName is invalid: ' . $databaseName);
        if (isset($options['readConcern']) && !$options['readConcern'] instanceof ReadConcern) throw InvalidArgumentException::invalidType('"readConcern" option', $options['readConcern'], ReadConcern::class);
        if (isset($options['readPreference']) && !$options['readPreference'] instanceof ReadPreference) throw InvalidArgumentException::invalidType('"readPreference" option', $options['readPreference'], ReadPreference::class);
        if (isset($options['typeMap']) && !is_array($options['typeMap'])) throw InvalidArgumentException::invalidType('"typeMap" option', $options['typeMap'], 'array');
        if (isset($options['writeConcern']) && !$options['writeConcern'] instanceof WriteConcern) throw InvalidArgumentException::invalidType('"writeConcern" option', $options['writeConcern'], WriteConcern::class);
        $this->readConcern = $options['readConcern'] ?? $manager->getReadConcern();
        $this->readPreference = $options['readPreference'] ?? $manager->getReadPreference();
        $this->typeMap = $options['typeMap'] ?? self::$defaultTypeMap;
        $this->writeConcern = $options['writeConcern'] ?? $manager->getWriteConcern();
    }

    public function __debugInfo(): array
    {
        return ['databaseName' => $this->databaseName, 'manager' => $this->manager, 'readConcern' => $this->readConcern,
            'readPreference' => $this->readPreference, 'typeMap' => $this->typeMap, 'writeConcern' => $this->writeConcern];
    }

    public function __get(string $collectionName): Collection
    {
        return $this->selectCollection($collectionName);
    }

    public function __toString(): string
    {
        return $this->databaseName;
    }

    private function inherit(array $options): array
    {
        return $options + ['readConcern' => $this->readConcern, 'readPreference' => $this->readPreference, 'typeMap' => $this->typeMap,
            'writeConcern' => $this->writeConcern];
    }

    private function writeOptions(array $options): array
    {
        if (!isset($options['writeConcern']) && !Functions::isInTransaction($options)) $options['writeConcern'] = $this->writeConcern;
        if (!isset($options['typeMap'])) $options['typeMap'] = $this->typeMap;
        return $options;
    }

    /** Runs an aggregation framework pipeline on the database for stages that do not require an underlying collection. */
    public function aggregate(array $pipeline, array $options = []): CursorInterface
    {
        $hasWrite = Functions::isLastPipelineOperatorWrite($pipeline);
        $inTx = Functions::isInTransaction($options);
        if (!isset($options['readPreference']) && !$inTx) $options['readPreference'] = $this->readPreference;
        $server = $hasWrite ? Functions::selectServerForAggregateWriteStage($this->manager, $options) : Functions::selectServer($this->manager, $options);
        if (!isset($options['readConcern']) && !$inTx) $options['readConcern'] = $this->readConcern;
        if (!isset($options['typeMap'])) $options['typeMap'] = $this->typeMap;
        if ($hasWrite && !isset($options['writeConcern']) && !$inTx) $options['writeConcern'] = $this->writeConcern;
        return (new Aggregate($this->databaseName, null, $pipeline, $options))->execute($server);
    }

    /** Executes a command on this database (always on the primary unless a readPreference is given). */
    public function command(array|object $command, array $options = []): CursorInterface
    {
        if (!isset($options['typeMap'])) $options['typeMap'] = $this->typeMap;
        $server = Functions::selectServer($this->manager, $options);
        return (new DatabaseCommand($this->databaseName, $command, $options))->execute($server);
    }

    public function createCollection(string $collectionName, array $options = []): array|object
    {
        $options = $this->writeOptions($options);
        if (!isset($options['encryptedFields'])) {
            $ef = Functions::encryptedFieldsFromMap($this->manager, $this->databaseName, $collectionName);
            if ($ef !== null) $options['encryptedFields'] = $ef;
        }
        $server = Functions::selectServer($this->manager, $options);
        if (!isset($options['encryptedFields'])) {
            return (new CreateCollection($this->databaseName, $collectionName, $options))->execute($server);
        }
        // Queryable Encryption: state collections, the collection itself, then the __safeContent__ index
        $ef = Functions::documentToArray($options['encryptedFields']);
        $aux = array_intersect_key($options, ['session' => 1, 'writeConcern' => 1]);
        foreach (Functions::encryptedStateCollections($collectionName, $ef) as $state) {
            (new CreateCollection($this->databaseName, $state, $aux + ['clusteredIndex' => ['key' => ['_id' => 1], 'unique' => true]]))->execute($server);
        }
        $result = (new CreateCollection($this->databaseName, $collectionName, $options))->execute($server);
        (new \SwooleMongo\Operation\CreateIndexes($this->databaseName, $collectionName, [['key' => ['__safeContent__' => 1]]], $aux))->execute($server);
        return $result;
    }

    /**
     * Creates a Queryable Encryption collection, creating a data key for every encrypted field whose keyId is null.
     * @return array{0: array|object, 1: array} the create result and the encryptedFields actually used
     */
    public function createEncryptedCollection(string $collectionName, \SwooleMongo\Driver\ClientEncryption $clientEncryption, string $kmsProvider, ?array $masterKey, array $options): array
    {
        if (!isset($options['encryptedFields']) || !is_array($options['encryptedFields']) && !is_object($options['encryptedFields'])) {
            throw InvalidArgumentException::invalidType('"encryptedFields" option', $options['encryptedFields'] ?? null, 'array or object');
        }
        $ef = Functions::documentToArray($options['encryptedFields']);
        $fields = $ef['fields'] ?? [];
        $fields = $fields instanceof \SwooleMongo\BSON\PackedArray ? $fields->toPHP(['root' => 'array', 'array' => 'array', 'document' => 'array']) : (array) $fields;
        foreach ($fields as $i => $field) {
            $field = Functions::documentToArray($field);
            if (array_key_exists('keyId', $field) && $field['keyId'] === null) {
                $field['keyId'] = $clientEncryption->createDataKey($kmsProvider, $masterKey === null ? [] : ['masterKey' => $masterKey]);
            }
            $fields[$i] = $field;
        }
        $ef['fields'] = array_values($fields);
        $options['encryptedFields'] = $ef;
        try {
            $result = $this->createCollection($collectionName, $options);
        } catch (\Throwable $e) {
            throw new Exception\CreateEncryptedCollectionException($e, $ef);
        }
        return [$result, $ef];
    }

    public function drop(array $options = []): array|object
    {
        $options = $this->writeOptions($options);
        return (new DropDatabase($this->databaseName, $options))->execute(Functions::selectServer($this->manager, $options));
    }

    public function dropCollection(string $collectionName, array $options = []): array|object
    {
        $options = $this->writeOptions($options);
        return Functions::dropCollection($this->manager, $this->databaseName, $collectionName, $options);
    }

    public function getDatabaseName(): string { return $this->databaseName; }
    public function getManager(): Manager { return $this->manager; }
    public function getReadConcern(): ReadConcern { return $this->readConcern; }
    public function getReadPreference(): ReadPreference { return $this->readPreference; }
    public function getTypeMap(): array { return $this->typeMap; }
    public function getWriteConcern(): WriteConcern { return $this->writeConcern; }

    public function listCollectionNames(array $options = []): \Iterator
    {
        return (new ListCollectionNames($this->databaseName, $options))->execute(Functions::selectServer($this->manager, $options));
    }

    public function listCollections(array $options = []): CollectionInfoIterator
    {
        return (new ListCollections($this->databaseName, $options))->execute(Functions::selectServer($this->manager, $options));
    }

    public function modifyCollection(string $collectionName, array $collectionOptions, array $options = []): array|object
    {
        $options = $this->writeOptions($options);
        return (new ModifyCollection($this->databaseName, $collectionName, $collectionOptions, $options))->execute(Functions::selectServer($this->manager, $options));
    }

    public function renameCollection(string $fromCollectionName, string $toCollectionName, ?string $toDatabaseName = null, array $options = []): array|object
    {
        $options = $this->writeOptions($options);
        return (new RenameCollection($this->databaseName, $fromCollectionName, $toDatabaseName ?? $this->databaseName, $toCollectionName, $options))
            ->execute(Functions::selectServer($this->manager, $options));
    }

    public function selectCollection(string $collectionName, array $options = []): Collection
    {
        return new Collection($this->manager, $this->databaseName, $collectionName, $this->inherit($options));
    }

    public function getCollection(string $collectionName, array $options = []): Collection
    {
        return $this->selectCollection($collectionName, $options);
    }

    public function selectGridFSBucket(array $options = []): Bucket
    {
        $options += ['readConcern' => $this->readConcern, 'readPreference' => $this->readPreference, 'typeMap' => $this->typeMap,
            'writeConcern' => $this->writeConcern];
        return new Bucket($this->manager, $this->databaseName, $options);
    }

    public function watch(array $pipeline = [], array $options = []): ChangeStream
    {
        $inTx = Functions::isInTransaction($options);
        if (!isset($options['readPreference']) && !$inTx) $options['readPreference'] = $this->readPreference;
        if (!isset($options['readConcern']) && !$inTx && !$this->readConcern->isDefault()) $options['readConcern'] = $this->readConcern;
        if (!isset($options['typeMap'])) $options['typeMap'] = $this->typeMap;
        $server = Functions::selectServer($this->manager, $options);
        return (new Watch($this->manager, $this->databaseName, null, $pipeline, $options))->execute($server);
    }

    public function withOptions(array $options = []): static
    {
        return new static($this->manager, $this->databaseName, $this->inherit($options));
    }
}
