<?php
declare(strict_types=1);

namespace SwooleMongo\Model;

use ArrayAccess;
use ArrayIterator;
use ArrayObject;
use Countable;
use Iterator;
use IteratorIterator;
use JsonSerializable;
use SwooleMongo\BSON\Serializable;
use SwooleMongo\BSON\Unserializable;
use SwooleMongo\Exception\BadMethodCallException;
use SwooleMongo\Exception\InvalidArgumentException;
use Traversable;

class BSONArray extends ArrayObject implements JsonSerializable, Serializable, Unserializable
{
    public function __clone()
    {
        foreach ($this as $key => $value) {
            if (is_object($value)) {
                $this[$key] = clone $value;
            }
        }
    }

    public static function __set_state(array $properties): self
    {
        $array = new static();
        $array->exchangeArray($properties);
        return $array;
    }

    public function bsonSerialize(): array
    {
        return array_values($this->getArrayCopy());
    }

    public function bsonUnserialize(array $data): void
    {
        parent::__construct($data);
    }

    public function jsonSerialize(): array
    {
        return array_values($this->getArrayCopy());
    }
}

class BSONDocument extends ArrayObject implements JsonSerializable, Serializable, Unserializable
{
    public function __construct(array $input = [], int $flags = ArrayObject::ARRAY_AS_PROPS, string $iteratorClass = ArrayIterator::class)
    {
        parent::__construct($input, $flags, $iteratorClass);
    }

    public function __clone()
    {
        foreach ($this as $key => $value) {
            if (is_object($value)) {
                $this[$key] = clone $value;
            }
        }
    }

    public static function __set_state(array $properties): self
    {
        $document = new static();
        $document->exchangeArray($properties);
        return $document;
    }

    public function bsonSerialize(): object
    {
        return (object) $this->getArrayCopy();
    }

    public function bsonUnserialize(array $data): void
    {
        parent::__construct($data, ArrayObject::ARRAY_AS_PROPS);
    }

    public function jsonSerialize(): object
    {
        return (object) $this->getArrayCopy();
    }
}

/** @internal */
final class CallbackIterator implements Iterator
{
    private Iterator $iterator;

    public function __construct(Traversable $traversable, private $callback)
    {
        $this->iterator = $traversable instanceof Iterator ? $traversable : new IteratorIterator($traversable);
    }

    public function current(): mixed { return ($this->callback)($this->iterator->current(), $this->iterator->key()); }
    public function key(): mixed { return $this->iterator->key(); }
    public function next(): void { $this->iterator->next(); }
    public function rewind(): void { $this->iterator->rewind(); }
    public function valid(): bool { return $this->iterator->valid(); }
}

/** Caches a (non rewindable) cursor so it can be iterated several times. @internal */
class CachingIterator implements Countable, Iterator
{
    private const FIELD_KEY = 0;
    private const FIELD_VALUE = 1;

    private array $items = [];
    private ?Iterator $iterator;
    private bool $iteratorAdvanced = false;
    private bool $iteratorExhausted = false;

    public function __construct(Traversable $traversable)
    {
        $this->iterator = $traversable instanceof Iterator ? $traversable : new IteratorIterator($traversable);
        $this->iterator->rewind();
        $this->storeCurrentItem();
    }

    public function count(): int
    {
        $this->exhaustIterator();
        return count($this->items);
    }

    public function current(): mixed
    {
        $current = current($this->items);
        return $current !== false ? $current[self::FIELD_VALUE] : null;
    }

    public function key(): mixed
    {
        $current = current($this->items);
        return $current !== false ? $current[self::FIELD_KEY] : null;
    }

    public function next(): void
    {
        if (!$this->iteratorExhausted) {
            $this->iteratorAdvanced = true;
            $this->iterator->next();
            $this->storeCurrentItem();
            $this->iteratorExhausted = !$this->iterator->valid();
        }
        next($this->items);
    }

    public function rewind(): void
    {
        if ($this->iteratorAdvanced) {
            $this->exhaustIterator();
        }
        reset($this->items);
    }

    public function valid(): bool
    {
        return $this->key() !== null;
    }

    private function exhaustIterator(): void
    {
        while (!$this->iteratorExhausted) {
            $this->next();
        }
        $this->iterator = null;
    }

    private function storeCurrentItem(): void
    {
        if (!$this->iterator->valid()) {
            $this->iteratorExhausted = true;
            return;
        }
        $this->items[] = [self::FIELD_KEY => $this->iterator->key(), self::FIELD_VALUE => $this->iterator->current()];
    }
}

abstract class InfoModel implements ArrayAccess
{
    public function __construct(protected array $info)
    {
    }

    public function __debugInfo(): array { return $this->info; }
    public function offsetExists(mixed $offset): bool { return array_key_exists($offset, $this->info); }
    public function offsetGet(mixed $offset): mixed { return $this->info[$offset]; }
    public function offsetSet(mixed $offset, mixed $value): void { throw BadMethodCallException::classIsImmutable(static::class); }
    public function offsetUnset(mixed $offset): void { throw BadMethodCallException::classIsImmutable(static::class); }
}

class CollectionInfo extends InfoModel
{
    public function getCappedMax(): ?int { return isset($this->info['options']['max']) ? (int) $this->info['options']['max'] : null; }
    public function getCappedSize(): ?int { return isset($this->info['options']['size']) ? (int) $this->info['options']['size'] : null; }
    public function getIdIndex(): array { return (array) ($this->info['idIndex'] ?? []); }
    public function getInfo(): array { return (array) ($this->info['info'] ?? []); }
    public function getName(): string { return (string) $this->info['name']; }
    public function getOptions(): array { return (array) ($this->info['options'] ?? []); }
    public function getType(): string { return (string) $this->info['type']; }
    public function isCapped(): bool { return !empty($this->info['options']['capped']); }
}

class DatabaseInfo extends InfoModel
{
    public function getName(): string { return (string) $this->info['name']; }
    public function getSizeOnDisk(): int { return (int) $this->info['sizeOnDisk']; }
    public function isEmpty(): bool { return (bool) $this->info['empty']; }
}

class IndexInfo extends InfoModel
{
    public function __toString(): string { return $this->getName(); }
    public function getKey(): array { return (array) $this->info['key']; }
    public function getName(): string { return (string) $this->info['name']; }
    public function getNamespace(): string { return (string) ($this->info['ns'] ?? ''); }
    public function getVersion(): int { return (int) $this->info['v']; }
    public function is2dSphere(): bool { return array_search('2dsphere', $this->getKey(), true) !== false; }
    public function isGeoHaystack(): bool { return array_search('geoHaystack', $this->getKey(), true) !== false; }
    public function isSparse(): bool { return !empty($this->info['sparse']); }
    public function isText(): bool { return array_search('text', $this->getKey(), true) !== false; }
    public function isTtl(): bool { return array_key_exists('expireAfterSeconds', $this->info); }
    public function isUnique(): bool { return !empty($this->info['unique']); }
}

interface CollectionInfoIterator extends Iterator
{
    public function current(): CollectionInfo;
}

interface DatabaseInfoIterator extends Iterator
{
    public function current(): DatabaseInfo;
}

interface IndexInfoIterator extends Iterator
{
    public function current(): IndexInfo;
}

class CollectionInfoCommandIterator extends IteratorIterator implements CollectionInfoIterator
{
    public function __construct(Traversable $iterator, private ?string $databaseName = null)
    {
        parent::__construct($iterator);
    }

    public function current(): CollectionInfo
    {
        $info = parent::current();
        if ($this->databaseName !== null && isset($info['idIndex']) && !isset($info['idIndex']['ns'])) {
            $info['idIndex']['ns'] = $this->databaseName . '.' . $info['name'];
        }
        return new CollectionInfo($info);
    }
}

class DatabaseInfoLegacyIterator implements DatabaseInfoIterator
{
    public function __construct(private array $databases)
    {
    }

    public function current(): DatabaseInfo { return new DatabaseInfo(current($this->databases)); }
    public function key(): mixed { return key($this->databases); }
    public function next(): void { next($this->databases); }
    public function rewind(): void { reset($this->databases); }
    public function valid(): bool { return key($this->databases) !== null; }
}

class IndexInfoIteratorIterator extends IteratorIterator implements IndexInfoIterator
{
    public function __construct(Traversable $iterator, private ?string $ns = null)
    {
        parent::__construct($iterator);
    }

    public function current(): IndexInfo
    {
        $info = parent::current();
        if (!array_key_exists('ns', $info) && $this->ns !== null) {
            $info['ns'] = $this->ns;
        }
        return new IndexInfo($info);
    }
}

/** @internal index specification used by createIndex(es) */
final class IndexInput implements Serializable
{
    public function __construct(private array $index)
    {
        if (!isset($index['key'])) {
            throw new InvalidArgumentException('Required "key" document is missing from index specification');
        }
        if (!is_array($index['key']) && !is_object($index['key'])) {
            throw InvalidArgumentException::expectedDocumentType('"key" option', $index['key']);
        }
        foreach ($index['key'] as $fieldName => $order) {
            if (!is_int($order) && !is_float($order) && !is_string($order)) {
                throw InvalidArgumentException::invalidType(sprintf('order value for "%s" field within "key" option', $fieldName), $order, 'numeric or string');
            }
        }
        if (!isset($index['name'])) {
            $this->index['name'] = self::generateIndexName($index['key']);
        }
        if (!is_string($this->index['name'])) {
            throw InvalidArgumentException::invalidType('"name" option', $this->index['name'], 'string');
        }
    }

    public function __toString(): string
    {
        return $this->index['name'];
    }

    public function bsonSerialize(): object
    {
        return (object) $this->index;
    }

    private static function generateIndexName(array|object $document): string
    {
        $name = '';
        foreach (\SwooleMongo\Functions::documentToArray($document) as $field => $type) {
            $name .= ($name !== '' ? '_' : '') . $field . '_' . $type;
        }
        return $name;
    }
}
