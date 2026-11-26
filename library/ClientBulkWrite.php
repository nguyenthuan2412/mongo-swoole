<?php
declare(strict_types=1);

namespace SwooleMongo;

use SwooleMongo\Driver\BulkWriteCommand;
use SwooleMongo\Driver\Manager;
use SwooleMongo\Exception\InvalidArgumentException;

/**
 * Builder for the MongoDB 8.0+ client-level bulkWrite command (mirrors MongoDB\ClientBulkWrite).
 *
 *   $bulk = ClientBulkWrite::createWithCollection($users, ['verboseResults' => true])
 *       ->insertOne(['name' => 'a'], $id)
 *       ->withCollection($orders)->updateMany(['paid' => false], ['$set' => ['due' => true]]);
 *   $result = $client->bulkWrite($bulk);
 */
final class ClientBulkWrite
{
    private function __construct(
        public readonly BulkWriteCommand $bulkWriteCommand,
        private Manager $manager,
        private string $namespace,
    ) {
    }

    public static function createWithCollection(Collection $collection, array $options = []): self
    {
        $options += ['ordered' => true];
        return new self(new BulkWriteCommand($options), $collection->getManager(), $collection->getNamespace());
    }

    public function withCollection(Collection $collection): self
    {
        if ($collection->getManager() !== $this->manager) {
            throw new InvalidArgumentException('$collection was not created with the same MongoDB\Driver\Manager used to create this ClientBulkWrite');
        }
        $clone = clone $this;
        $clone->namespace = $collection->getNamespace();
        return $clone;
    }

    public function deleteOne(array|object $filter, array $options = []): self
    {
        $this->bulkWriteCommand->deleteOne($this->namespace, $filter, $options);
        return $this;
    }

    public function deleteMany(array|object $filter, array $options = []): self
    {
        $this->bulkWriteCommand->deleteMany($this->namespace, $filter, $options);
        return $this;
    }

    public function insertOne(array|object $document, mixed &$id = null): self
    {
        $id = $this->bulkWriteCommand->insertOne($this->namespace, $document);
        return $this;
    }

    public function replaceOne(array|object $filter, array|object $replacement, array $options = []): self
    {
        if (Functions::isFirstKeyOperator($replacement)) throw new InvalidArgumentException('First key in $replacement is an update operator');
        $this->bulkWriteCommand->replaceOne($this->namespace, $filter, $replacement, $options);
        return $this;
    }

    public function updateOne(array|object $filter, array|object $update, array $options = []): self
    {
        if (!Functions::isFirstKeyOperator($update) && !Functions::isPipeline($update)) {
            throw new InvalidArgumentException('Expected update operator(s) or non-empty pipeline for $update');
        }
        $this->bulkWriteCommand->updateOne($this->namespace, $filter, $update, $options);
        return $this;
    }

    public function updateMany(array|object $filter, array|object $update, array $options = []): self
    {
        if (!Functions::isFirstKeyOperator($update) && !Functions::isPipeline($update)) {
            throw new InvalidArgumentException('Expected update operator(s) or non-empty pipeline for $update');
        }
        $this->bulkWriteCommand->updateMany($this->namespace, $filter, $update, $options);
        return $this;
    }

    /** @internal */
    public function getManager(): Manager
    {
        return $this->manager;
    }
}
