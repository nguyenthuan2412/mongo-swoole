<?php
declare(strict_types=1);

namespace SwooleMongo;

use SwooleMongo\BSON\Document;
use SwooleMongo\BSON\Int64;
use SwooleMongo\Driver\Command;
use SwooleMongo\Driver\Cursor;
use SwooleMongo\Driver\Exception\ConnectionException;
use SwooleMongo\Driver\Exception\RuntimeException as DriverRuntimeException;
use SwooleMongo\Driver\Exception\ServerException;
use SwooleMongo\Driver\Manager;
use SwooleMongo\Driver\Server;
use SwooleMongo\Exception\ResumeTokenException;

/**
 * Iterator over a change stream (mirrors MongoDB\ChangeStream).
 *
 *   $stream = $collection->watch();
 *   for ($stream->rewind(); true; $stream->next()) {
 *       if (!$stream->valid()) continue;          // nothing within maxAwaitTimeMS
 *       $event = $stream->current();
 *   }
 *
 * Resumes automatically after network errors and resumable server errors.
 */
class ChangeStream implements \Iterator
{
    public const CURSOR_NOT_FOUND = 43;

    private const RESUMABLE_CODES = [6, 7, 63, 89, 91, 133, 150, 189, 234, 262, 9001, 10107, 11600, 11602, 13388, 13435, 13436];

    private ?Cursor $cursor = null;
    private array|object|null $resumeToken = null;
    private array|object|null $current = null;
    private ?int $key = null;
    private int $counter = 0;
    private bool $hasAdvanced = false;
    private bool $returnedAny = false;
    private ?string $serverAddress = null;
    private array $typeMap;

    /** @internal use Collection::watch(), Database::watch() or Client::watch() */
    public function __construct(
        private Manager $manager,
        private ?string $databaseName,
        private ?string $collectionName,
        private array $pipeline,
        private array $options,
        Server $server,
    ) {
        $this->typeMap = $options['typeMap'] ?? ['root' => 'array', 'document' => 'array', 'array' => 'array'];
        if (isset($options['startAfter'])) $this->resumeToken = $options['startAfter'];
        elseif (isset($options['resumeAfter'])) $this->resumeToken = $options['resumeAfter'];
        $this->createCursor($server);
    }

    private function createCursor(?Server $server = null): void
    {
        $stage = [];
        foreach (['fullDocument', 'fullDocumentBeforeChange', 'showExpandedEvents'] as $k) {
            if (isset($this->options[$k])) $stage[$k] = $this->options[$k];
        }
        if ($this->databaseName === null) $stage['allChangesForCluster'] = true;
        if ($this->resumeToken !== null) {
            if (isset($this->options['startAfter']) && !$this->returnedAny) $stage['startAfter'] = $this->resumeToken;
            else $stage['resumeAfter'] = $this->resumeToken;
        } elseif (isset($this->options['startAtOperationTime'])) {
            $stage['startAtOperationTime'] = $this->options['startAtOperationTime'];
        }
        $cmd = [
            'aggregate' => $this->collectionName ?? 1,
            'pipeline' => array_merge([['$changeStream' => (object) $stage]], $this->pipeline),
            'cursor' => isset($this->options['batchSize']) ? ['batchSize' => $this->options['batchSize']] : new \stdClass(),
        ];
        foreach (['collation', 'comment'] as $k) {
            if (isset($this->options[$k])) $cmd[$k] = $this->options[$k];
        }
        $execOptions = [];
        foreach (['readConcern', 'readPreference', 'session'] as $k) {
            if (isset($this->options[$k])) $execOptions[$k] = $this->options[$k];
        }
        if (isset($execOptions['readConcern']) && $execOptions['readConcern']->isDefault()) unset($execOptions['readConcern']);
        $server ??= Functions::selectServer($this->manager, $execOptions);
        $command = new Command($cmd, ['maxAwaitTimeMS' => $this->options['maxAwaitTimeMS'] ?? 1000]);
        $this->cursor = $server->executeReadCommand($this->databaseName ?? 'admin', $command, $execOptions);
        $this->cursor->setTypeMap($this->typeMap);
        $this->serverAddress = $server->getHost() . ':' . $server->getPort();
        $this->hasAdvanced = false;
        $this->current = null;
    }

    private static function isResumable(\Throwable $e): bool
    {
        if ($e instanceof ConnectionException) return true;
        if ($e instanceof ServerException) {
            return $e->hasErrorLabel('ResumableChangeStreamError') || in_array($e->getCode(), self::RESUMABLE_CODES, true)
                || $e->getCode() === self::CURSOR_NOT_FOUND;
        }
        return false;
    }

    private function extractResumeToken(array|object $document): array|object
    {
        if ($document instanceof Document) {
            $token = $document->has('_id') ? $document->get('_id') : null;
        } elseif (is_array($document) || $document instanceof \ArrayAccess) {
            $token = $document['_id'] ?? null;
        } else {
            $token = $document->_id ?? null;
        }
        if ($token === null) throw ResumeTokenException::notFound();
        if (!is_array($token) && !is_object($token)) throw ResumeTokenException::invalidType($token);
        return $token;
    }

    private function onIteration(bool $incrementKey): void
    {
        if (!$this->cursor->valid()) {
            $this->current = null;
            $pbrt = $this->cursor->getPostBatchResumeToken();
            if ($pbrt !== null) $this->resumeToken = $pbrt;
            return;
        }
        $this->current = $this->cursor->current();
        $this->resumeToken = $this->extractResumeToken($this->current);
        if ($this->cursor->bufferedCount() <= 1 && ($pbrt = $this->cursor->getPostBatchResumeToken()) !== null) {
            $this->resumeToken = $pbrt;
        }
        if ($this->returnedAny) $this->counter++;
        $this->key = $this->counter;
        $this->returnedAny = true;
    }

    private function withResume(callable $step): void
    {
        for ($attempt = 0; ; $attempt++) {
            try {
                $step();
                return;
            } catch (DriverRuntimeException $e) {
                if ($attempt > 0 || !self::isResumable($e)) throw $e;
                try { $this->cursor?->kill(); } catch (\Throwable) {}
                $this->createCursor();
                $this->cursor->rewind();
                $this->onIteration($this->returnedAny);
                if ($this->cursor->valid()) return;
                $step = fn () => null;
            }
        }
    }

    public function rewind(): void
    {
        $this->withResume(function () {
            if ($this->hasAdvanced) return;
            $this->cursor->rewind();
            $this->onIteration(false);
        });
    }

    public function next(): void
    {
        $this->withResume(function () {
            $this->hasAdvanced = true;
            $wasValid = $this->cursor->valid();
            $this->cursor->next();
            $this->onIteration($wasValid);
        });
    }

    public function valid(): bool
    {
        return $this->current !== null;
    }

    public function current(): array|object|null
    {
        return $this->current;
    }

    public function key(): ?int
    {
        return $this->current !== null ? $this->key : null;
    }

    public function getCursorId(): Int64
    {
        return $this->cursor->getId();
    }

    public function getResumeToken(): array|object|null
    {
        return $this->resumeToken;
    }
}
