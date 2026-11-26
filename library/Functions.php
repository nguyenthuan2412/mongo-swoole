<?php
declare(strict_types=1);

namespace SwooleMongo\Exception;

use SwooleMongo\Driver\Exception\InvalidArgumentException as DriverInvalidArgumentException;
use SwooleMongo\Driver\Exception\RuntimeException as DriverRuntimeException;
use SwooleMongo\Driver\Exception\UnexpectedValueException as DriverUnexpectedValueException;

interface Exception extends \Throwable
{
}

class InvalidArgumentException extends DriverInvalidArgumentException implements Exception
{
    public static function expectedDocumentType(string $name, mixed $value): self
    {
        return new self(sprintf('Expected %s to have type "document" (array or object) but found "%s"', $name, get_debug_type($value)));
    }

    public static function invalidType(string $name, mixed $value, string|array $expectedType): self
    {
        if (is_array($expectedType)) {
            $expectedType = implode('" or "', $expectedType);
        }
        return new self(sprintf('Expected %s to have type "%s" but found "%s"', $name, $expectedType, get_debug_type($value)));
    }
}

class BadMethodCallException extends \BadMethodCallException implements Exception
{
    public static function classIsImmutable(string $class): self
    {
        return new self(sprintf('%s is immutable', $class));
    }

    public static function unacknowledgedWriteResultAccess(string $method): self
    {
        return new self(sprintf('%s should not be called for an unacknowledged write result', $method));
    }
}

class RuntimeException extends DriverRuntimeException implements Exception
{
}

class UnexpectedValueException extends DriverUnexpectedValueException implements Exception
{
}

class UnsupportedException extends RuntimeException
{
    public static function readConcernNotSupportedInTransaction(): self
    {
        return new self('The "readConcern" option cannot be specified within a transaction. Instead, specify it when starting the transaction.');
    }

    public static function writeConcernNotSupportedInTransaction(): self
    {
        return new self('The "writeConcern" option cannot be specified within a transaction. Instead, specify it when starting the transaction.');
    }

    public static function explainNotSupported(): self
    {
        return new self('Explain is not supported');
    }

    public static function clientEncryptionNotSupported(): self
    {
        return new self('Client-side field level encryption was not compiled in; rebuild swmongo with --with-swmongo-mongocrypt');
    }
}

class ResumeTokenException extends RuntimeException
{
    public static function invalidType(mixed $value): self
    {
        return new self(sprintf('Expected resume token to have type "array or object" but found "%s"', get_debug_type($value)));
    }

    public static function notFound(): self
    {
        return new self('Resume token not found in change document');
    }
}

class CreateEncryptedCollectionException extends RuntimeException
{
    public function __construct(\Throwable $previous, private array $encryptedFields)
    {
        parent::__construct(sprintf('Creating encrypted collection failed due to previous %s: %s', get_class($previous), $previous->getMessage()), 0, $previous);
    }

    /** encryptedFields including the keyIds of the data keys created so far */
    public function getEncryptedFields(): array
    {
        return $this->encryptedFields;
    }
}

namespace SwooleMongo;

use SwooleMongo\BSON\Document;
use SwooleMongo\BSON\PackedArray;
use SwooleMongo\BSON\Serializable;
use SwooleMongo\Driver\Manager;
use SwooleMongo\Driver\ReadPreference;
use SwooleMongo\Driver\Server;
use SwooleMongo\Driver\Session;
use SwooleMongo\Driver\WriteConcern;
use SwooleMongo\Exception\InvalidArgumentException;

/**
 * Helper functions of mongodb/mongodb (functions.php) exposed as static methods.
 * @internal
 */
final class Functions
{
    /** encryptedFields configured for db.coll in the autoEncryption encryptedFieldsMap, or null */
    public static function encryptedFieldsFromMap(Manager $manager, string $db, string $coll): array|object|null
    {
        $map = $manager->getEncryptedFieldsMap();
        if ($map === null) return null;
        if ($map instanceof Document) $map = $map->toPHP(['root' => 'array', 'document' => 'array', 'array' => 'array']);
        $map = is_object($map) ? get_object_vars($map) : $map;
        return $map[$db . '.' . $coll] ?? null;
    }

    /** names of the Queryable Encryption state collections (esc, ecoc) of a collection */
    public static function encryptedStateCollections(string $coll, array $encryptedFields): array
    {
        return [
            $encryptedFields['escCollection'] ?? 'enxcol_.' . $coll . '.esc',
            $encryptedFields['ecocCollection'] ?? 'enxcol_.' . $coll . '.ecoc',
        ];
    }

    /** drop that also removes the Queryable Encryption state collections */
    public static function dropCollection(Manager $manager, string $db, string $coll, array $options): array|object
    {
        $server = self::selectServer($manager, $options);
        $ef = $options['encryptedFields'] ?? self::encryptedFieldsFromMap($manager, $db, $coll);
        if ($ef === null && $manager->getEncryptedFieldsMap() !== null) {
            // auto encryption is configured: look the collection options up on the server
            $info = (new Operation\ListCollections($db, ['filter' => ['name' => $coll]] + array_intersect_key($options, ['session' => 1])))->execute($server);
            foreach ($info as $ci) {
                $ef = $ci->getOptions()['encryptedFields'] ?? null;
            }
        }
        unset($options['encryptedFields']);
        if ($ef !== null) {
            $aux = array_intersect_key($options, ['session' => 1, 'writeConcern' => 1]);
            foreach (self::encryptedStateCollections($coll, self::documentToArray($ef)) as $state) {
                (new Operation\DropCollection($db, $state, $aux))->execute($server);
            }
        }
        return (new Operation\DropCollection($db, $coll, $options))->execute($server);
    }

    public static function documentToArray(array|object $document): array
    {
        if ($document instanceof Document) {
            $document = $document->toPHP(['array' => 'array', 'document' => 'array', 'root' => 'array']);
        } elseif ($document instanceof PackedArray) {
            $document = $document->toPHP(['array' => 'array', 'document' => 'array', 'root' => 'array']);
        } elseif ($document instanceof Serializable) {
            $document = $document->bsonSerialize();
        }
        if (is_object($document)) {
            $document = get_object_vars($document);
        }
        if (!is_array($document)) {
            throw InvalidArgumentException::expectedDocumentType('$document', $document);
        }
        return $document;
    }

    public static function isFirstKeyOperator(array|object $document): bool
    {
        if ($document instanceof PackedArray) {
            return false;
        }
        $document = self::documentToArray($document);
        $firstKey = array_key_first($document);
        return is_string($firstKey) && $firstKey !== '' && $firstKey[0] === '$';
    }

    public static function isPipeline(array|object $pipeline, bool $allowEmpty = false): bool
    {
        if ($pipeline instanceof PackedArray) {
            $pipeline = $pipeline->toPHP(['array' => 'array', 'document' => 'array', 'root' => 'array']);
        }
        if (!is_array($pipeline)) {
            return false;
        }
        if ($pipeline === []) {
            return $allowEmpty;
        }
        if (!array_is_list($pipeline)) {
            return false;
        }
        foreach ($pipeline as $stage) {
            if (!is_array($stage) && !is_object($stage)) {
                return false;
            }
            if (!self::isFirstKeyOperator($stage)) {
                return false;
            }
        }
        return true;
    }

    public static function isInTransaction(array $options): bool
    {
        return isset($options['session']) && $options['session'] instanceof Session && $options['session']->isInTransaction();
    }

    public static function isLastPipelineOperatorWrite(array $pipeline): bool
    {
        $last = end($pipeline);
        if ($last === false) {
            return false;
        }
        $key = array_key_first(self::documentToArray($last));
        return $key === '$out' || $key === '$merge';
    }

    public static function isWriteConcernAcknowledged(WriteConcern $writeConcern): bool
    {
        return $writeConcern->getW() !== 0 || $writeConcern->getJournal() === true;
    }

    public static function createFieldPathTypeMap(array $typeMap, string $fieldPath): array
    {
        $fieldPaths = [];
        if (isset($typeMap['fieldPaths']) && is_array($typeMap['fieldPaths'])) {
            foreach ($typeMap['fieldPaths'] as $existing => $type) {
                $fieldPaths[$fieldPath . '.' . $existing] = $type;
            }
            unset($typeMap['fieldPaths']);
        }
        if (isset($typeMap['root'])) {
            $fieldPaths[$fieldPath] = $typeMap['root'];
            unset($typeMap['root']);
        }
        $typeMap['fieldPaths'] = $fieldPaths;
        return $typeMap;
    }

    public static function selectServer(Manager $manager, array $options): Server
    {
        $server = isset($options['session']) && $options['session'] instanceof Session ? $options['session']->getServer() : null;
        if ($server !== null) {
            return $server;
        }
        $readPreference = $options['readPreference'] ?? null;
        if (!$readPreference instanceof ReadPreference) {
            $readPreference = new ReadPreference(ReadPreference::PRIMARY);
        }
        return $manager->selectServer($readPreference);
    }

    public static function selectServerForAggregateWriteStage(Manager $manager, array &$options): Server
    {
        $readPreference = $options['readPreference'] ?? null;
        if ($readPreference instanceof ReadPreference && $readPreference->getModeString() === ReadPreference::PRIMARY) {
            return self::selectServer($manager, $options);
        }
        try {
            $server = self::selectServer($manager, $options);
            /* $out / $merge on secondaries requires MongoDB 5.0 (wire version 13) */
            if (($server->getInfo()['maxWireVersion'] ?? 0) < 13) {
                unset($options['readPreference']);
                return self::selectServer($manager, $options);
            }
            return $server;
        } catch (\Throwable) {
            unset($options['readPreference']);
            return self::selectServer($manager, $options);
        }
    }

    /** Equivalent of MongoDB\with_transaction() */
    public static function withTransaction(Session $session, callable $callback, array $transactionOptions = []): void
    {
        (new Operation\WithTransaction($callback, $transactionOptions))->execute($session);
    }
}
