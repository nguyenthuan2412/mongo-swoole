<?php
declare(strict_types=1);

namespace SwooleMongo;

use SwooleMongo\Driver\WriteResult;
use SwooleMongo\Exception\BadMethodCallException;

class InsertOneResult
{
    public function __construct(private WriteResult $writeResult, private mixed $insertedId)
    {
    }

    public function getInsertedCount(): int
    {
        if ($this->writeResult->isAcknowledged()) return (int) $this->writeResult->getInsertedCount();
        throw BadMethodCallException::unacknowledgedWriteResultAccess(__METHOD__);
    }

    public function getInsertedId(): mixed { return $this->insertedId; }
    public function isAcknowledged(): bool { return $this->writeResult->isAcknowledged(); }
}

class InsertManyResult
{
    public function __construct(private WriteResult $writeResult, private array $insertedIds)
    {
    }

    public function getInsertedCount(): int
    {
        if ($this->writeResult->isAcknowledged()) return (int) $this->writeResult->getInsertedCount();
        throw BadMethodCallException::unacknowledgedWriteResultAccess(__METHOD__);
    }

    public function getInsertedIds(): array { return $this->insertedIds; }
    public function isAcknowledged(): bool { return $this->writeResult->isAcknowledged(); }
}

class UpdateResult
{
    public function __construct(private WriteResult $writeResult)
    {
    }

    public function getMatchedCount(): int
    {
        if ($this->writeResult->isAcknowledged()) return (int) $this->writeResult->getMatchedCount();
        throw BadMethodCallException::unacknowledgedWriteResultAccess(__METHOD__);
    }

    public function getModifiedCount(): int
    {
        if ($this->writeResult->isAcknowledged()) return (int) $this->writeResult->getModifiedCount();
        throw BadMethodCallException::unacknowledgedWriteResultAccess(__METHOD__);
    }

    public function getUpsertedCount(): int
    {
        if ($this->writeResult->isAcknowledged()) return (int) $this->writeResult->getUpsertedCount();
        throw BadMethodCallException::unacknowledgedWriteResultAccess(__METHOD__);
    }

    public function getUpsertedId(): mixed
    {
        if ($this->writeResult->isAcknowledged()) {
            foreach ($this->writeResult->getUpsertedIds() as $id) return $id;
            return null;
        }
        throw BadMethodCallException::unacknowledgedWriteResultAccess(__METHOD__);
    }

    public function isAcknowledged(): bool { return $this->writeResult->isAcknowledged(); }
}

class DeleteResult
{
    public function __construct(private WriteResult $writeResult)
    {
    }

    public function getDeletedCount(): int
    {
        if ($this->writeResult->isAcknowledged()) return (int) $this->writeResult->getDeletedCount();
        throw BadMethodCallException::unacknowledgedWriteResultAccess(__METHOD__);
    }

    public function isAcknowledged(): bool { return $this->writeResult->isAcknowledged(); }
}

class BulkWriteResult
{
    public function __construct(private WriteResult $writeResult, private array $insertedIds)
    {
    }

    private function ack(string $method): void
    {
        if (!$this->writeResult->isAcknowledged()) throw BadMethodCallException::unacknowledgedWriteResultAccess($method);
    }

    public function getDeletedCount(): int { $this->ack(__METHOD__); return (int) $this->writeResult->getDeletedCount(); }
    public function getInsertedCount(): int { $this->ack(__METHOD__); return (int) $this->writeResult->getInsertedCount(); }
    public function getInsertedIds(): array { return $this->insertedIds; }
    public function getMatchedCount(): int { $this->ack(__METHOD__); return (int) $this->writeResult->getMatchedCount(); }
    public function getModifiedCount(): int { $this->ack(__METHOD__); return (int) $this->writeResult->getModifiedCount(); }
    public function getUpsertedCount(): int { $this->ack(__METHOD__); return (int) $this->writeResult->getUpsertedCount(); }
    public function getUpsertedIds(): array { $this->ack(__METHOD__); return $this->writeResult->getUpsertedIds(); }
    public function isAcknowledged(): bool { return $this->writeResult->isAcknowledged(); }
}
