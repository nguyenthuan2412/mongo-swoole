<?php
declare(strict_types=1);

namespace SwooleMongo\GridFS\Exception;

use SwooleMongo\BSON\Document;
use SwooleMongo\Exception\RuntimeException;

class FileNotFoundException extends RuntimeException
{
    public static function byId(mixed $id, string $namespace): self
    {
        $json = is_array($id) || is_object($id) ? json_encode($id) : var_export($id, true);
        return new self(sprintf('File "%s" not found in "%s"', $json, $namespace));
    }

    public static function byFilenameAndRevision(string $filename, int $revision, string $namespace): self
    {
        return new self(sprintf('File with name "%s" and revision "%d" not found in "%s"', $filename, $revision, $namespace));
    }
}

class CorruptFileException extends RuntimeException
{
    public static function missingChunk(int $expectedChunkIndex): self
    {
        return new self(sprintf('Chunk not found for index "%d"', $expectedChunkIndex));
    }

    public static function unexpectedIndex(int $chunkIndex, int $expectedChunkIndex): self
    {
        return new self(sprintf('Unexpected chunk index "%d"; expected "%d"', $chunkIndex, $expectedChunkIndex));
    }

    public static function unexpectedSize(int $chunkSize, int $expectedChunkSize): self
    {
        return new self(sprintf('Unexpected chunk size "%d"; expected "%d"', $chunkSize, $expectedChunkSize));
    }
}

class StreamException extends RuntimeException
{
    public static function downloadFromIdFailed(mixed $id, $source, $destination): self
    {
        return new self(sprintf('Downloading file from "%s" to "%s" failed. GridFS identifier: "%s"',
            stream_get_meta_data($source)['uri'] ?? '', stream_get_meta_data($destination)['uri'] ?? '', json_encode($id)));
    }

    public static function downloadFromFilenameFailed(string $filename, $source, $destination): self
    {
        return new self(sprintf('Downloading file from "%s" to "%s" failed. GridFS filename: "%s"',
            stream_get_meta_data($source)['uri'] ?? '', stream_get_meta_data($destination)['uri'] ?? '', $filename));
    }

    public static function uploadFailed(string $filename, $source, $destination): self
    {
        return new self(sprintf('Uploading file from "%s" to "%s" failed. GridFS filename: "%s"',
            stream_get_meta_data($source)['uri'] ?? '', stream_get_meta_data($destination)['uri'] ?? '', $filename));
    }
}

namespace SwooleMongo\GridFS;

use SwooleMongo\BSON\Binary;
use SwooleMongo\BSON\ObjectId;
use SwooleMongo\BSON\UTCDateTime;
use SwooleMongo\Collection;
use SwooleMongo\Driver\CursorInterface;
use SwooleMongo\Driver\Manager;
use SwooleMongo\Driver\ReadConcern;
use SwooleMongo\Driver\ReadPreference;
use SwooleMongo\Driver\WriteConcern;
use SwooleMongo\Exception\InvalidArgumentException;
use SwooleMongo\GridFS\Exception\CorruptFileException;
use SwooleMongo\GridFS\Exception\FileNotFoundException;
use SwooleMongo\GridFS\Exception\StreamException;
use SwooleMongo\Model\BSONArray;
use SwooleMongo\Model\BSONDocument;

/** @internal */
class CollectionWrapper
{
    private Collection $chunksCollection;
    private Collection $filesCollection;
    private bool $checkedIndexes = false;

    public function __construct(Manager $manager, private string $databaseName, private string $bucketName, array $collectionOptions = [])
    {
        $this->filesCollection = new Collection($manager, $databaseName, sprintf('%s.files', $bucketName), $collectionOptions);
        $this->chunksCollection = new Collection($manager, $databaseName, sprintf('%s.chunks', $bucketName), $collectionOptions);
    }

    public function deleteChunksByFilesId(mixed $id): void
    {
        $this->chunksCollection->deleteMany(['files_id' => $id]);
    }

    public function deleteFileAndChunksById(mixed $id): void
    {
        $this->filesCollection->deleteOne(['_id' => $id]);
        $this->chunksCollection->deleteMany(['files_id' => $id]);
    }

    public function deleteFileAndChunksByFilename(string $filename): int
    {
        $ids = [];
        foreach ($this->filesCollection->find(['filename' => $filename], ['projection' => ['_id' => 1], 'typeMap' => ['root' => 'array']]) as $f) {
            $ids[] = $f['_id'];
        }
        if (!$ids) return 0;
        $count = $this->filesCollection->deleteMany(['_id' => ['$in' => $ids]])->getDeletedCount();
        $this->chunksCollection->deleteMany(['files_id' => ['$in' => $ids]]);
        return $count;
    }

    public function dropCollections(): void
    {
        $this->filesCollection->drop(['typeMap' => []]);
        $this->chunksCollection->drop(['typeMap' => []]);
    }

    public function findChunksByFileId(mixed $id, int $fromChunk = 0): CursorInterface
    {
        return $this->chunksCollection->find(['files_id' => $id, 'n' => ['$gte' => $fromChunk]],
            ['sort' => ['n' => 1], 'typeMap' => ['root' => 'stdClass']]);
    }

    public function findFileByFilenameAndRevision(string $filename, int $revision): ?object
    {
        if ($revision < 0) {
            $skip = abs($revision) - 1;
            $sortOrder = -1;
        } else {
            $skip = $revision;
            $sortOrder = 1;
        }
        $file = $this->filesCollection->findOne(['filename' => $filename],
            ['skip' => $skip, 'sort' => ['uploadDate' => $sortOrder], 'typeMap' => ['root' => 'stdClass']]);
        return $file === null ? null : (object) $file;
    }

    public function findFileById(mixed $id): ?object
    {
        $file = $this->filesCollection->findOne(['_id' => $id], ['typeMap' => ['root' => 'stdClass']]);
        return $file === null ? null : (object) $file;
    }

    public function findFiles(array|object $filter, array $options = []): CursorInterface
    {
        return $this->filesCollection->find($filter, $options);
    }

    public function findOneFile(array|object $filter, array $options = []): array|object|null
    {
        return $this->filesCollection->findOne($filter, $options);
    }

    public function getBucketName(): string { return $this->bucketName; }
    public function getChunksCollection(): Collection { return $this->chunksCollection; }
    public function getDatabaseName(): string { return $this->databaseName; }
    public function getFilesCollection(): Collection { return $this->filesCollection; }

    public function insertChunk(array|object $chunk): void
    {
        if (!$this->checkedIndexes) $this->ensureIndexes();
        $this->chunksCollection->insertOne($chunk);
    }

    public function insertFile(array|object $file): void
    {
        if (!$this->checkedIndexes) $this->ensureIndexes();
        $this->filesCollection->insertOne($file);
    }

    public function updateFilenameForId(mixed $id, string $filename): int
    {
        return $this->filesCollection->updateOne(['_id' => $id], ['$set' => ['filename' => $filename]])->getMatchedCount();
    }

    public function updateFilenameForFilename(string $filename, string $newFilename): int
    {
        return $this->filesCollection->updateMany(['filename' => $filename], ['$set' => ['filename' => $newFilename]])->getMatchedCount();
    }

    private function indexExists(Collection $collection, array $key): bool
    {
        foreach ($collection->listIndexes() as $index) {
            $existing = $index->getKey();
            if (count($existing) !== count($key)) continue;
            $same = true;
            foreach ($key as $field => $dir) {
                if (!isset($existing[$field]) || (int) $existing[$field] !== $dir) { $same = false; break; }
            }
            if ($same) return true;
        }
        return false;
    }

    private function ensureIndexes(): void
    {
        $this->checkedIndexes = true;
        $empty = $this->filesCollection->findOne([], ['projection' => ['_id' => 1], 'typeMap' => [],
            'readPreference' => new ReadPreference(ReadPreference::PRIMARY)]) === null;
        if (!$empty) return;
        if (!$this->indexExists($this->filesCollection, ['filename' => 1, 'uploadDate' => 1])) {
            $this->filesCollection->createIndex(['filename' => 1, 'uploadDate' => 1]);
        }
        if (!$this->indexExists($this->chunksCollection, ['files_id' => 1, 'n' => 1])) {
            $this->chunksCollection->createIndex(['files_id' => 1, 'n' => 1], ['unique' => true]);
        }
    }
}

/** @internal */
class ReadableStream
{
    private ?string $buffer = null;
    private int $bufferChunk = -1;
    private ?\Iterator $chunksIterator = null;
    private int $iteratorNext = 0;
    private int $position = 0;
    private int $chunkSize;
    private int $length;
    private int $numChunks;

    public function __construct(private CollectionWrapper $collectionWrapper, private object $file)
    {
        if (!isset($file->chunkSize) || !is_int($file->chunkSize) || $file->chunkSize < 1) {
            throw new CorruptFileException('file.chunkSize is not an integer >= 1');
        }
        if (!isset($file->length) || !is_int($file->length) || $file->length < 0) {
            throw new CorruptFileException('file.length is not an integer > 0');
        }
        if (!property_exists($file, '_id')) throw new CorruptFileException('file._id does not exist');
        $this->chunkSize = $file->chunkSize;
        $this->length = $file->length;
        $this->numChunks = (int) ceil($this->length / $this->chunkSize);
    }

    public function __debugInfo(): array
    {
        return ['bucketName' => $this->collectionWrapper->getBucketName(), 'databaseName' => $this->collectionWrapper->getDatabaseName(), 'file' => $this->file];
    }

    public function close(): void { $this->chunksIterator = null; }
    public function getFile(): object { return $this->file; }
    public function getSize(): int { return $this->length; }
    public function isEOF(): bool { return $this->position >= $this->length; }
    public function tell(): int { return $this->position; }

    public function seek(int $offset): void
    {
        if ($offset < 0 || $offset > $this->length) throw new InvalidArgumentException(sprintf('$offset must be >= 0 and <= %d; given: %d', $this->length, $offset));
        $this->position = $offset;
    }

    public function readBytes(int $length): string
    {
        if ($length < 0) throw new InvalidArgumentException(sprintf('$length must be >= 0; given: %d', $length));
        $out = '';
        while (strlen($out) < $length && $this->position < $this->length) {
            $chunk = intdiv($this->position, $this->chunkSize);
            $this->loadChunk($chunk);
            $offset = $this->position - $chunk * $this->chunkSize;
            $take = min($length - strlen($out), strlen($this->buffer) - $offset);
            $out .= substr($this->buffer, $offset, $take);
            $this->position += $take;
        }
        return $out;
    }

    private function loadChunk(int $n): void
    {
        if ($this->bufferChunk === $n) return;
        if ($this->chunksIterator === null || $this->iteratorNext !== $n) {
            $this->chunksIterator = new \IteratorIterator($this->collectionWrapper->findChunksByFileId($this->file->_id, $n));
            $this->chunksIterator->rewind();
            $this->iteratorNext = $n;
        }
        if (!$this->chunksIterator->valid()) throw CorruptFileException::missingChunk($n);
        $doc = $this->chunksIterator->current();
        $this->chunksIterator->next();
        $this->iteratorNext = $n + 1;
        if ((int) $doc->n !== $n) throw CorruptFileException::unexpectedIndex((int) $doc->n, $n);
        if (!$doc->data instanceof Binary) throw new CorruptFileException('Chunk data is not a Binary');
        $data = $doc->data->getData();
        $expected = $n === $this->numChunks - 1 ? $this->length - $n * $this->chunkSize : $this->chunkSize;
        if (strlen($data) !== $expected) throw CorruptFileException::unexpectedSize(strlen($data), $expected);
        $this->buffer = $data;
        $this->bufferChunk = $n;
    }
}

/** @internal */
class WritableStream
{
    private const DEFAULT_CHUNK_SIZE_BYTES = 261120;

    private string $buffer = '';
    private int $chunkOffset = 0;
    private int $chunkSize;
    private bool $isClosed = false;
    private int $length = 0;
    private array $file;

    public function __construct(private CollectionWrapper $collectionWrapper, string $filename, array $options = [])
    {
        $options += ['_id' => new ObjectId(), 'chunkSizeBytes' => self::DEFAULT_CHUNK_SIZE_BYTES];
        if (!is_int($options['chunkSizeBytes']) || $options['chunkSizeBytes'] < 1) {
            throw new InvalidArgumentException(sprintf('Expected "chunkSizeBytes" option to be >= 1, %s given', var_export($options['chunkSizeBytes'], true)));
        }
        if (isset($options['metadata']) && !is_array($options['metadata']) && !is_object($options['metadata'])) {
            throw InvalidArgumentException::expectedDocumentType('"metadata" option', $options['metadata']);
        }
        $this->chunkSize = $options['chunkSizeBytes'];
        $this->file = ['_id' => $options['_id'], 'chunkSize' => $this->chunkSize, 'filename' => $filename, 'length' => null, 'uploadDate' => null]
            + array_intersect_key($options, ['aliases' => 1, 'contentType' => 1, 'metadata' => 1]);
    }

    public function __debugInfo(): array
    {
        return ['bucketName' => $this->collectionWrapper->getBucketName(), 'databaseName' => $this->collectionWrapper->getDatabaseName(), 'file' => $this->file];
    }

    public function close(): void
    {
        if ($this->isClosed) return;
        if ($this->buffer !== '') $this->insertChunkFromBuffer();
        $this->file['length'] = $this->length;
        $this->file['uploadDate'] = new UTCDateTime();
        try {
            $this->collectionWrapper->insertFile($this->file);
        } catch (\Throwable $e) {
            $this->abort();
            throw $e;
        }
        $this->isClosed = true;
    }

    public function getFile(): object { return (object) $this->file; }
    public function getSize(): int { return $this->length + strlen($this->buffer); }
    public function tell(): int { return $this->getSize(); }

    public function writeBytes(string $data): int
    {
        if ($this->isClosed) return 0;
        $this->buffer .= $data;
        while (strlen($this->buffer) >= $this->chunkSize) {
            $this->insertChunkFromBuffer();
        }
        return strlen($data);
    }

    private function abort(): void
    {
        try { $this->collectionWrapper->deleteChunksByFilesId($this->file['_id']); } catch (\Throwable) {}
        $this->isClosed = true;
    }

    private function insertChunkFromBuffer(): void
    {
        $data = substr($this->buffer, 0, $this->chunkSize);
        $this->buffer = (string) substr($this->buffer, strlen($data));
        $chunk = ['files_id' => $this->file['_id'], 'n' => $this->chunkOffset, 'data' => new Binary($data)];
        try {
            $this->collectionWrapper->insertChunk($chunk);
        } catch (\Throwable $e) {
            $this->abort();
            throw $e;
        }
        $this->length += strlen($data);
        $this->chunkOffset++;
    }
}

/** Stream wrapper for reading and writing GridFS files (protocol "gridfs"). @internal */
class StreamWrapper
{
    /** @var resource|null */
    public $context;

    private ReadableStream|WritableStream|null $stream = null;
    private string $protocol = 'gridfs';

    public static function register(string $protocol = 'gridfs'): void
    {
        if (in_array($protocol, stream_get_wrappers(), true)) {
            stream_wrapper_unregister($protocol);
        }
        stream_wrapper_register($protocol, static::class, STREAM_IS_URL);
    }

    public function getFile(): object
    {
        return $this->stream->getFile();
    }

    public function stream_close(): void
    {
        $this->stream?->close();
    }

    public function stream_eof(): bool
    {
        return $this->stream instanceof ReadableStream ? $this->stream->isEOF() : false;
    }

    public function stream_open(string $path, string $mode, int $options, ?string &$openedPath): bool
    {
        $this->protocol = explode('://', $path, 2)[0];
        $context = stream_context_get_options($this->context)[$this->protocol] ?? [];
        if ($mode === 'r' || $mode === 'rb') {
            if (!isset($context['collectionWrapper'], $context['file'])) return false;
            $this->stream = new ReadableStream($context['collectionWrapper'], $context['file']);
            return true;
        }
        if ($mode === 'w' || $mode === 'wb') {
            if (!isset($context['collectionWrapper'], $context['filename'])) return false;
            $this->stream = new WritableStream($context['collectionWrapper'], $context['filename'], $context['options'] ?? []);
            return true;
        }
        return false;
    }

    public function stream_read(int $length): string
    {
        return $this->stream instanceof ReadableStream ? $this->stream->readBytes($length) : '';
    }

    public function stream_seek(int $offset, int $whence = SEEK_SET): bool
    {
        if (!$this->stream instanceof ReadableStream) return false;
        $size = $this->stream->getSize();
        $pos = match ($whence) { SEEK_SET => $offset, SEEK_CUR => $this->stream->tell() + $offset, SEEK_END => $size + $offset, default => -1 };
        if ($pos < 0 || $pos > $size) return false;
        $this->stream->seek($pos);
        return true;
    }

    public function stream_stat(): array
    {
        $stat = array_fill_keys(['dev', 'ino', 'mode', 'nlink', 'uid', 'gid', 'rdev', 'size', 'atime', 'mtime', 'ctime', 'blksize', 'blocks'], 0);
        $stat['mode'] = $this->stream instanceof ReadableStream ? 0100444 : 0100222;
        $stat['size'] = $this->stream?->getSize() ?? 0;
        $file = $this->stream?->getFile();
        if ($file !== null) {
            if (isset($file->uploadDate) && $file->uploadDate instanceof UTCDateTime) {
                $ts = $file->uploadDate->toDateTime()->getTimestamp();
                $stat['mtime'] = $stat['ctime'] = $ts;
            }
            if (isset($file->chunkSize) && is_int($file->chunkSize)) $stat['blksize'] = $file->chunkSize;
        }
        return $stat;
    }

    public function stream_tell(): int
    {
        return $this->stream?->tell() ?? 0;
    }

    public function stream_write(string $data): int
    {
        return $this->stream instanceof WritableStream ? $this->stream->writeBytes($data) : 0;
    }
}

/** Mirrors MongoDB\GridFS\Bucket. */
class Bucket
{
    private const DEFAULT_BUCKET_NAME = 'fs';
    private const DEFAULT_CHUNK_SIZE_BYTES = 261120;
    private const STREAM_WRAPPER_PROTOCOL = 'gridfs';

    private static array $defaultTypeMap = ['array' => BSONArray::class, 'document' => BSONDocument::class, 'root' => BSONDocument::class];

    private string $bucketName;
    private int $chunkSizeBytes;
    private CollectionWrapper $collectionWrapper;
    private ReadConcern $readConcern;
    private ReadPreference $readPreference;
    private array $typeMap;
    private WriteConcern $writeConcern;

    public function __construct(private Manager $manager, private string $databaseName, array $options = [])
    {
        $options += ['bucketName' => self::DEFAULT_BUCKET_NAME, 'chunkSizeBytes' => self::DEFAULT_CHUNK_SIZE_BYTES];
        if (!is_string($options['bucketName']) || $options['bucketName'] === '') throw InvalidArgumentException::invalidType('"bucketName" option', $options['bucketName'], 'string');
        if (!is_int($options['chunkSizeBytes']) || $options['chunkSizeBytes'] < 1) {
            throw new InvalidArgumentException(sprintf('Expected "chunkSizeBytes" option to be >= 1, %s given', var_export($options['chunkSizeBytes'], true)));
        }
        $this->bucketName = $options['bucketName'];
        $this->chunkSizeBytes = $options['chunkSizeBytes'];
        $this->readConcern = $options['readConcern'] ?? $manager->getReadConcern();
        $this->readPreference = $options['readPreference'] ?? $manager->getReadPreference();
        $this->typeMap = $options['typeMap'] ?? self::$defaultTypeMap;
        $this->writeConcern = $options['writeConcern'] ?? $manager->getWriteConcern();
        $collectionOptions = ['readConcern' => $this->readConcern, 'readPreference' => $this->readPreference,
            'typeMap' => $this->typeMap, 'writeConcern' => $this->writeConcern];
        $this->collectionWrapper = new CollectionWrapper($manager, $databaseName, $this->bucketName, $collectionOptions);
        $this->registerStreamWrapper();
    }

    public function __debugInfo(): array
    {
        return ['bucketName' => $this->bucketName, 'databaseName' => $this->databaseName, 'manager' => $this->manager,
            'chunkSizeBytes' => $this->chunkSizeBytes, 'readConcern' => $this->readConcern, 'readPreference' => $this->readPreference,
            'typeMap' => $this->typeMap, 'writeConcern' => $this->writeConcern];
    }

    public function delete(mixed $id): void
    {
        $file = $this->collectionWrapper->findFileById($id);
        $this->collectionWrapper->deleteFileAndChunksById($id);
        if ($file === null) throw FileNotFoundException::byId($id, $this->getFilesNamespace());
    }

    public function deleteByName(string $filename): void
    {
        if ($this->collectionWrapper->deleteFileAndChunksByFilename($filename) === 0) {
            throw FileNotFoundException::byFilenameAndRevision($filename, -1, $this->getFilesNamespace());
        }
    }

    /** @param resource $destination */
    public function downloadToStream(mixed $id, $destination): void
    {
        if (!is_resource($destination) || get_resource_type($destination) !== 'stream') {
            throw InvalidArgumentException::invalidType('$destination', $destination, 'resource');
        }
        $source = $this->openDownloadStream($id);
        if (@stream_copy_to_stream($source, $destination) === false) {
            throw StreamException::downloadFromIdFailed($id, $source, $destination);
        }
        fclose($source);
    }

    /** @param resource $destination */
    public function downloadToStreamByName(string $filename, $destination, array $options = []): void
    {
        if (!is_resource($destination) || get_resource_type($destination) !== 'stream') {
            throw InvalidArgumentException::invalidType('$destination', $destination, 'resource');
        }
        $source = $this->openDownloadStreamByName($filename, $options);
        if (@stream_copy_to_stream($source, $destination) === false) {
            throw StreamException::downloadFromFilenameFailed($filename, $source, $destination);
        }
        fclose($source);
    }

    public function drop(): void
    {
        $this->collectionWrapper->dropCollections();
    }

    public function find(array|object $filter = [], array $options = []): CursorInterface
    {
        return $this->collectionWrapper->findFiles($filter, $options);
    }

    public function findOne(array|object $filter = [], array $options = []): array|object|null
    {
        return $this->collectionWrapper->findOneFile($filter, $options);
    }

    public function getBucketName(): string { return $this->bucketName; }
    public function getChunkSizeBytes(): int { return $this->chunkSizeBytes; }
    public function getChunksCollection(): Collection { return $this->collectionWrapper->getChunksCollection(); }
    public function getDatabaseName(): string { return $this->databaseName; }
    public function getFilesCollection(): Collection { return $this->collectionWrapper->getFilesCollection(); }
    public function getReadConcern(): ReadConcern { return $this->readConcern; }
    public function getReadPreference(): ReadPreference { return $this->readPreference; }
    public function getTypeMap(): array { return $this->typeMap; }
    public function getWriteConcern(): WriteConcern { return $this->writeConcern; }

    /** @param resource $stream */
    public function getFileDocumentForStream($stream): array|object
    {
        $file = $this->getRawFileDocumentForStream($stream);
        $typeMap = $this->typeMap;
        return \SwooleMongo\BSON\Document::fromPHP($file)->toPHP($typeMap);
    }

    /** @param resource $stream */
    public function getFileIdForStream($stream): mixed
    {
        $file = $this->getRawFileDocumentForStream($stream);
        return $file->_id;
    }

    /** @return resource */
    public function openDownloadStream(mixed $id)
    {
        $file = $this->collectionWrapper->findFileById($id);
        if ($file === null) throw FileNotFoundException::byId($id, $this->getFilesNamespace());
        return $this->openDownloadStreamByFile($file);
    }

    /** @return resource */
    public function openDownloadStreamByName(string $filename, array $options = [])
    {
        $options += ['revision' => -1];
        $file = $this->collectionWrapper->findFileByFilenameAndRevision($filename, (int) $options['revision']);
        if ($file === null) throw FileNotFoundException::byFilenameAndRevision($filename, (int) $options['revision'], $this->getFilesNamespace());
        return $this->openDownloadStreamByFile($file);
    }

    /** @return resource */
    public function openUploadStream(string $filename, array $options = [])
    {
        $options += ['chunkSizeBytes' => $this->chunkSizeBytes];
        $path = $this->createPathForUpload();
        $context = stream_context_create([self::STREAM_WRAPPER_PROTOCOL => [
            'collectionWrapper' => $this->collectionWrapper, 'filename' => $filename, 'options' => $options]]);
        return fopen($path, 'w', false, $context);
    }

    public function rename(mixed $id, string $newFilename): void
    {
        if ($this->collectionWrapper->updateFilenameForId($id, $newFilename) === 0) {
            throw FileNotFoundException::byId($id, $this->getFilesNamespace());
        }
    }

    public function renameByName(string $filename, string $newFilename): void
    {
        if ($this->collectionWrapper->updateFilenameForFilename($filename, $newFilename) === 0) {
            throw FileNotFoundException::byFilenameAndRevision($filename, -1, $this->getFilesNamespace());
        }
    }

    /** @param resource $source */
    public function uploadFromStream(string $filename, $source, array $options = []): mixed
    {
        if (!is_resource($source) || get_resource_type($source) !== 'stream') {
            throw InvalidArgumentException::invalidType('$source', $source, 'resource');
        }
        $destination = $this->openUploadStream($filename, $options);
        if (@stream_copy_to_stream($source, $destination) === false) {
            throw StreamException::uploadFailed($filename, $source, $destination);
        }
        $id = $this->getFileIdForStream($destination);
        fclose($destination);
        return $id;
    }

    private function createPathForFile(object $file): string
    {
        $id = $file->_id;
        $id = is_array($id) || (is_object($id) && !method_exists($id, '__toString')) ? json_encode($id) : (string) $id;
        return sprintf('%s://%s/%s.files/%s', self::STREAM_WRAPPER_PROTOCOL, urlencode($this->databaseName), urlencode($this->bucketName), urlencode($id));
    }

    private function createPathForUpload(): string
    {
        return sprintf('%s://%s/%s.files', self::STREAM_WRAPPER_PROTOCOL, urlencode($this->databaseName), urlencode($this->bucketName));
    }

    private function getFilesNamespace(): string
    {
        return sprintf('%s.%s.files', $this->databaseName, $this->bucketName);
    }

    /** @param resource $stream */
    private function getRawFileDocumentForStream($stream): object
    {
        if (!is_resource($stream) || get_resource_type($stream) !== 'stream') {
            throw InvalidArgumentException::invalidType('$stream', $stream, 'resource');
        }
        $metadata = stream_get_meta_data($stream);
        if (!isset($metadata['wrapper_data']) || !$metadata['wrapper_data'] instanceof StreamWrapper) {
            throw InvalidArgumentException::invalidType('$stream wrapper data', $metadata['wrapper_data'] ?? null, StreamWrapper::class);
        }
        return $metadata['wrapper_data']->getFile();
    }

    /** @return resource */
    private function openDownloadStreamByFile(object $file)
    {
        $path = $this->createPathForFile($file);
        $context = stream_context_create([self::STREAM_WRAPPER_PROTOCOL => [
            'collectionWrapper' => $this->collectionWrapper, 'file' => $file]]);
        return fopen($path, 'r', false, $context);
    }

    private function registerStreamWrapper(): void
    {
        if (in_array(self::STREAM_WRAPPER_PROTOCOL, stream_get_wrappers(), true)) return;
        StreamWrapper::register(self::STREAM_WRAPPER_PROTOCOL);
    }
}
