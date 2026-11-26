<?php
declare(strict_types=1);

namespace SwooleMongo\Operation;

use SwooleMongo\BSON\Document;
use SwooleMongo\BSON\PackedArray;
use SwooleMongo\ChangeStream;
use SwooleMongo\DeleteResult;
use SwooleMongo\Driver\BulkWrite as Bulk;
use SwooleMongo\Driver\Command;
use SwooleMongo\Driver\CursorInterface;
use SwooleMongo\Driver\Exception\CommandException;
use SwooleMongo\Driver\Manager;
use SwooleMongo\Driver\Query;
use SwooleMongo\Driver\ReadConcern;
use SwooleMongo\Driver\ReadPreference;
use SwooleMongo\Driver\Server;
use SwooleMongo\Driver\Session;
use SwooleMongo\Driver\WriteConcern;
use SwooleMongo\Exception\InvalidArgumentException;
use SwooleMongo\Exception\UnexpectedValueException;
use SwooleMongo\Exception\UnsupportedException;
use SwooleMongo\Functions;
use SwooleMongo\InsertManyResult;
use SwooleMongo\InsertOneResult;
use SwooleMongo\BulkWriteResult;
use SwooleMongo\Model\CachingIterator;
use SwooleMongo\Model\CallbackIterator;
use SwooleMongo\Model\CollectionInfoCommandIterator;
use SwooleMongo\Model\CollectionInfoIterator;
use SwooleMongo\Model\DatabaseInfoIterator;
use SwooleMongo\Model\DatabaseInfoLegacyIterator;
use SwooleMongo\Model\IndexInfoIterator;
use SwooleMongo\Model\IndexInfoIteratorIterator;
use SwooleMongo\Model\IndexInput;
use SwooleMongo\UpdateResult;

interface Executable
{
    public function execute(Server $server): mixed;
}

interface Explainable extends Executable
{
    public function getCommandDocument(): array;
}

/** @internal shared option handling for operations */
abstract class AbstractOperation
{
    protected array $options;

    protected static function checkType(array $options, string $key, string|array $types): void
    {
        if (!isset($options[$key])) return;
        $v = $options[$key];
        foreach ((array) $types as $t) {
            $ok = match ($t) {
                'bool' => is_bool($v), 'int' => is_int($v), 'string' => is_string($v), 'array' => is_array($v),
                'document' => is_array($v) || is_object($v), 'callable' => is_callable($v),
                default => $v instanceof $t,
            };
            if ($ok) return;
        }
        throw InvalidArgumentException::invalidType('"' . $key . '" option', $v, $types);
    }

    protected static function checkCommon(array $options): void
    {
        self::checkType($options, 'session', Session::class);
        self::checkType($options, 'readConcern', ReadConcern::class);
        self::checkType($options, 'readPreference', ReadPreference::class);
        self::checkType($options, 'writeConcern', WriteConcern::class);
        self::checkType($options, 'typeMap', 'array');
        self::checkType($options, 'maxTimeMS', 'int');
        self::checkType($options, 'collation', 'document');
        self::checkType($options, 'let', 'document');
    }

    /** removes default concerns and validates transaction restrictions */
    protected static function normalizeConcerns(array $options): array
    {
        if (isset($options['readConcern']) && $options['readConcern']->isDefault()) unset($options['readConcern']);
        if (isset($options['writeConcern']) && $options['writeConcern']->isDefault()) unset($options['writeConcern']);
        if (Functions::isInTransaction($options)) {
            if (isset($options['readConcern'])) throw UnsupportedException::readConcernNotSupportedInTransaction();
            if (isset($options['writeConcern'])) throw UnsupportedException::writeConcernNotSupportedInTransaction();
        }
        return $options;
    }

    protected function executeOptions(array $keys): array
    {
        $o = [];
        $keys[] = 'timeoutMS';
        foreach ($keys as $k) if (isset($this->options[$k])) $o[$k] = $this->options[$k];
        return $o;
    }

    protected static function copy(array $from, array &$to, array $keys): void
    {
        foreach ($keys as $src => $dst) {
            if (is_int($src)) $src = $dst;
            if (isset($from[$src])) $to[$dst] = $from[$src];
        }
    }
}

/* ===================================================================== CRUD */

final class InsertOne extends AbstractOperation implements Executable
{
    public function __construct(private string $databaseName, private string $collectionName, private array|object $document, array $options = [])
    {
        self::checkCommon($options);
        self::checkType($options, 'bypassDocumentValidation', 'bool');
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): InsertOneResult
    {
        $bulkOptions = [];
        self::copy($this->options, $bulkOptions, ['bypassDocumentValidation', 'comment']);
        $bulk = new Bulk($bulkOptions);
        $insertedId = $bulk->insert($this->document);
        $writeResult = $server->executeBulkWrite($this->databaseName . '.' . $this->collectionName, $bulk, $this->executeOptions(['session', 'writeConcern']));
        return new InsertOneResult($writeResult, $insertedId);
    }
}

final class InsertMany extends AbstractOperation implements Executable
{
    private array $documents;

    public function __construct(private string $databaseName, private string $collectionName, array $documents, array $options = [])
    {
        if ($documents === []) throw new InvalidArgumentException('$documents is empty');
        if (!array_is_list($documents)) throw new InvalidArgumentException('$documents is not a list');
        foreach ($documents as $i => $d) {
            if (!is_array($d) && !is_object($d)) throw InvalidArgumentException::expectedDocumentType(sprintf('$documents[%d]', $i), $d);
        }
        self::checkCommon($options);
        self::checkType($options, 'ordered', 'bool');
        self::checkType($options, 'bypassDocumentValidation', 'bool');
        $options += ['ordered' => true];
        $this->documents = $documents;
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): InsertManyResult
    {
        $bulkOptions = ['ordered' => $this->options['ordered']];
        self::copy($this->options, $bulkOptions, ['bypassDocumentValidation', 'comment']);
        $bulk = new Bulk($bulkOptions);
        $insertedIds = [];
        foreach ($this->documents as $i => $document) {
            $insertedIds[$i] = $bulk->insert($document);
        }
        $writeResult = $server->executeBulkWrite($this->databaseName . '.' . $this->collectionName, $bulk, $this->executeOptions(['session', 'writeConcern']));
        return new InsertManyResult($writeResult, $insertedIds);
    }
}

/** @internal */
class Update extends AbstractOperation implements Executable
{
    public function __construct(private string $databaseName, private string $collectionName, private array|object $filter, private array|object $update, array $options = [])
    {
        self::checkCommon($options);
        self::checkType($options, 'arrayFilters', 'array');
        self::checkType($options, 'bypassDocumentValidation', 'bool');
        self::checkType($options, 'hint', ['string', 'document']);
        self::checkType($options, 'multi', 'bool');
        self::checkType($options, 'upsert', 'bool');
        $options += ['multi' => false, 'upsert' => false];
        if ($options['multi'] && !Functions::isFirstKeyOperator($update) && !Functions::isPipeline($update)) {
            throw new InvalidArgumentException('"multi" option cannot be true if $update is a replacement document');
        }
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): UpdateResult
    {
        $bulkOptions = [];
        self::copy($this->options, $bulkOptions, ['bypassDocumentValidation', 'comment', 'let']);
        $updateOptions = [];
        self::copy($this->options, $updateOptions, ['arrayFilters', 'collation', 'hint', 'multi', 'upsert', 'sort']);
        $bulk = new Bulk($bulkOptions);
        $bulk->update($this->filter, $this->update, $updateOptions);
        $writeResult = $server->executeBulkWrite($this->databaseName . '.' . $this->collectionName, $bulk, $this->executeOptions(['session', 'writeConcern']));
        return new UpdateResult($writeResult);
    }
}

final class UpdateOne implements Executable
{
    private Update $update;

    public function __construct(string $databaseName, string $collectionName, array|object $filter, array|object $update, array $options = [])
    {
        if (!Functions::isFirstKeyOperator($update) && !Functions::isPipeline($update)) {
            throw new InvalidArgumentException('Expected update operator(s) or non-empty pipeline for $update');
        }
        $this->update = new Update($databaseName, $collectionName, $filter, $update, ['multi' => false] + $options);
    }

    public function execute(Server $server): UpdateResult { return $this->update->execute($server); }
}

final class UpdateMany implements Executable
{
    private Update $update;

    public function __construct(string $databaseName, string $collectionName, array|object $filter, array|object $update, array $options = [])
    {
        if (!Functions::isFirstKeyOperator($update) && !Functions::isPipeline($update)) {
            throw new InvalidArgumentException('Expected update operator(s) or non-empty pipeline for $update');
        }
        $this->update = new Update($databaseName, $collectionName, $filter, $update, ['multi' => true] + $options);
    }

    public function execute(Server $server): UpdateResult { return $this->update->execute($server); }
}

final class ReplaceOne implements Executable
{
    private Update $update;

    public function __construct(string $databaseName, string $collectionName, array|object $filter, array|object $replacement, array $options = [])
    {
        if (Functions::isFirstKeyOperator($replacement)) throw new InvalidArgumentException('First key in $replacement is an update operator');
        if (Functions::isPipeline($replacement, true)) throw new InvalidArgumentException('$replacement is an update pipeline');
        $this->update = new Update($databaseName, $collectionName, $filter, $replacement, ['multi' => false] + $options);
    }

    public function execute(Server $server): UpdateResult { return $this->update->execute($server); }
}

/** @internal */
class Delete extends AbstractOperation implements Executable, Explainable
{
    public function __construct(private string $databaseName, private string $collectionName, private array|object $filter, private int $limit, array $options = [])
    {
        if ($limit !== 0 && $limit !== 1) throw new InvalidArgumentException('$limit must be 0 or 1');
        self::checkCommon($options);
        self::checkType($options, 'hint', ['string', 'document']);
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): DeleteResult
    {
        $bulkOptions = [];
        self::copy($this->options, $bulkOptions, ['comment', 'let']);
        $deleteOptions = ['limit' => $this->limit === 1];
        self::copy($this->options, $deleteOptions, ['collation', 'hint']);
        $bulk = new Bulk($bulkOptions);
        $bulk->delete($this->filter, $deleteOptions);
        $writeResult = $server->executeBulkWrite($this->databaseName . '.' . $this->collectionName, $bulk, $this->executeOptions(['session', 'writeConcern']));
        return new DeleteResult($writeResult);
    }

    public function getCommandDocument(): array
    {
        $delete = ['q' => (object) Functions::documentToArray($this->filter), 'limit' => $this->limit];
        self::copy($this->options, $delete, ['collation', 'hint']);
        $cmd = ['delete' => $this->collectionName, 'deletes' => [$delete]];
        self::copy($this->options, $cmd, ['comment', 'let']);
        return $cmd;
    }
}

final class DeleteOne implements Executable, Explainable
{
    private Delete $delete;

    public function __construct(string $databaseName, string $collectionName, array|object $filter, array $options = [])
    {
        $this->delete = new Delete($databaseName, $collectionName, $filter, 1, $options);
    }

    public function execute(Server $server): DeleteResult { return $this->delete->execute($server); }
    public function getCommandDocument(): array { return $this->delete->getCommandDocument(); }
}

final class DeleteMany implements Executable, Explainable
{
    private Delete $delete;

    public function __construct(string $databaseName, string $collectionName, array|object $filter, array $options = [])
    {
        $this->delete = new Delete($databaseName, $collectionName, $filter, 0, $options);
    }

    public function execute(Server $server): DeleteResult { return $this->delete->execute($server); }
    public function getCommandDocument(): array { return $this->delete->getCommandDocument(); }
}

final class BulkWrite extends AbstractOperation implements Executable
{
    public const DELETE_MANY = 'deleteMany';
    public const DELETE_ONE = 'deleteOne';
    public const INSERT_ONE = 'insertOne';
    public const REPLACE_ONE = 'replaceOne';
    public const UPDATE_MANY = 'updateMany';
    public const UPDATE_ONE = 'updateOne';

    private bool $isArrayFiltersUsed = false;

    /** @param list<array<string, array>> $operations e.g. [['insertOne' => [$doc]], ['updateOne' => [$filter, $update, $options]]] */
    public function __construct(private string $databaseName, private string $collectionName, private array $operations, array $options = [])
    {
        if ($operations === []) throw new InvalidArgumentException('$operations is empty');
        if (!array_is_list($operations)) throw new InvalidArgumentException('$operations is not a list');
        foreach ($operations as $i => $operation) {
            if (!is_array($operation)) throw InvalidArgumentException::invalidType(sprintf('$operations[%d]', $i), $operation, 'array');
            if (count($operation) !== 1) throw new InvalidArgumentException(sprintf('Expected one element in $operation[%d], actually: %d', $i, count($operation)));
            $type = key($operation);
            $args = current($operation);
            if (!is_array($args)) throw InvalidArgumentException::invalidType(sprintf('$operations[%d]["%s"]', $i, $type), $args, 'array');
            switch ($type) {
                case self::INSERT_ONE:
                    if (!isset($args[0])) throw new InvalidArgumentException(sprintf('Missing $operations[%d]["%s"][0]', $i, $type));
                    break;
                case self::DELETE_MANY:
                case self::DELETE_ONE:
                    if (!isset($args[0])) throw new InvalidArgumentException(sprintf('Missing $operations[%d]["%s"][0]', $i, $type));
                    break;
                case self::REPLACE_ONE:
                    if (!isset($args[1])) throw new InvalidArgumentException(sprintf('Missing $operations[%d]["%s"][1]', $i, $type));
                    if (Functions::isFirstKeyOperator($args[1])) throw new InvalidArgumentException(sprintf('First key in $operations[%d]["%s"][1] is an update operator', $i, $type));
                    break;
                case self::UPDATE_MANY:
                case self::UPDATE_ONE:
                    if (!isset($args[1])) throw new InvalidArgumentException(sprintf('Missing $operations[%d]["%s"][1]', $i, $type));
                    if (!Functions::isFirstKeyOperator($args[1]) && !Functions::isPipeline($args[1])) {
                        throw new InvalidArgumentException(sprintf('Expected update operator(s) or non-empty pipeline for $operations[%d]["%s"][1]', $i, $type));
                    }
                    if (isset($args[2]['arrayFilters'])) $this->isArrayFiltersUsed = true;
                    break;
                default:
                    throw new InvalidArgumentException(sprintf('Unknown operation type "%s" in $operations[%d]', $type, $i));
            }
        }
        self::checkCommon($options);
        self::checkType($options, 'ordered', 'bool');
        self::checkType($options, 'bypassDocumentValidation', 'bool');
        $options += ['ordered' => true];
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): BulkWriteResult
    {
        $bulkOptions = ['ordered' => $this->options['ordered']];
        self::copy($this->options, $bulkOptions, ['bypassDocumentValidation', 'comment', 'let']);
        $bulk = new Bulk($bulkOptions);
        $insertedIds = [];
        foreach ($this->operations as $i => $operation) {
            $type = key($operation);
            $args = current($operation);
            switch ($type) {
                case self::DELETE_MANY:
                case self::DELETE_ONE:
                    $o = $args[1] ?? [];
                    $o['limit'] = $type === self::DELETE_ONE;
                    $bulk->delete($args[0], $o);
                    break;
                case self::INSERT_ONE:
                    $insertedIds[$i] = $bulk->insert($args[0]);
                    break;
                case self::REPLACE_ONE:
                case self::UPDATE_MANY:
                case self::UPDATE_ONE:
                    $o = ($args[2] ?? []) + ['upsert' => false];
                    $o['multi'] = $type === self::UPDATE_MANY;
                    $bulk->update($args[0], $args[1], $o);
                    break;
            }
        }
        $writeResult = $server->executeBulkWrite($this->databaseName . '.' . $this->collectionName, $bulk, $this->executeOptions(['session', 'writeConcern']));
        return new BulkWriteResult($writeResult, $insertedIds);
    }
}

/* ===================================================================== reads */

final class Find extends AbstractOperation implements Executable, Explainable
{
    public const NON_TAILABLE = 1;
    public const TAILABLE = 2;
    public const TAILABLE_AWAIT = 3;

    public function __construct(private string $databaseName, private string $collectionName, private array|object $filter, array $options = [])
    {
        self::checkCommon($options);
        foreach (['allowDiskUse', 'allowPartialResults', 'noCursorTimeout', 'oplogReplay', 'returnKey', 'showRecordId', 'singleBatch'] as $b) {
            self::checkType($options, $b, 'bool');
        }
        foreach (['batchSize', 'limit', 'skip', 'maxAwaitTimeMS', 'cursorType'] as $i) {
            self::checkType($options, $i, 'int');
        }
        foreach (['projection', 'sort', 'max', 'min'] as $d) {
            self::checkType($options, $d, 'document');
        }
        self::checkType($options, 'hint', ['string', 'document']);
        if (isset($options['cursorType']) && !in_array($options['cursorType'], [self::NON_TAILABLE, self::TAILABLE, self::TAILABLE_AWAIT], true)) {
            throw new InvalidArgumentException('Invalid value for "cursorType" option: ' . $options['cursorType']);
        }
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): CursorInterface
    {
        $cursor = $server->executeQuery($this->databaseName . '.' . $this->collectionName, new Query($this->filter, $this->createQueryOptions()),
            $this->executeOptions(['readPreference', 'session']));
        if (isset($this->options['typeMap'])) $cursor->setTypeMap($this->options['typeMap']);
        return $cursor;
    }

    public function getCommandDocument(): array
    {
        $cmd = ['find' => $this->collectionName, 'filter' => (object) Functions::documentToArray($this->filter)];
        $o = $this->createQueryOptions();
        if (isset($o['limit']) && $o['limit'] < 0) { $o['limit'] = -$o['limit']; $o['singleBatch'] = true; }
        unset($o['maxAwaitTimeMS'], $o['readConcern']);
        foreach ($o as $k => $v) $cmd[$k] = $v;
        return $cmd;
    }

    private function createQueryOptions(): array
    {
        $o = [];
        if (isset($this->options['cursorType'])) {
            if ($this->options['cursorType'] === self::TAILABLE) $o['tailable'] = true;
            if ($this->options['cursorType'] === self::TAILABLE_AWAIT) { $o['tailable'] = true; $o['awaitData'] = true; }
        }
        self::copy($this->options, $o, ['allowDiskUse', 'allowPartialResults', 'batchSize', 'collation', 'comment', 'hint', 'let', 'limit',
            'max', 'maxAwaitTimeMS', 'maxTimeMS', 'min', 'noCursorTimeout', 'oplogReplay', 'projection', 'readConcern', 'returnKey',
            'showRecordId', 'singleBatch', 'skip', 'sort']);
        if (!empty($this->options['modifiers'])) {
            $map = ['$comment' => 'comment', '$hint' => 'hint', '$max' => 'max', '$maxTimeMS' => 'maxTimeMS', '$min' => 'min',
                    '$orderby' => 'sort', '$returnKey' => 'returnKey', '$showDiskLoc' => 'showRecordId'];
            foreach (Functions::documentToArray($this->options['modifiers']) as $k => $v) {
                if (isset($map[$k]) && !isset($o[$map[$k]])) $o[$map[$k]] = $v;
            }
        }
        return $o;
    }
}

final class FindOne implements Executable, Explainable
{
    private Find $find;

    public function __construct(string $databaseName, string $collectionName, array|object $filter, array $options = [])
    {
        $this->find = new Find($databaseName, $collectionName, $filter, ['limit' => 1, 'singleBatch' => true] + $options);
    }

    public function execute(Server $server): array|object|null
    {
        $cursor = $this->find->execute($server);
        $document = current($cursor->toArray());
        return $document === false ? null : $document;
    }

    public function getCommandDocument(): array { return $this->find->getCommandDocument(); }
}

final class Aggregate extends AbstractOperation implements Executable, Explainable
{
    private bool $isWrite;

    public function __construct(private string $databaseName, private ?string $collectionName, private array $pipeline, array $options = [])
    {
        if (!array_is_list($pipeline)) throw new InvalidArgumentException('$pipeline is not a list');
        self::checkCommon($options);
        self::checkType($options, 'allowDiskUse', 'bool');
        self::checkType($options, 'batchSize', 'int');
        self::checkType($options, 'bypassDocumentValidation', 'bool');
        self::checkType($options, 'explain', 'bool');
        self::checkType($options, 'hint', ['string', 'document']);
        self::checkType($options, 'maxAwaitTimeMS', 'int');
        $this->isWrite = Functions::isLastPipelineOperatorWrite($pipeline) && empty($options['explain']);
        if (!$this->isWrite) unset($options['writeConcern']);
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): CursorInterface
    {
        $cmdOptions = [];
        if (isset($this->options['maxAwaitTimeMS'])) $cmdOptions['maxAwaitTimeMS'] = $this->options['maxAwaitTimeMS'];
        $command = new Command($this->createCommandDocument(), $cmdOptions);
        if ($this->isWrite) {
            $cursor = $server->executeReadWriteCommand($this->databaseName, $command, $this->executeOptions(['readConcern', 'writeConcern', 'session']));
        } else {
            $cursor = $server->executeReadCommand($this->databaseName, $command, $this->executeOptions(['readConcern', 'readPreference', 'session']));
        }
        if (isset($this->options['typeMap'])) $cursor->setTypeMap($this->options['typeMap']);
        return $cursor;
    }

    public function getCommandDocument(): array
    {
        $cmd = $this->createCommandDocument();
        unset($cmd['cursor']);
        return $cmd;
    }

    private function createCommandDocument(): array
    {
        $cmd = ['aggregate' => $this->collectionName ?? 1, 'pipeline' => $this->pipeline];
        self::copy($this->options, $cmd, ['allowDiskUse', 'bypassDocumentValidation', 'collation', 'comment', 'explain', 'hint', 'let', 'maxTimeMS']);
        if (isset($cmd['hint']) && is_array($cmd['hint'])) $cmd['hint'] = (object) $cmd['hint'];
        if (empty($this->options['explain'])) {
            $cmd['cursor'] = isset($this->options['batchSize']) ? ['batchSize' => $this->options['batchSize']] : new \stdClass();
        }
        return $cmd;
    }
}

final class CountDocuments extends AbstractOperation implements Executable
{
    private Aggregate $aggregate;

    public function __construct(string $databaseName, string $collectionName, array|object $filter, array $options = [])
    {
        self::checkType($options, 'limit', 'int');
        self::checkType($options, 'skip', 'int');
        $pipeline = [['$match' => (object) Functions::documentToArray($filter)]];
        if (isset($options['skip'])) $pipeline[] = ['$skip' => $options['skip']];
        if (isset($options['limit'])) $pipeline[] = ['$limit' => $options['limit']];
        $pipeline[] = ['$group' => ['_id' => 1, 'n' => ['$sum' => 1]]];
        $aggOptions = array_intersect_key($options, array_flip(['collation', 'comment', 'hint', 'maxTimeMS', 'readConcern', 'readPreference', 'session']));
        $aggOptions['typeMap'] = ['root' => 'array'];
        $this->aggregate = new Aggregate($databaseName, $collectionName, $pipeline, $aggOptions);
    }

    public function execute(Server $server): int
    {
        $all = $this->aggregate->execute($server)->toArray();
        if ($all === []) return 0;
        $r = current($all);
        if (!isset($r['n']) || !is_int($r['n'])) throw new UnexpectedValueException('countDocuments command did not return a numeric "n" value');
        return $r['n'];
    }
}

final class Count extends AbstractOperation implements Executable, Explainable
{
    public function __construct(private string $databaseName, private string $collectionName, private array|object $filter = [], array $options = [])
    {
        self::checkCommon($options);
        self::checkType($options, 'limit', 'int');
        self::checkType($options, 'skip', 'int');
        self::checkType($options, 'hint', ['string', 'document']);
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): int
    {
        $cursor = $server->executeReadCommand($this->databaseName, new Command($this->getCommandDocument()), $this->executeOptions(['readConcern', 'readPreference', 'session']));
        $cursor->setTypeMap(['root' => 'array']);
        $r = current($cursor->toArray());
        if (!isset($r['n']) || !(is_int($r['n']) || is_float($r['n']))) throw new UnexpectedValueException('count command did not return a numeric "n" value');
        return (int) $r['n'];
    }

    public function getCommandDocument(): array
    {
        $cmd = ['count' => $this->collectionName];
        $filter = Functions::documentToArray($this->filter);
        if ($filter !== []) $cmd['query'] = (object) $filter;
        self::copy($this->options, $cmd, ['collation', 'comment', 'hint', 'limit', 'maxTimeMS', 'skip']);
        return $cmd;
    }
}

final class EstimatedDocumentCount extends AbstractOperation implements Executable, Explainable
{
    public function __construct(private string $databaseName, private string $collectionName, array $options = [])
    {
        self::checkCommon($options);
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): int
    {
        try {
            return (new Count($this->databaseName, $this->collectionName, [], $this->options))->execute($server);
        } catch (CommandException $e) {
            if ($e->getCode() === 26) return 0;
            throw $e;
        }
    }

    public function getCommandDocument(): array
    {
        return (new Count($this->databaseName, $this->collectionName, [], $this->options))->getCommandDocument();
    }
}

final class Distinct extends AbstractOperation implements Executable, Explainable
{
    public function __construct(private string $databaseName, private string $collectionName, private string $fieldName, private array|object $filter = [], array $options = [])
    {
        self::checkCommon($options);
        self::checkType($options, 'hint', ['string', 'document']);
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): array
    {
        $cursor = $server->executeReadCommand($this->databaseName, new Command($this->getCommandDocument()), $this->executeOptions(['readConcern', 'readPreference', 'session']));
        $tm = $this->options['typeMap'] ?? [];
        $cursor->setTypeMap(['root' => 'array', 'array' => 'array'] + array_intersect_key($tm, ['document' => 1])
            + ['fieldPaths' => isset($tm['root']) ? ['values.$' => $tm['root']] : []]);
        $r = current($cursor->toArray());
        if (!isset($r['values']) || !is_array($r['values'])) throw new UnexpectedValueException('distinct command did not return a "values" array');
        return $r['values'];
    }

    public function getCommandDocument(): array
    {
        $cmd = ['distinct' => $this->collectionName, 'key' => $this->fieldName];
        $filter = Functions::documentToArray($this->filter);
        if ($filter !== []) $cmd['query'] = (object) $filter;
        self::copy($this->options, $cmd, ['collation', 'comment', 'hint', 'maxTimeMS']);
        return $cmd;
    }
}

/* ============================================================ findAndModify */

final class FindAndModify extends AbstractOperation implements Executable, Explainable
{
    public function __construct(private string $databaseName, private string $collectionName, array $options)
    {
        self::checkCommon($options);
        self::checkType($options, 'arrayFilters', 'array');
        self::checkType($options, 'bypassDocumentValidation', 'bool');
        self::checkType($options, 'fields', 'document');
        self::checkType($options, 'hint', ['string', 'document']);
        self::checkType($options, 'new', 'bool');
        self::checkType($options, 'query', 'document');
        self::checkType($options, 'remove', 'bool');
        self::checkType($options, 'sort', 'document');
        self::checkType($options, 'update', 'document');
        self::checkType($options, 'upsert', 'bool');
        $options += ['remove' => false, 'new' => false, 'upsert' => false];
        if (!(isset($options['update']) xor $options['remove'])) {
            throw new InvalidArgumentException('The "remove" option must be true or an "update" document must be specified, but not both');
        }
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): array|object|null
    {
        $cursor = $server->executeWriteCommand($this->databaseName, new Command($this->getCommandDocument()), $this->executeOptions(['session', 'writeConcern']));
        $tm = $this->options['typeMap'] ?? null;
        if ($tm !== null) {
            $cursor->setTypeMap(Functions::createFieldPathTypeMap($tm, 'value') + ['root' => 'array']);
            $r = current($cursor->toArray());
            return $r['value'] ?? null;
        }
        $r = current($cursor->toArray());
        return $r->value ?? null;
    }

    public function getCommandDocument(): array
    {
        $cmd = ['findAndModify' => $this->collectionName];
        if ($this->options['remove']) {
            $cmd['remove'] = true;
        } else {
            $cmd['new'] = $this->options['new'];
            $cmd['upsert'] = $this->options['upsert'];
        }
        foreach (['fields', 'query', 'sort'] as $doc) {
            if (isset($this->options[$doc])) $cmd[$doc] = (object) Functions::documentToArray($this->options[$doc]);
        }
        if (isset($this->options['update'])) {
            $cmd['update'] = Functions::isPipeline($this->options['update']) ? $this->options['update'] : (object) Functions::documentToArray($this->options['update']);
        }
        self::copy($this->options, $cmd, ['arrayFilters', 'bypassDocumentValidation', 'collation', 'comment', 'hint', 'let', 'maxTimeMS']);
        if (isset($this->options['writeConcern'])) $cmd['writeConcern'] = $this->options['writeConcern'];
        return $cmd;
    }
}

final class FindOneAndDelete implements Executable, Explainable
{
    private FindAndModify $fam;

    public function __construct(string $databaseName, string $collectionName, array|object $filter, array $options = [])
    {
        if (isset($options['projection'])) { $options['fields'] = $options['projection']; unset($options['projection']); }
        $this->fam = new FindAndModify($databaseName, $collectionName, ['query' => $filter, 'remove' => true] + $options);
    }

    public function execute(Server $server): array|object|null { return $this->fam->execute($server); }
    public function getCommandDocument(): array { return $this->fam->getCommandDocument(); }
}

final class FindOneAndReplace implements Executable, Explainable
{
    public const RETURN_DOCUMENT_BEFORE = 1;
    public const RETURN_DOCUMENT_AFTER = 2;

    private FindAndModify $fam;

    public function __construct(string $databaseName, string $collectionName, array|object $filter, array|object $replacement, array $options = [])
    {
        if (Functions::isFirstKeyOperator($replacement)) throw new InvalidArgumentException('First key in $replacement is an update operator');
        if (Functions::isPipeline($replacement, true)) throw new InvalidArgumentException('$replacement is an update pipeline');
        $options += ['returnDocument' => self::RETURN_DOCUMENT_BEFORE, 'upsert' => false];
        if (!in_array($options['returnDocument'], [self::RETURN_DOCUMENT_BEFORE, self::RETURN_DOCUMENT_AFTER], true)) {
            throw new InvalidArgumentException('Invalid value for "returnDocument" option');
        }
        if (isset($options['projection'])) { $options['fields'] = $options['projection']; unset($options['projection']); }
        $options['new'] = $options['returnDocument'] === self::RETURN_DOCUMENT_AFTER;
        unset($options['returnDocument']);
        $this->fam = new FindAndModify($databaseName, $collectionName, ['query' => $filter, 'update' => $replacement] + $options);
    }

    public function execute(Server $server): array|object|null { return $this->fam->execute($server); }
    public function getCommandDocument(): array { return $this->fam->getCommandDocument(); }
}

final class FindOneAndUpdate implements Executable, Explainable
{
    public const RETURN_DOCUMENT_BEFORE = 1;
    public const RETURN_DOCUMENT_AFTER = 2;

    private FindAndModify $fam;

    public function __construct(string $databaseName, string $collectionName, array|object $filter, array|object $update, array $options = [])
    {
        if (!Functions::isFirstKeyOperator($update) && !Functions::isPipeline($update)) {
            throw new InvalidArgumentException('Expected update operator(s) or non-empty pipeline for $update');
        }
        $options += ['returnDocument' => self::RETURN_DOCUMENT_BEFORE, 'upsert' => false];
        if (!in_array($options['returnDocument'], [self::RETURN_DOCUMENT_BEFORE, self::RETURN_DOCUMENT_AFTER], true)) {
            throw new InvalidArgumentException('Invalid value for "returnDocument" option');
        }
        if (isset($options['projection'])) { $options['fields'] = $options['projection']; unset($options['projection']); }
        $options['new'] = $options['returnDocument'] === self::RETURN_DOCUMENT_AFTER;
        unset($options['returnDocument']);
        $this->fam = new FindAndModify($databaseName, $collectionName, ['query' => $filter, 'update' => $update] + $options);
    }

    public function execute(Server $server): array|object|null { return $this->fam->execute($server); }
    public function getCommandDocument(): array { return $this->fam->getCommandDocument(); }
}

/* ================================================================== indexes */

final class CreateIndexes extends AbstractOperation implements Executable
{
    /** @var list<IndexInput> */
    private array $indexes = [];

    public function __construct(private string $databaseName, private string $collectionName, array $indexes, array $options = [])
    {
        if ($indexes === []) throw new InvalidArgumentException('$indexes is empty');
        if (!array_is_list($indexes)) throw new InvalidArgumentException('$indexes is not a list');
        foreach ($indexes as $i => $index) {
            if (!is_array($index)) throw InvalidArgumentException::invalidType(sprintf('$index[%d]', $i), $index, 'array');
            $this->indexes[] = new IndexInput($index);
        }
        self::checkCommon($options);
        $this->options = self::normalizeConcerns($options);
    }

    /** @return list<string> */
    public function execute(Server $server): array
    {
        $cmd = ['createIndexes' => $this->collectionName, 'indexes' => $this->indexes];
        self::copy($this->options, $cmd, ['comment', 'commitQuorum', 'maxTimeMS']);
        $server->executeWriteCommand($this->databaseName, new Command($cmd), $this->executeOptions(['session', 'writeConcern']));
        return array_map(static fn (IndexInput $i) => (string) $i, $this->indexes);
    }
}

final class DropIndexes extends AbstractOperation implements Executable
{
    public function __construct(private string $databaseName, private string $collectionName, private string $indexName, array $options = [])
    {
        if ($indexName === '') throw new InvalidArgumentException('$indexName cannot be empty');
        self::checkCommon($options);
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): array|object
    {
        $cmd = ['dropIndexes' => $this->collectionName, 'index' => $this->indexName];
        self::copy($this->options, $cmd, ['comment', 'maxTimeMS']);
        $cursor = $server->executeWriteCommand($this->databaseName, new Command($cmd), $this->executeOptions(['session', 'writeConcern']));
        if (isset($this->options['typeMap'])) $cursor->setTypeMap($this->options['typeMap']);
        return current($cursor->toArray());
    }
}

final class ListIndexes extends AbstractOperation implements Executable
{
    public function __construct(private string $databaseName, private string $collectionName, array $options = [])
    {
        self::checkCommon($options);
        $this->options = $options;
    }

    public function execute(Server $server): IndexInfoIterator
    {
        $cmd = ['listIndexes' => $this->collectionName];
        self::copy($this->options, $cmd, ['comment', 'maxTimeMS']);
        try {
            $cursor = $server->executeReadCommand($this->databaseName, new Command($cmd), $this->executeOptions(['session']));
        } catch (CommandException $e) {
            if ($e->getCode() === 26) return new IndexInfoIteratorIterator(new \EmptyIterator());
            throw $e;
        }
        $cursor->setTypeMap(['root' => 'array', 'document' => 'array']);
        return new IndexInfoIteratorIterator(new CachingIterator($cursor), $this->databaseName . '.' . $this->collectionName);
    }
}

final class CreateSearchIndexes extends AbstractOperation implements Executable
{
    public function __construct(private string $databaseName, private string $collectionName, private array $indexes, array $options = [])
    {
        foreach ($indexes as $i => $index) {
            if (!is_array($index)) throw InvalidArgumentException::invalidType(sprintf('$indexes[%d]', $i), $index, 'array');
            if (!isset($index['definition'])) throw new InvalidArgumentException(sprintf('Required "definition" is missing from $indexes[%d]', $i));
        }
        $this->options = $options;
    }

    public function execute(Server $server): array
    {
        $cursor = $server->executeCommand($this->databaseName, new Command(['createSearchIndexes' => $this->collectionName, 'indexes' => $this->indexes]),
            $this->executeOptions(['session']));
        $cursor->setTypeMap(['root' => 'array', 'document' => 'array']);
        $r = current($cursor->toArray());
        return array_column($r['indexesCreated'] ?? [], 'name');
    }
}

final class DropSearchIndex extends AbstractOperation implements Executable
{
    public function __construct(private string $databaseName, private string $collectionName, private string $name, array $options = [])
    {
        $this->options = $options;
    }

    public function execute(Server $server): mixed
    {
        try {
            $server->executeCommand($this->databaseName, new Command(['dropSearchIndex' => $this->collectionName, 'name' => $this->name]), $this->executeOptions(['session']));
        } catch (CommandException $e) {
            if ($e->getCode() !== 26) throw $e;
        }
        return null;
    }
}

final class UpdateSearchIndex extends AbstractOperation implements Executable
{
    public function __construct(private string $databaseName, private string $collectionName, private string $name, private array|object $definition, array $options = [])
    {
        $this->options = $options;
    }

    public function execute(Server $server): mixed
    {
        $server->executeCommand($this->databaseName, new Command(['updateSearchIndex' => $this->collectionName, 'name' => $this->name,
            'definition' => (object) Functions::documentToArray($this->definition)]), $this->executeOptions(['session']));
        return null;
    }
}

final class ListSearchIndexes extends AbstractOperation implements Executable
{
    private Aggregate $aggregate;

    public function __construct(string $databaseName, string $collectionName, array $options = [])
    {
        $stage = [];
        if (isset($options['name'])) $stage['name'] = $options['name'];
        unset($options['name']);
        $this->aggregate = new Aggregate($databaseName, $collectionName, [['$listSearchIndexes' => (object) $stage]], $options);
    }

    public function execute(Server $server): \Iterator
    {
        return $this->aggregate->execute($server);
    }
}

/* =============================================================== management */

final class DatabaseCommand extends AbstractOperation implements Executable
{
    public function __construct(private string $databaseName, private array|object $command, array $options = [])
    {
        self::checkCommon($options);
        $this->options = $options;
    }

    public function execute(Server $server): CursorInterface
    {
        $cursor = $server->executeCommand($this->databaseName, new Command($this->command), $this->executeOptions(['readPreference', 'session']));
        if (isset($this->options['typeMap'])) $cursor->setTypeMap($this->options['typeMap']);
        return $cursor;
    }
}

final class CreateCollection extends AbstractOperation implements Executable
{
    public const USE_POWER_OF_2_SIZES = 1;
    public const NO_PADDING = 2;

    public function __construct(private string $databaseName, private string $collectionName, array $options = [])
    {
        self::checkCommon($options);
        self::checkType($options, 'capped', 'bool');
        self::checkType($options, 'max', 'int');
        self::checkType($options, 'size', 'int');
        self::checkType($options, 'expireAfterSeconds', 'int');
        foreach (['changeStreamPreAndPostImages', 'clusteredIndex', 'indexOptionDefaults', 'storageEngine', 'timeseries', 'validator'] as $d) {
            self::checkType($options, $d, 'document');
        }
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): array|object
    {
        $cmd = ['create' => $this->collectionName];
        self::copy($this->options, $cmd, ['capped', 'changeStreamPreAndPostImages', 'clusteredIndex', 'collation', 'comment',
            'encryptedFields', 'expireAfterSeconds', 'flags', 'indexOptionDefaults', 'max', 'maxTimeMS', 'pipeline', 'size',
            'storageEngine', 'timeseries', 'validationAction', 'validationLevel', 'validator', 'viewOn']);
        $cursor = $server->executeWriteCommand($this->databaseName, new Command($cmd), $this->executeOptions(['session', 'writeConcern']));
        if (isset($this->options['typeMap'])) $cursor->setTypeMap($this->options['typeMap']);
        return current($cursor->toArray());
    }
}

final class DropCollection extends AbstractOperation implements Executable
{
    public function __construct(private string $databaseName, private string $collectionName, array $options = [])
    {
        self::checkCommon($options);
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): array|object
    {
        $cmd = ['drop' => $this->collectionName];
        self::copy($this->options, $cmd, ['comment']);
        try {
            $cursor = $server->executeWriteCommand($this->databaseName, new Command($cmd), $this->executeOptions(['session', 'writeConcern']));
        } catch (CommandException $e) {
            if ($e->getCode() === 26) return $e->getResultDocument();
            throw $e;
        }
        if (isset($this->options['typeMap'])) $cursor->setTypeMap($this->options['typeMap']);
        return current($cursor->toArray());
    }
}

final class DropDatabase extends AbstractOperation implements Executable
{
    public function __construct(private string $databaseName, array $options = [])
    {
        self::checkCommon($options);
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): array|object
    {
        $cmd = ['dropDatabase' => 1];
        self::copy($this->options, $cmd, ['comment']);
        $cursor = $server->executeWriteCommand($this->databaseName, new Command($cmd), $this->executeOptions(['session', 'writeConcern']));
        if (isset($this->options['typeMap'])) $cursor->setTypeMap($this->options['typeMap']);
        return current($cursor->toArray());
    }
}

final class ModifyCollection extends AbstractOperation implements Executable
{
    public function __construct(private string $databaseName, private string $collectionName, private array $collectionOptions, array $options = [])
    {
        if ($collectionOptions === []) throw new InvalidArgumentException('$collectionOptions is empty');
        self::checkCommon($options);
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): array|object
    {
        $cmd = ['collMod' => $this->collectionName] + $this->collectionOptions;
        self::copy($this->options, $cmd, ['comment']);
        $cursor = $server->executeWriteCommand($this->databaseName, new Command($cmd), $this->executeOptions(['session', 'writeConcern']));
        if (isset($this->options['typeMap'])) $cursor->setTypeMap($this->options['typeMap']);
        return current($cursor->toArray());
    }
}

final class RenameCollection extends AbstractOperation implements Executable
{
    public function __construct(private string $fromDatabaseName, private string $fromCollectionName, private string $toDatabaseName, private string $toCollectionName, array $options = [])
    {
        self::checkCommon($options);
        self::checkType($options, 'dropTarget', 'bool');
        $this->options = self::normalizeConcerns($options);
    }

    public function execute(Server $server): array|object
    {
        $cmd = ['renameCollection' => $this->fromDatabaseName . '.' . $this->fromCollectionName, 'to' => $this->toDatabaseName . '.' . $this->toCollectionName];
        self::copy($this->options, $cmd, ['comment', 'dropTarget']);
        $cursor = $server->executeWriteCommand('admin', new Command($cmd), $this->executeOptions(['session', 'writeConcern']));
        if (isset($this->options['typeMap'])) $cursor->setTypeMap($this->options['typeMap']);
        return current($cursor->toArray());
    }
}

final class ListCollections extends AbstractOperation implements Executable
{
    public function __construct(private string $databaseName, array $options = [])
    {
        self::checkCommon($options);
        self::checkType($options, 'filter', 'document');
        self::checkType($options, 'authorizedCollections', 'bool');
        self::checkType($options, 'nameOnly', 'bool');
        $this->options = $options;
    }

    public function execute(Server $server): CollectionInfoIterator
    {
        return new CollectionInfoCommandIterator($this->cursor($server), $this->databaseName);
    }

    /** @internal */
    public function cursor(Server $server): \Iterator
    {
        $cmd = ['listCollections' => 1];
        if (isset($this->options['filter'])) $cmd['filter'] = (object) Functions::documentToArray($this->options['filter']);
        self::copy($this->options, $cmd, ['authorizedCollections', 'comment', 'maxTimeMS', 'nameOnly']);
        $cursor = $server->executeReadCommand($this->databaseName, new Command($cmd), $this->executeOptions(['session']));
        $cursor->setTypeMap(['root' => 'array', 'document' => 'array']);
        return new CachingIterator($cursor);
    }
}

final class ListCollectionNames implements Executable
{
    private ListCollections $list;

    public function __construct(string $databaseName, array $options = [])
    {
        $this->list = new ListCollections($databaseName, ['nameOnly' => true] + $options);
    }

    public function execute(Server $server): \Iterator
    {
        return new CallbackIterator($this->list->cursor($server), static fn (array $info) => $info['name']);
    }
}

final class ListDatabases extends AbstractOperation implements Executable
{
    public function __construct(array $options = [])
    {
        self::checkCommon($options);
        self::checkType($options, 'filter', 'document');
        self::checkType($options, 'authorizedDatabases', 'bool');
        self::checkType($options, 'nameOnly', 'bool');
        $this->options = $options;
    }

    public function execute(Server $server): DatabaseInfoIterator
    {
        return new DatabaseInfoLegacyIterator($this->databases($server));
    }

    /** @internal */
    public function databases(Server $server): array
    {
        $cmd = ['listDatabases' => 1];
        if (isset($this->options['filter'])) $cmd['filter'] = (object) Functions::documentToArray($this->options['filter']);
        self::copy($this->options, $cmd, ['authorizedDatabases', 'comment', 'maxTimeMS', 'nameOnly']);
        $cursor = $server->executeReadCommand('admin', new Command($cmd), $this->executeOptions(['session']));
        $cursor->setTypeMap(['root' => 'array', 'document' => 'array']);
        $result = current($cursor->toArray());
        if (!isset($result['databases']) || !is_array($result['databases'])) throw new UnexpectedValueException('listDatabases command did not return a "databases" array');
        return $result['databases'];
    }
}

final class ListDatabaseNames implements Executable
{
    private ListDatabases $list;

    public function __construct(array $options = [])
    {
        $this->list = new ListDatabases(['nameOnly' => true] + $options);
    }

    public function execute(Server $server): \Iterator
    {
        $names = array_column($this->list->databases($server), 'name');
        return new \ArrayIterator($names);
    }
}

final class Explain extends AbstractOperation implements Executable
{
    public const VERBOSITY_ALL_PLANS = 'allPlansExecution';
    public const VERBOSITY_EXEC_STATS = 'executionStats';
    public const VERBOSITY_QUERY = 'queryPlanner';

    public function __construct(private string $databaseName, private Explainable $explainable, array $options = [])
    {
        self::checkCommon($options);
        self::checkType($options, 'verbosity', 'string');
        $this->options = $options;
    }

    public function execute(Server $server): array|object
    {
        $cmd = ['explain' => $this->explainable->getCommandDocument()];
        self::copy($this->options, $cmd, ['comment', 'verbosity']);
        $cursor = $server->executeCommand($this->databaseName, new Command($cmd), $this->executeOptions(['readPreference', 'session']));
        if (isset($this->options['typeMap'])) $cursor->setTypeMap($this->options['typeMap']);
        return current($cursor->toArray());
    }
}

final class Watch implements Executable
{
    public const FULL_DOCUMENT_DEFAULT = 'default';
    public const FULL_DOCUMENT_UPDATE_LOOKUP = 'updateLookup';
    public const FULL_DOCUMENT_WHEN_AVAILABLE = 'whenAvailable';
    public const FULL_DOCUMENT_REQUIRED = 'required';
    public const FULL_DOCUMENT_BEFORE_CHANGE_OFF = 'off';
    public const FULL_DOCUMENT_BEFORE_CHANGE_WHEN_AVAILABLE = 'whenAvailable';
    public const FULL_DOCUMENT_BEFORE_CHANGE_REQUIRED = 'required';

    public function __construct(private Manager $manager, private ?string $databaseName, private ?string $collectionName, private array $pipeline, private array $options = [])
    {
        if (isset($options['startAfter']) && isset($options['resumeAfter'])) {
            throw new InvalidArgumentException('Only one of "resumeAfter" and "startAfter" may be specified');
        }
        if (!array_is_list($pipeline)) throw new InvalidArgumentException('$pipeline is not a list');
    }

    public function execute(Server $server): ChangeStream
    {
        return new ChangeStream($this->manager, $this->databaseName, $this->collectionName, $this->pipeline, $this->options, $server);
    }
}

final class WithTransaction
{
    public function __construct(private $callback, private array $transactionOptions = [])
    {
        if (!is_callable($callback)) throw InvalidArgumentException::invalidType('$callback', $callback, 'callable');
    }

    public function execute(Session $session): void
    {
        $session->withTransaction($this->callback, $this->transactionOptions);
    }
}
