#ifndef DOCUMENT_H
#define DOCUMENT_H

/**
 * document.h - Core data structures for the collaborative document editor.
 *
 * This file defines the chunk-based document representation used by both
 * the server and client. Instead of a simple character array (which would
 * require O(n) shifts on every insert), we use a piece-table-like linked
 * list of "chunks" that reference regions within a single append-only buffer.
 *
 * Key design decisions:
 *   - Append-only buffer: All inserted text is appended to a single
 *     contiguous buffer (`buf`). Chunks store offset+length pairs into
 *     this buffer, avoiding memory fragmentation.
 *   - Soft deletion: Chunks are never physically removed. Instead, they
 *     are marked as `deleted` with a `deletion_version`, enabling
 *     version-aware flattening (i.e., reconstructing the document at
 *     any committed version).
 *   - Version tagging: Each chunk records the version at which it was
 *     created, supporting the server's version-based synchronization
 *     protocol.
 */

#include <stdbool.h>

/**
 * struct chunk - A single piece in the document's piece table.
 *
 * Each chunk represents a contiguous substring within the shared buffer.
 * Chunks form a singly linked list in document order.
 *
 * @offset:            Starting byte index into ChunkTable.buf
 * @length:            Number of bytes this chunk spans
 * @version:           Document version when this chunk was inserted
 * @next:              Pointer to the next chunk in document order
 * @deleted:           Whether this chunk has been soft-deleted
 * @deletion_version:  The version at which deletion takes effect
 */
typedef struct chunk {
    size_t offset;
    size_t length;
    uint64_t version;
    struct chunk *next;
    bool deleted;
    uint64_t deletion_version;
} chunk;

/**
 * struct ChunkTable - Manages the append-only buffer and chunk linked list.
 *
 * @buf:           Append-only buffer holding all inserted text content
 * @buf_size:      Current number of bytes used in the buffer
 * @buf_capacity:  Total allocated capacity of the buffer (doubles on overflow)
 * @head:          Head of the chunk linked list (document order)
 */
typedef struct {
    char *buf;
    size_t buf_size;
    size_t buf_capacity;
    chunk *head;
} ChunkTable;

/**
 * struct document - Top-level document state.
 *
 * @ct:       The chunk table storing all document content
 * @version:  Current committed version number (starts at 0, incremented
 *            after each synchronization round)
 */
typedef struct {
    ChunkTable ct;
    uint64_t version;
} document;

#endif /* DOCUMENT_H */
