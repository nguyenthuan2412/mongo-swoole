<?php
declare(strict_types=1);

namespace SwooleMongo;

use SwooleMongo\Driver\CursorInterface;
use SwooleMongo\Driver\Manager;
use SwooleMongo\Driver\ReadConcern;
use SwooleMongo\Driver\ReadPreference;
use SwooleMongo\Driver\WriteConcern;
use SwooleMongo\Exception\InvalidArgumentException;
use SwooleMongo\Model\BSONArray;
use SwooleMongo\Model\BSONDocument;
use SwooleMongo\Model\IndexInfo;
use SwooleMongo\Model\IndexInfoIterator;
use SwooleMongo\Operation\Aggregate;
use SwooleMongo\Operation\BulkWrite;
use SwooleMongo\Operation\Count;
use SwooleMongo\Operation\CountDocuments;
use SwooleMongo\Operation\CreateIndexes;
use SwooleMongo\Operation\CreateSearchIndexes;
use SwooleMongo\Operation\DeleteMany;
use SwooleMongo\Operation\DeleteOne;
use SwooleMongo\Operation\Distinct;
use SwooleMongo\Operation\DropCollection;
use SwooleMongo\Operation\DropIndexes;
use SwooleMongo\Operation\DropSearchIndex;
use SwooleMongo\Operation\EstimatedDocumentCount;
use SwooleMongo\Operation\Explain;
use SwooleMongo\Operation\Explainable;
use SwooleMongo\Operation\Find;
use SwooleMongo\Operation\FindOne;
use SwooleMongo\Operation\FindOneAndDelete;
use SwooleMongo\Operation\FindOneAndReplace;
use SwooleMongo\Operation\FindOneAndUpdate;
use SwooleMongo\Operation\InsertMany;
use SwooleMongo\Operation\InsertOne;
use SwooleMongo\Operation\ListIndexes;
use SwooleMongo\Operation\ListSearchIndexes;
use SwooleMongo\Operation\RenameCollection;
use SwooleMongo\Operation\ReplaceOne;
use SwooleMongo\Operation\UpdateMany;
use SwooleMongo\Operation\UpdateOne;
use SwooleMongo\Operation\UpdateSearchIndex;
use SwooleMongo\Operation\Watch;

/** Mirrors MongoDB\Collection. */
class Collection
{
    private static array $defaultTypeMap = ['array' => BSONArray::class, 'document' => BSONDocument::class, 'root' => BSONDocument::class];

    private ReadConcern $readConcern;
    private ReadPreference $readPreference;
    private array $typeMap;
    private WriteConcern $writeConcern;

    public function __construct(private Manager $manager, private string $databaseName, private string $collectionName, array $options = [])
    {
        if ($databaseName === '') throw new InvalidArgumentException('$databaseName is invalid: ' . $databaseName);
        if ($collectionName === '') throw new InvalidArgumentException('$collectionName is invalid: ' . $collectionName);
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
        return ['collectionName' => $this->collectionName, 'databaseName' => $this->databaseName, 'manager' => $this->manager,
            'readConcern' => $this->readConcern, 'readPreference' => $this->readPreference, 'typeMap' => $this->typeMap,
            'writeConcern' => $this->writeConcern];
    }

    public function __toString(): string
    {
        return $this->databaseName . '.' . $this->collectionName;
    }

    /* ---------------------------------------------------- option helpers */
    private function readOptions(array $options, bool $withReadConcern = true, bool $withTypeMap = true): array
    {
        $inTx = Functions::isInTransaction($options);
        if (!isset($options['readPreference']) && !$inTx) $options['readPreference'] = $this->readPreference;
        if ($withReadConcern && !isset($options['readConcern']) && !$inTx) $options['readConcern'] = $this->readConcern;
        if ($withTypeMap && !isset($options['typeMap'])) $options['typeMap'] = $this->typeMap;
        return $options;
    }

    private function writeOptions(array $options, bool $withTypeMap = false): array
    {
        if (!isset($options['writeConcern']) && !Functions::isInTransaction($options)) $options['writeConcern'] = $this->writeConcern;
        if ($withTypeMap && !isset($options['typeMap'])) $options['typeMap'] = $this->typeMap;
        return $options;
    }

    private function primary(array $options)
    {
        return Functions::selectServer($this->manager, $options);
    }

    /* --------------------------------------------------------------- CRUD */
    public function aggregate(array $pipeline, array $options = []): CursorInterface
    {
        $hasWrite = Functions::isLastPipelineOperatorWrite($pipeline);
        $inTx = Functions::isInTransaction($options);
        if (!isset($options['readPreference']) && !$inTx) $options['readPreference'] = $this->readPreference;
        $server = $hasWrite ? Functions::selectServerForAggregateWriteStage($this->manager, $options) : Functions::selectServer($this->manager, $options);
        if (!isset($options['readConcern']) && !$inTx) $options['readConcern'] = $this->readConcern;
        if (!isset($options['typeMap'])) $options['typeMap'] = $this->typeMap;
        if ($hasWrite && !isset($options['writeConcern']) && !$inTx) $options['writeConcern'] = $this->writeConcern;
        return (new Aggregate($this->databaseName, $this->collectionName, $pipeline, $options))->execute($server);
    }

    public function bulkWrite(array $operations, array $options = []): BulkWriteResult
    {
        $options = $this->writeOptions($options);
        return (new BulkWrite($this->databaseName, $this->collectionName, $operations, $options))->execute($this->primary($options));
    }

    /** @deprecated use countDocuments() or estimatedDocumentCount() */
    public function count(array|object $filter = [], array $options = []): int
    {
        $options = $this->readOptions($options, true, false);
        return (new Count($this->databaseName, $this->collectionName, $filter, $options))->execute(Functions::selectServer($this->manager, $options));
    }

    public function countDocuments(array|object $filter = [], array $options = []): int
    {
        $options = $this->readOptions($options, true, false);
        return (new CountDocuments($this->databaseName, $this->collectionName, $filter, $options))->execute(Functions::selectServer($this->manager, $options));
    }

    public function createIndex(array|object $key, array $options = []): string
    {
        $commandOptionKeys = ['commitQuorum' => 1, 'maxTimeMS' => 1, 'session' => 1, 'writeConcern' => 1, 'comment' => 1];
        $indexOptions = array_diff_key($options, $commandOptionKeys);
        $commandOptions = array_intersect_key($options, $commandOptionKeys);
        return current($this->createIndexes([['key' => $key] + $indexOptions], $commandOptions));
    }

    public function createIndexes(array $indexes, array $options = []): array
    {
        $options = $this->writeOptions($options);
        return (new CreateIndexes($this->databaseName, $this->collectionName, $indexes, $options))->execute($this->primary($options));
    }

    public function createSearchIndex(array|object $definition, array $options = []): string
    {
        $index = ['definition' => $definition];
        if (isset($options['name'])) { $index['name'] = $options['name']; unset($options['name']); }
        if (isset($options['type'])) { $index['type'] = $options['type']; unset($options['type']); }
        return current($this->createSearchIndexes([$index], $options));
    }

    public function createSearchIndexes(array $indexes, array $options = []): array
    {
        return (new CreateSearchIndexes($this->databaseName, $this->collectionName, $indexes, $options))->execute($this->primary($options));
    }

    public function deleteMany(array|object $filter, array $options = []): DeleteResult
    {
        $options = $this->writeOptions($options);
        return (new DeleteMany($this->databaseName, $this->collectionName, $filter, $options))->execute($this->primary($options));
    }

    public function deleteOne(array|object $filter, array $options = []): DeleteResult
    {
        $options = $this->writeOptions($options);
        return (new DeleteOne($this->databaseName, $this->collectionName, $filter, $options))->execute($this->primary($options));
    }

    public function distinct(string $fieldName, array|object $filter = [], array $options = []): array
    {
        $options = $this->readOptions($options);
        return (new Distinct($this->databaseName, $this->collectionName, $fieldName, $filter, $options))->execute(Functions::selectServer($this->manager, $options));
    }

    public function drop(array $options = []): array|object
    {
        $options = $this->writeOptions($options, true);
        return (new DropCollection($this->databaseName, $this->collectionName, $options))->execute($this->primary($options));
    }

    public function dropIndex(string|IndexInfo $indexName, array $options = []): array|object
    {
        $indexName = (string) $indexName;
        if ($indexName === '*') throw new InvalidArgumentException('dropIndexes() must be used to drop multiple indexes');
        $options = $this->writeOptions($options, true);
        return (new DropIndexes($this->databaseName, $this->collectionName, $indexName, $options))->execute($this->primary($options));
    }

    public function dropIndexes(array $options = []): array|object
    {
        $options = $this->writeOptions($options, true);
        return (new DropIndexes($this->databaseName, $this->collectionName, '*', $options))->execute($this->primary($options));
    }

    public function dropSearchIndex(string $name, array $options = []): void
    {
        (new DropSearchIndex($this->databaseName, $this->collectionName, $name, $options))->execute($this->primary($options));
    }

    public function estimatedDocumentCount(array $options = []): int
    {
        $options = $this->readOptions($options, true, false);
        return (new EstimatedDocumentCount($this->databaseName, $this->collectionName, $options))->execute(Functions::selectServer($this->manager, $options));
    }

    public function explain(Explainable $explainable, array $options = []): array|object
    {
        if (!isset($options['readPreference']) && !Functions::isInTransaction($options)) $options['readPreference'] = $this->readPreference;
        if (!isset($options['typeMap'])) $options['typeMap'] = $this->typeMap;
        return (new Explain($this->databaseName, $explainable, $options))->execute(Functions::selectServer($this->manager, $options));
    }

    public function find(array|object $filter = [], array $options = []): CursorInterface
    {
        $options = $this->readOptions($options);
        return (new Find($this->databaseName, $this->collectionName, $filter, $options))->execute(Functions::selectServer($this->manager, $options));
    }

    public function findOne(array|object $filter = [], array $options = []): array|object|null
    {
        $options = $this->readOptions($options);
        return (new FindOne($this->databaseName, $this->collectionName, $filter, $options))->execute(Functions::selectServer($this->manager, $options));
    }

    public function findOneAndDelete(array|object $filter, array $options = []): array|object|null
    {
        $options = $this->writeOptions($options, true);
        return (new FindOneAndDelete($this->databaseName, $this->collectionName, $filter, $options))->execute($this->primary($options));
    }

    public function findOneAndReplace(array|object $filter, array|object $replacement, array $options = []): array|object|null
    {
        $options = $this->writeOptions($options, true);
        return (new FindOneAndReplace($this->databaseName, $this->collectionName, $filter, $replacement, $options))->execute($this->primary($options));
    }

    public function findOneAndUpdate(array|object $filter, array|object $update, array $options = []): array|object|null
    {
        $options = $this->writeOptions($options, true);
        return (new FindOneAndUpdate($this->databaseName, $this->collectionName, $filter, $update, $options))->execute($this->primary($options));
    }

    public function getCollectionName(): string { return $this->collectionName; }
    public function getDatabaseName(): string { return $this->databaseName; }
    public function getManager(): Manager { return $this->manager; }
    public function getNamespace(): string { return $this->databaseName . '.' . $this->collectionName; }
    public function getReadConcern(): ReadConcern { return $this->readConcern; }
    public function getReadPreference(): ReadPreference { return $this->readPreference; }
    public function getTypeMap(): array { return $this->typeMap; }
    public function getWriteConcern(): WriteConcern { return $this->writeConcern; }

    public function insertMany(array $documents, array $options = []): InsertManyResult
    {
        $options = $this->writeOptions($options);
        return (new InsertMany($this->databaseName, $this->collectionName, $documents, $options))->execute($this->primary($options));
    }

    public function insertOne(array|object $document, array $options = []): InsertOneResult
    {
        $options = $this->writeOptions($options);
        return (new InsertOne($this->databaseName, $this->collectionName, $document, $options))->execute($this->primary($options));
    }

    public function listIndexes(array $options = []): IndexInfoIterator
    {
        return (new ListIndexes($this->databaseName, $this->collectionName, $options))->execute($this->primary($options));
    }

    public function listSearchIndexes(array $options = []): \Iterator
    {
        $options = $this->readOptions($options);
        return (new ListSearchIndexes($this->databaseName, $this->collectionName, $options))->execute(Functions::selectServer($this->manager, $options));
    }

    public function rename(string $toCollectionName, ?string $toDatabaseName = null, array $options = []): array|object
    {
        $options = $this->writeOptions($options, true);
        return (new RenameCollection($this->databaseName, $this->collectionName, $toDatabaseName ?? $this->databaseName, $toCollectionName, $options))
            ->execute($this->primary($options));
    }

    public function replaceOne(array|object $filter, array|object $replacement, array $options = []): UpdateResult
    {
        $options = $this->writeOptions($options);
        return (new ReplaceOne($this->databaseName, $this->collectionName, $filter, $replacement, $options))->execute($this->primary($options));
    }

    public function updateMany(array|object $filter, array|object $update, array $options = []): UpdateResult
    {
        $options = $this->writeOptions($options);
        return (new UpdateMany($this->databaseName, $this->collectionName, $filter, $update, $options))->execute($this->primary($options));
    }

    public function updateOne(array|object $filter, array|object $update, array $options = []): UpdateResult
    {
        $options = $this->writeOptions($options);
        return (new UpdateOne($this->databaseName, $this->collectionName, $filter, $update, $options))->execute($this->primary($options));
    }

    public function updateSearchIndex(string $name, array|object $definition, array $options = []): void
    {
        (new UpdateSearchIndex($this->databaseName, $this->collectionName, $name, $definition, $options))->execute($this->primary($options));
    }

    public function watch(array $pipeline = [], array $options = []): ChangeStream
    {
        $options = $this->readOptions($options);
        if (isset($options['readConcern']) && $options['readConcern']->isDefault()) unset($options['readConcern']);
        $server = Functions::selectServer($this->manager, $options);
        return (new Watch($this->manager, $this->databaseName, $this->collectionName, $pipeline, $options))->execute($server);
    }

    public function withOptions(array $options = []): static
    {
        $options += ['readConcern' => $this->readConcern, 'readPreference' => $this->readPreference, 'typeMap' => $this->typeMap,
            'writeConcern' => $this->writeConcern];
        return new static($this->manager, $this->databaseName, $this->collectionName, $options);
    }
}
