/**
 * markdown.c - Core document engine implementing a chunk-based piece table.
 *
 * Design Overview:
 *   This module manages a Markdown document using a piece-table-like structure.
 *   Instead of storing the document as a single mutable string (which requires
 *   O(n) shifts on every insert), we maintain:
 *
 *   1. An append-only text buffer: All inserted text is appended sequentially.
 *      Text is never moved or deleted from this buffer.
 *
 *   2. A linked list of "chunks": Each chunk stores an (offset, length) pair
 *      referencing a substring in the buffer, plus version metadata.
 *
 *   This approach provides O(1) insertions (append + pointer update) and
 *   supports version-aware operations through soft deletion (chunks are
 *   marked deleted rather than physically removed).
 *
 * Version Control:
 *   - Each chunk records the version at which it was created.
 *   - Deleted chunks record their deletion_version.
 *   - Flattening the document only includes chunks visible at the current version.
 *   - This enables the server to accept edits targeting the current version
 *     while preserving history for synchronization.
 *
 * Formatting:
 *   Markdown formatting is implemented as text insertions (e.g., "**" for bold,
 *   "# " for heading). The engine handles auto-newline insertion for block-level
 *   elements and automatic renumbering for ordered lists.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>
#include <inttypes.h>

#include "markdown.h"
#include "document.h"

#define INIT_ADD_BUF_CAP 32

/*
 * Internal flags for controlling version behavior during compound operations.
 *
 * suppress_version_bump: When true, inserted/deleted chunks use the current
 *   version instead of version+1. Used internally by formatting commands that
 *   need multiple atomic operations (e.g., ordered list renumbering).
 *
 * version_bump: When true, suppresses ordered list renumbering logic during
 *   recursive delete operations to prevent infinite loops.
 */
static bool suppress_version_bump = false;
static bool version_bump = false;

/* ========== Debug Utilities ========== */

/**
 * print_chunk_info - Print detailed information about a single chunk.
 *
 * Displays the chunk's offset, length, version, deletion status, and
 * text content. Used for debugging chunk operations.
 */
void print_chunk_info(chunk *cu, document *doc) {
    if (!cu || !doc) {
        printf("[print_chunk_info] Invalid input.\n");
        return;
    }
    printf("chunk info:\n");
    printf("  offset = %zu\n", cu->offset);
    printf("  length = %zu\n", cu->length);
    printf("  version = %" PRIu64 "\n", cu->version);
    printf("  deleted = %s\n", cu->deleted ? "true" : "false");
    printf("  deletion_version = %" PRIu64 "\n", cu->deletion_version);
    printf("  text = \"");
    fwrite(doc->ct.buf + cu->offset, 1, cu->length, stdout);
    printf("\"\n");
}

/**
 * debug_print_chunks - Dump the entire chunk linked list for debugging.
 *
 * Iterates through all chunks (including deleted ones) and prints their
 * metadata and text content. Useful for diagnosing piece table corruption.
 */
void debug_print_chunks(const document *doc) {
    if (!doc) {
        printf("[debug] document pointer is NULL\n");
        return;
    }
    printf("[debug] document version = %llu\n", (unsigned long long)doc->version);

    ChunkTable *ct = (ChunkTable *)&doc->ct;
    printf("[debug] buffer size = %zu, capacity = %zu\n", ct->buf_size, ct->buf_capacity);

    chunk *cur = ct->head;
    size_t index = 0;
    while (cur) {
        if (cur->offset + cur->length > ct->buf_size) {
            printf("  [!] chunk[%zu] has invalid bounds (offset=%zu, length=%zu)\n",
                   index, cur->offset, cur->length);
        }
        printf("  chunk[%zu] @%p: offset=%zu, length=%zu, version=%llu",
               index, (void *)cur, cur->offset, cur->length,
               (unsigned long long)cur->version);
        if (cur->deleted) {
            printf(" [DELETED at version %llu]",
                   (unsigned long long)cur->deletion_version);
        }
        printf("\n");

        size_t to_print = cur->length;
        if (to_print > 80) to_print = 80;
        printf("    text = \"%.*s\"%s\n",
               (int)to_print, ct->buf + cur->offset,
               cur->length > to_print ? "..." : "");
        cur = cur->next;
        index++;
    }
    if (index == 0) {
        printf("  [debug] no chunks in document\n");
    }
}

/* ========== Chunk Query Helpers ========== */

/**
 * is_number_chunk_from_chunk - Check if a chunk starts with an ordered list prefix.
 *
 * Tests whether the chunk's text begins with a pattern like "N. " where N
 * is a single digit. Used during ordered list renumbering.
 *
 * @doc: Document containing the buffer
 * @cu:  Chunk to examine
 * Returns: true if the chunk starts with a digit followed by ". "
 */
bool is_number_chunk_from_chunk(const document *doc, const chunk *cu) {
    if (!doc || !cu) return false;
    const char *text = doc->ct.buf + cu->offset;
    return isdigit((unsigned char)text[0]) && text[1] == '.' && text[2] == ' ';
}

/**
 * find_chunk_flatten - Locate the chunk containing a given position (visible only).
 *
 * Traverses the chunk list, considering only chunks that are visible at the
 * specified document version (not deleted, version <= doc_version). Returns
 * the chunk containing `pos` and sets *prev_out to the preceding chunk.
 *
 * Unlike find_chunk(), this function does NOT compute the offset within the
 * found chunk. It is used for ordered list renumbering where we need the
 * chunk itself rather than a precise split point.
 *
 * @ct:          Chunk table to search
 * @pos:         Target cursor position in the flattened document
 * @prev_out:    Output: pointer to the chunk immediately before the result
 * @doc_version: Version to use for visibility filtering
 * Returns: The chunk containing pos, or NULL if pos is past the end
 */
chunk *find_chunk_flatten(ChunkTable *ct, size_t pos, chunk **prev_out, uint64_t doc_version) {
    chunk *cu = ct->head;
    chunk *prev = NULL;
    size_t cur_pos = 0;

    while (cu) {
        bool visible = (cu->version <= doc_version)
            && (!cu->deleted || cu->deletion_version > doc_version);
        if (visible) {
            if (cur_pos + cu->length > pos) break;
            cur_pos += cu->length;
        }
        prev = cu;
        cu = cu->next;
    }

    *prev_out = prev;
    return cu;  /* NULL if pos is at/past end of document */
}

/**
 * try_replace_number - In-place replacement of an ordered list number prefix.
 *
 * During renumbering, if a chunk was recently replaced (old chunk deleted,
 * new chunk inserted at version+1), this function can update the new chunk's
 * content directly in the buffer instead of delete+insert.
 *
 * Verification steps:
 *   1. Find the visible chunk at `pos` (should be deleted = old version)
 *   2. Find its physical predecessor (should be the replacement chunk)
 *   3. Verify both are "N. " formatted with consecutive versions
 *   4. Overwrite the replacement chunk's content
 *
 * @doc:     Document instance
 * @pos:     Position of the list number in the flattened document
 * @new_str: New number string (e.g., "3. ")
 * Returns: true if replacement succeeded, false if conditions not met
 */
bool try_replace_number(document *doc, size_t pos, const char *new_str) {
    chunk *prev_logic;
    chunk *cu = find_chunk_flatten(&doc->ct, pos, &prev_logic, doc->version);
    if (!cu || !cu->deleted) return false;

    /* Find the physical predecessor of the deleted chunk */
    chunk *prev_real = prev_logic;
    while (prev_real && prev_real->next != cu) {
        prev_real = prev_real->next;
    }
    if (!prev_real) return false;

    chunk *real = cu;

    /* Verify the deleted chunk is a number prefix */
    const char *real_buf = doc->ct.buf + real->offset;
    if (!(isdigit(real_buf[0]) && real_buf[1] == '.' && real_buf[2] == ' '))
        return false;

    /* Verify the predecessor is the replacement chunk with version+1 */
    const char *prev_buf = doc->ct.buf + prev_real->offset;
    if (!(isdigit(prev_buf[0]) && prev_buf[1] == '.' && prev_buf[2] == ' '))
        return false;
    if (prev_real->deleted) return false;
    if (real->version + 1 != prev_real->version) return false;

    /* Perform in-place content replacement */
    memcpy(doc->ct.buf + prev_real->offset, new_str, strlen(new_str));
    return true;
}

/* ========== Document Flattening ========== */

/**
 * markdown_flatten - Reconstruct the document as a plain string.
 *
 * Iterates through all chunks and concatenates only those visible at the
 * current document version (version <= doc->version, not deleted at or
 * before doc->version).
 *
 * @doc: Document to flatten
 * Returns: Newly allocated string (caller must free), or NULL on failure
 */
char *markdown_flatten(const document *doc) {
    if (!doc) return NULL;

    /* Pass 1: Calculate total visible length */
    size_t total_length = 0;
    for (chunk *cu = doc->ct.head; cu; cu = cu->next) {
        if (cu->version <= doc->version &&
            !(cu->deleted && cu->deletion_version <= doc->version)) {
            total_length += cu->length;
        }
    }

    char *result = malloc(total_length + 1);
    if (!result) return NULL;

    /* Pass 2: Copy visible content */
    size_t pos = 0;
    for (chunk *cu = doc->ct.head; cu; cu = cu->next) {
        if (cu->version <= doc->version &&
            !(cu->deleted && cu->deletion_version <= doc->version)) {
            memcpy(result + pos, doc->ct.buf + cu->offset, cu->length);
            pos += cu->length;
        }
    }
    result[pos] = '\0';
    return result;
}

/**
 * conn_useful - Flatten the document as it will appear at version+1.
 *
 * Used during ordered list insertion to preview the document state after
 * pending operations are committed. Includes chunks up to version+1.
 *
 * @doc: Document to preview
 * Returns: Newly allocated string (caller must free), or NULL on failure
 */
char *conn_useful(const document *doc) {
    if (!doc) return NULL;

    size_t total_length = 0;
    for (chunk *cu = doc->ct.head; cu; cu = cu->next) {
        if (cu->version <= doc->version + 1 &&
            !(cu->deleted && cu->deletion_version <= doc->version + 1)) {
            total_length += cu->length;
        }
    }

    char *result = malloc(total_length + 1);
    if (!result) return NULL;

    size_t pos = 0;
    for (chunk *cu = doc->ct.head; cu; cu = cu->next) {
        if (cu->version <= doc->version + 1 &&
            !(cu->deleted && cu->deletion_version <= doc->version + 1)) {
            memcpy(result + pos, doc->ct.buf + cu->offset, cu->length);
            pos += cu->length;
        }
    }
    result[pos] = '\0';
    return result;
}

/* ========== Document Lifecycle ========== */

/**
 * markdown_init - Create and initialize an empty document.
 *
 * Allocates the document structure, initializes the append-only buffer
 * with a small initial capacity (32 bytes), and creates a sentinel
 * head chunk with zero length.
 *
 * Returns: Pointer to the new document, or NULL on allocation failure
 */
document *markdown_init(void) {
    document *init_docu = malloc(sizeof *init_docu);
    if (!init_docu) return NULL;

    init_docu->version = 0;

    /* Initialize the append-only buffer with small initial capacity */
    init_docu->ct.buf = malloc(INIT_ADD_BUF_CAP);
    init_docu->ct.buf_size = 0;
    init_docu->ct.buf_capacity = INIT_ADD_BUF_CAP;

    /* Create sentinel head chunk (zero-length placeholder) */
    init_docu->ct.head = malloc(sizeof(chunk));
    if (!init_docu->ct.buf || !init_docu->ct.head) {
        free(init_docu->ct.buf);
        free(init_docu);
        return NULL;
    }

    init_docu->ct.head->length = 0;
    init_docu->ct.head->offset = 0;
    init_docu->ct.head->version = 0;
    init_docu->ct.head->next = NULL;
    init_docu->ct.head->deleted = false;
    init_docu->ct.head->deletion_version = 0;

    return init_docu;
}

/**
 * markdown_free - Release all memory associated with a document.
 *
 * Frees every chunk in the linked list, the shared text buffer,
 * and the document structure itself.
 */
void markdown_free(document *doc) {
    chunk *cu = doc->ct.head;
    while (cu != NULL) {
        chunk *next = cu->next;
        free(cu);
        cu = next;
    }
    free(doc->ct.buf);
    free(doc);
}

/* ========== Internal Buffer & Chunk Operations ========== */

/**
 * append_to_buf - Append a string to the shared text buffer.
 *
 * Doubles the buffer capacity as needed to accommodate new content.
 * Returns the offset at which the string was stored.
 *
 * @ct: Chunk table containing the buffer
 * @s:  String to append
 * Returns: Starting offset of the appended string, or (size_t)-1 on failure
 */
static size_t append_to_buf(ChunkTable *ct, const char *s) {
    size_t len = strlen(s);
    if (ct->buf_size + len + 1 > ct->buf_capacity) {
        size_t new_cap = ct->buf_capacity * 2;
        while (new_cap < ct->buf_size + len + 1) new_cap *= 2;

        char *new_buf = realloc(ct->buf, new_cap);
        if (!new_buf) {
            perror("realloc");
            return (size_t)-1;
        }
        ct->buf = new_buf;
        ct->buf_capacity = new_cap;
    }

    size_t offset = ct->buf_size;
    memcpy(ct->buf + offset, s, len);
    ct->buf_size += len;
    return offset;
}

/**
 * find_chunk - Locate the chunk and intra-chunk offset for a cursor position.
 *
 * Traverses the chunk list considering only visible chunks at the given
 * version. Returns the chunk containing `pos` and computes the byte offset
 * within that chunk.
 *
 * @ct:              Chunk table to search
 * @pos:             Target cursor position
 * @prev_out:        Output: preceding chunk in the list
 * @offset_in_chunk: Output: byte offset within the found chunk
 * @doc_version:     Version to use for visibility filtering
 * Returns: The chunk containing pos, or NULL if pos is past the end
 */
chunk *find_chunk(ChunkTable *ct, size_t pos, chunk **prev_out,
                  size_t *offset_in_chunk, uint64_t doc_version) {
    chunk *cu = ct->head;
    chunk *prev = NULL;
    size_t cur_pos = 0;

    while (cu) {
        bool visible = (cu->version <= doc_version)
            && !(cu->deleted && cu->deletion_version <= doc_version);
        if (visible) {
            if (cur_pos + cu->length >= pos) break;
            cur_pos += cu->length;
        }
        prev = cu;
        cu = cu->next;
    }

    *prev_out = prev;
    if (!cu) return NULL;
    *offset_in_chunk = pos - cur_pos;
    return cu;
}

/**
 * split_chunk - Split a chunk into two at the given byte offset.
 *
 * Creates a new right-half chunk that inherits all properties from the
 * original (including deletion status). The original chunk is truncated
 * to cover only the first `split_off` bytes.
 *
 * @ct:        Chunk table (unused, kept for API consistency)
 * @c:         Chunk to split
 * @split_off: Byte offset within the chunk at which to split
 */
void split_chunk(ChunkTable *ct, chunk *c, size_t split_off) {
    (void)ct;
    if (!c || split_off == 0 || split_off >= c->length) return;

    chunk *right = malloc(sizeof *right);
    if (!right) return;

    /* Right half inherits all properties from the original */
    right->offset = c->offset + split_off;
    right->length = c->length - split_off;
    right->version = c->version;
    right->deleted = c->deleted;
    right->deletion_version = c->deletion_version;
    right->next = c->next;

    /* Left half retains only the first split_off bytes */
    c->length = split_off;
    c->next = right;
}

/* ========== Edit Commands ========== */

/**
 * markdown_insert - Insert text at a cursor position.
 *
 * Creates a new chunk referencing the appended text in the buffer and
 * splices it into the chunk list at the correct position.
 *
 * The new chunk's version is set to version+1 (pending commit) unless
 * suppress_version_bump is active (internal compound operations).
 *
 * @doc:     Document to modify
 * @version: Target document version (must match current)
 * @pos:     Cursor position (0 = before first character)
 * @content: Text to insert (must not contain newlines for INSERT command)
 * Returns: 0 on success, negative error code on failure
 */
int markdown_insert(document *doc, uint64_t version, size_t pos, const char *content) {
    if (!doc || !content) return -1;
    if (version != doc->version) return -2;

    /* Append text to the shared buffer */
    size_t len = strlen(content);
    size_t offset = append_to_buf(&doc->ct, content);
    if (offset == (size_t)-1) return -3;

    /* Create a new chunk referencing the appended text */
    chunk *new_chunk = malloc(sizeof *new_chunk);
    if (!new_chunk) return -4;

    new_chunk->offset = offset;
    new_chunk->length = len;
    new_chunk->deleted = false;
    new_chunk->deletion_version = 0;
    new_chunk->version = suppress_version_bump ? version : version + 1;
    new_chunk->next = NULL;

    /* Find the insertion point in the chunk list */
    chunk *prev;
    size_t off_in_chunk;
    chunk *cu = find_chunk(&doc->ct, pos, &prev, &off_in_chunk, doc->version);

    /* Split the target chunk if inserting in the middle */
    if (cu && off_in_chunk > 0 && off_in_chunk < cu->length) {
        split_chunk(&doc->ct, cu, off_in_chunk);
        prev = cu;
        cu = cu->next;
    }
    /* If at the end of a chunk, insert after it */
    else if (cu && off_in_chunk == cu->length) {
        prev = cu;
        cu = cu->next;
    }

    /* Splice the new chunk into the list */
    if (prev) {
        new_chunk->next = prev->next;
        prev->next = new_chunk;
    } else {
        new_chunk->next = doc->ct.head;
        doc->ct.head = new_chunk;
    }
    return 0;
}

/**
 * find_physical_prev - Find the physical predecessor of a chunk in the list.
 *
 * Traverses the linked list to find the chunk whose `next` pointer equals
 * the target. Used for chunk reorganization during renumbering.
 *
 * @head:   Head of the chunk list
 * @target: Target chunk to find the predecessor of
 * Returns: Predecessor chunk, or NULL if target is the head
 */
chunk *find_physical_prev(chunk *head, chunk *target) {
    if (!head || head == target) return NULL;
    chunk *cur = head;
    while (cur && cur->next != target) cur = cur->next;
    return cur;
}

/**
 * reorder_after_deletion - Renumber ordered list items after a deletion.
 *
 * Scans forward from `pos` in the flattened document, finding consecutive
 * lines that start with "N. " and renumbering them starting from `start_num`.
 *
 * Uses try_replace_number() for efficient in-place updates when possible,
 * falling back to delete+insert for chunks that can't be updated in place.
 *
 * @doc:       Document to modify
 * @pos:       Starting position in the flattened document
 * @start_num: First number to assign
 */
void reorder_after_deletion(document *doc, size_t pos, int start_num) {
    char *text = markdown_flatten(doc);
    size_t length = strlen(text);
    size_t scan = pos;
    int cur_num = start_num;

    while (scan < length) {
        size_t the_start = scan;

        /* Check if current line starts with an ordered list prefix */
        if (isdigit(text[the_start]) &&
            text[the_start + 1] == '.' &&
            text[the_start + 2] == ' ') {

            char newone[16];
            snprintf(newone, sizeof(newone), "%d. ", cur_num);

            /* Try in-place replacement first, fall back to delete+insert */
            if (!try_replace_number(doc, the_start, newone)) {
                markdown_delete(doc, doc->version, the_start, 3);
                markdown_insert(doc, doc->version, the_start, newone);
            }

            /* Re-flatten after modification and continue scanning */
            free(text);
            text = markdown_flatten(doc);
            length = strlen(text);
            scan = the_start + strlen(newone);
            cur_num++;
        } else {
            /* Skip to the next line */
            while (scan < length && text[scan] != '\n') scan++;
            scan++;
        }
    }
    free(text);
}

/**
 * markdown_delete - Delete characters from the document.
 *
 * Soft-deletes chunks (or portions of chunks) covering the range
 * [pos, pos+len). Chunks are split as needed to precisely mark only
 * the targeted bytes as deleted.
 *
 * After deletion, checks if ordered list renumbering is needed and
 * triggers reorder_after_deletion() if applicable.
 *
 * @doc:     Document to modify
 * @version: Target document version
 * @pos:     Starting cursor position
 * @len:     Number of characters to delete
 * Returns: 0 on success, negative error code on failure
 */
int markdown_delete(document *doc, uint64_t version, size_t pos, size_t len) {
    if (!doc) return -1;
    if (!suppress_version_bump) {
        if (version != doc->version) return -2;
    }
    if (len == 0) return 0;

    size_t del_start = pos;
    size_t del_end = pos + len;
    size_t cur_pos = 0;
    chunk *prev = NULL;
    chunk *cu = doc->ct.head;

    (void)prev;

    while (cu && cur_pos < del_end) {
        /* Skip chunks entirely before the deletion range */
        if (cur_pos + cu->length < del_start && cu->version <= version) {
            prev = cu;
            cur_pos += cu->length;
            cu = cu->next;
            continue;
        }

        /* Skip chunks from future versions */
        if (!suppress_version_bump && cu->version > version) {
            prev = cu;
            cu = cu->next;
            continue;
        }

        /* Compute overlap between chunk [chunk_start, chunk_end) and deletion range */
        size_t chunk_start = cur_pos;
        size_t chunk_end = cur_pos + cu->length;
        size_t over_start = (del_start <= chunk_start) ? chunk_start : del_start;
        size_t over_end = (del_end >= chunk_end) ? chunk_end : del_end;
        size_t check = over_start - chunk_start;
        size_t remove_len = over_end - over_start;

        if (remove_len == 0) {
            prev = cu;
            cur_pos += cu->length;
            cu = cu->next;
            continue;
        }

        /* Case 1: Entire chunk is deleted */
        if (check == 0 && remove_len == cu->length) {
            cu->deleted = true;
            cu->deletion_version = suppress_version_bump ? version : version + 1;
            prev = cu;
            cur_pos += cu->length;
            cu = cu->next;
        }
        /* Case 2: Delete from the beginning of the chunk */
        else if (check == 0) {
            split_chunk(&doc->ct, cu, remove_len);
            cu->deleted = true;
            cu->deletion_version = suppress_version_bump ? version : version + 1;
            prev = cu;
            cur_pos += remove_len;
            cu = cu->next;
        }
        /* Case 3: Delete from the end of the chunk */
        else if (check + remove_len >= cu->length) {
            split_chunk(&doc->ct, cu, check);
            chunk *del_chunk = cu->next;
            del_chunk->deleted = true;
            del_chunk->deletion_version = suppress_version_bump ? version : version + 1;
            prev = del_chunk;
            cur_pos += cu->length + remove_len;
            cu = del_chunk->next;
        }
        /* Case 4: Delete from the middle of the chunk */
        else {
            split_chunk(&doc->ct, cu, check);
            chunk *mid = cu->next;
            split_chunk(&doc->ct, mid, remove_len);
            mid->deleted = true;
            mid->deletion_version = suppress_version_bump ? version : version + 1;
            prev = mid;
            cur_pos = over_end;
            cu = mid->next;
        }
    }

    /* Post-deletion: check if ordered list renumbering is needed */
    if (!version_bump) {
        char *flat = markdown_flatten(doc);
        if (flat) {
            size_t total = strlen(flat);
            size_t line_start = pos;
            chunk *prev_real;
            find_chunk_flatten(&doc->ct, line_start, &prev_real, doc->version);
            int next_number = -1;

            /* Check if a list number chunk was exposed by this deletion */
            if (prev_real && !prev_real->deleted &&
                is_number_chunk_from_chunk(doc, prev_real)) {
                prev_real->deleted = true;
                prev_real->deletion_version = version;
                const char *text = doc->ct.buf + prev_real->offset;
                next_number = text[0] - '0';
            }

            /* Find the start of the next line */
            while (line_start < total && flat[line_start] != '\n') line_start++;
            line_start++;
            if (line_start >= total) {
                free(flat);
                return 0;
            }

            /* If the next line is a numbered list item, trigger renumbering */
            if (isdigit(flat[line_start]) &&
                flat[line_start + 1] == '.' &&
                flat[line_start + 2] == ' ') {
                if (next_number == -1) {
                    next_number = flat[line_start] - '0';
                    next_number -= 1;
                }
                if (next_number > 1) {
                    version_bump = true;
                    reorder_after_deletion(doc, line_start, next_number);
                    version_bump = false;
                }
            }
            free(flat);
        }
    }
    return 0;
}

/**
 * is_range_fully_deleted - Check if both endpoints fall in deleted regions.
 *
 * Used by formatting commands (BOLD, ITALIC, CODE, LINK) to detect
 * DELETED_POSITION errors before applying formatting.
 *
 * @doc:   Document to check
 * @start: Start cursor position
 * @end:   End cursor position
 * Returns: true if both positions are in deleted or invisible chunks
 */
bool is_range_fully_deleted(document *doc, size_t start, size_t end) {
    if (!doc || start > end) return false;

    chunk *start_prev, *end_prev;
    chunk *start_chunk = find_chunk_flatten(&doc->ct, start, &start_prev, doc->version);
    chunk *end_chunk = find_chunk_flatten(&doc->ct, end, &end_prev, doc->version);

    bool start_deleted = !start_chunk || start_chunk->deleted;
    bool end_deleted = !end_chunk || end_chunk->deleted;
    return start_deleted && end_deleted;
}

/* ========== Formatting Commands ========== */

/**
 * markdown_newline - Insert a newline character with ordered list handling.
 *
 * Inserts '\n' at the given position. If the newline splits an ordered list
 * item, renumbers subsequent items to maintain correct numbering.
 *
 * @doc:     Document to modify
 * @version: Target version (must match current)
 * @pos:     Cursor position for the newline
 * Returns: 0 on success, negative error code on failure
 */
int markdown_newline(document *doc, uint64_t version, size_t pos) {
    if (!doc) return -4;
    if (doc->version != version) return -3;

    char *text = markdown_flatten(doc);
    if (!text) return -4;
    size_t length = strlen(text);
    if (pos > length) {
        free(text);
        return -1;
    }
    free(text);

    /* Insert the newline character */
    int rc = markdown_insert(doc, version, pos, "\n");
    if (rc < 0) return rc;

    /* Check if ordered list renumbering is needed after the split */
    char *flat = markdown_flatten(doc);
    size_t total = strlen(flat);
    size_t line_start = pos;

    chunk *prev_real;
    find_chunk_flatten(&doc->ct, line_start, &prev_real, doc->version);
    int next_number = -1;

    if (prev_real && !prev_real->deleted &&
        is_number_chunk_from_chunk(doc, prev_real)) {
        prev_real->deleted = true;
        prev_real->deletion_version = version;
        const char *text = doc->ct.buf + prev_real->offset;
        next_number = text[0] - '0';
    }

    while (line_start < total && flat[line_start] != '\n') line_start++;
    line_start++;
    if (line_start >= total) {
        free(flat);
        return 0;
    }

    if (isdigit(flat[line_start]) &&
        flat[line_start + 1] == '.' &&
        flat[line_start + 2] == ' ') {
        version_bump = true;
        next_number = 1;
        reorder_after_deletion(doc, line_start, next_number);
        version_bump = false;
    }
    free(flat);
    return 0;
}

/**
 * markdown_heading - Insert a Markdown heading at the given position.
 *
 * Inserts "#", "##", or "###" followed by a space. Automatically prepends
 * a newline if the position is not at the start of a line.
 *
 * @doc:     Document to modify
 * @version: Target version
 * @level:   Heading level (1-3)
 * @pos:     Cursor position for the heading
 * Returns: 0 on success, negative error code on failure
 */
int markdown_heading(document *doc, uint64_t version, int level, size_t pos) {
    if (!doc) return -4;
    if (level < 1 || level > 3) return -2;
    if (version != doc->version) return -3;

    char *text = markdown_flatten(doc);
    if (!text) return -4;
    size_t length = strlen(text);
    if (pos > length) {
        free(text);
        return -1;
    }

    /* Build the heading prefix: "# ", "## ", or "### " */
    char heading[10];
    memset(heading, '#', level);
    heading[level] = ' ';
    heading[level + 1] = '\0';

    int rc = markdown_insert(doc, version, pos, heading);
    if (rc < 0) return rc;

    /* Auto-insert newline before heading if not at line start */
    if (pos > 0 && text[pos - 1] != '\n') {
        markdown_insert(doc, version, pos, "\n");
    }
    free(text);
    return rc;
}

/**
 * markdown_bold - Apply bold formatting (**text**) to a range.
 *
 * @doc:     Document to modify
 * @version: Target version
 * @start:   Start position (before first character to bold)
 * @end:     End position (after last character to bold)
 * Returns: 0 on success, negative error code on failure
 */
int markdown_bold(document *doc, uint64_t version, size_t start, size_t end) {
    if (!doc) return -4;
    if (start >= end) return -1;
    if (is_range_fully_deleted(doc, start, end)) return -2;
    if (version != doc->version) return -3;

    char *text = markdown_flatten(doc);
    if (!text) return -4;
    size_t length = strlen(text);
    if (end > length) {
        free(text);
        return -1;
    }
    free(text);

    /* Insert closing ** first (so start position isn't shifted) */
    int check = markdown_insert(doc, version, end, "**");
    if (check < 0) return check;
    return markdown_insert(doc, version, start, "**");
}

/**
 * markdown_italic - Apply italic formatting (*text*) to a range.
 */
int markdown_italic(document *doc, uint64_t version, size_t start, size_t end) {
    if (!doc) return -4;
    if (start >= end) return -1;
    if (is_range_fully_deleted(doc, start, end)) return -2;
    if (version != doc->version) return -3;

    char *text = markdown_flatten(doc);
    if (!text) return -4;
    size_t length = strlen(text);
    if (end > length) {
        free(text);
        return -1;
    }
    free(text);

    int rc = markdown_insert(doc, version, end, "*");
    if (rc < 0) return rc;
    return markdown_insert(doc, version, start, "*");
}

/**
 * markdown_blockquote - Insert blockquote formatting ("> ") at the position.
 *
 * Automatically prepends a newline if not at the start of a line.
 */
int markdown_blockquote(document *doc, uint64_t version, size_t pos) {
    if (!doc) return -4;
    if (version != doc->version) return -3;

    char *text = markdown_flatten(doc);
    if (!text) return -4;
    size_t length = strlen(text);
    if (pos > length) {
        free(text);
        return -1;
    }

    int rc = markdown_insert(doc, version, pos, "> ");
    if (rc != 0) return rc;

    /* Auto-insert newline before blockquote if not at line start */
    if (pos > 0 && text[pos - 1] != '\n') {
        markdown_insert(doc, version, pos, "\n");
        pos++;
    }
    free(text);
    return rc;
}

/**
 * markdown_ordered_list - Insert an ordered list item with auto-renumbering.
 *
 * Determines the correct number based on surrounding list items:
 *   - If at or after an existing "N. " item, uses N+1
 *   - If at the start of a new list, uses 1
 *
 * After insertion, renumbers all subsequent ordered list items to maintain
 * a consistent sequence.
 *
 * @doc:     Document to modify
 * @version: Target version
 * @pos:     Cursor position for the new list item
 * Returns: 0 on success, negative error code on failure
 */
int markdown_ordered_list(document *doc, uint64_t version, size_t pos) {
    if (version != doc->version) return -3;

    char *text = conn_useful(doc);
    size_t length = strlen(text);
    if (pos > length) return -1;

    /* Walk backward to find the start of the current line */
    size_t prev_line_end = pos;
    while (prev_line_end > 0 && text[prev_line_end - 1] != '\n') {
        prev_line_end--;
    }

    /* Determine the previous number in the ordered list */
    int prev_num;
    if (isdigit(text[prev_line_end]) &&
        text[prev_line_end + 1] == '.' &&
        text[prev_line_end + 2] == ' ') {
        prev_num = text[prev_line_end] - '0';
        size_t check_in_list = pos;
        while (check_in_list < length && text[check_in_list] == '\n') {
            check_in_list++;
        }
        check_in_list++;
        if (isdigit(text[prev_line_end]) &&
            text[prev_line_end + 1] == '.' &&
            text[prev_line_end + 2] == ' ') {
            /* Confirmed: we're inside an existing ordered list */
        }
    } else {
        /* Check the line above for an ordered list prefix */
        if (prev_line_end > 0) prev_line_end--;
        size_t line_start = prev_line_end;
        while (line_start > 0) {
            if (text[line_start - 1] == '\n') break;
            line_start--;
        }

        if (isdigit(text[line_start]) &&
            text[line_start + 1] == '.' &&
            text[line_start + 2] == ' ') {
            prev_num = text[line_start] - '0';
        } else {
            prev_num = 0;  /* Start a new list */
        }
    }

    if (prev_num >= 9) return -1;  /* Max 9 ordered list items */

    int num = prev_num + 1;
    char buf[16];

    /* Insert the list prefix, adding a newline if not at line start */
    if (pos > 0 && text[pos - 1] != '\n') {
        sprintf(buf, "\n%d. ", num);
        markdown_insert(doc, version, pos, buf);
    } else {
        sprintf(buf, "%d. ", num);
        markdown_insert(doc, version, pos, buf);
    }
    free(text);

    /* Renumber all subsequent ordered list items */
    size_t scan = pos + strlen(buf);
    int cur_num = num + 1;
    text = markdown_flatten(doc);
    length = strlen(text);

    while (scan < length) {
        size_t the_start = scan;
        /* Skip to the next line */
        while (the_start < length && text[the_start] != '\n') the_start++;
        if (the_start >= length) break;
        the_start++;

        /* Check if the next line is an ordered list item */
        if (isdigit(text[the_start]) &&
            text[the_start + 1] == '.' &&
            text[the_start + 2] == ' ') {

            char newone[16];
            sprintf(newone, "%d. ", cur_num);

            /* Try in-place update, fall back to delete+insert */
            if (!try_replace_number(doc, the_start, newone)) {
                markdown_delete(doc, version, the_start, 3);
                markdown_insert(doc, version, the_start, newone);
            }

            free(text);
            text = markdown_flatten(doc);
            length = strlen(text);
            cur_num++;
            scan = the_start + strlen(newone);
        } else {
            break;  /* Stop at non-list content */
        }
    }
    free(text);
    suppress_version_bump = false;
    return 0;
}

/**
 * markdown_unordered_list - Insert an unordered list marker ("- ") at the position.
 *
 * Automatically prepends a newline if not at the start of a line.
 */
int markdown_unordered_list(document *doc, uint64_t version, size_t pos) {
    if (!doc) return -1;
    if (version != doc->version) return -2;

    char *text = markdown_flatten(doc);
    if (!text) return -4;

    bool line = false;
    if (pos > 0 && text[pos - 1] != '\n') line = true;
    free(text);

    int rc = markdown_insert(doc, version, pos, "- ");
    if (rc != 0) return rc;

    if (line) {
        rc = markdown_insert(doc, version, pos, "\n");
        if (rc != 0) return rc;
    }
    return rc;
}

/**
 * markdown_code - Apply inline code formatting (`text`) to a range.
 */
int markdown_code(document *doc, uint64_t version, size_t start, size_t end) {
    if (!doc) return -4;
    if (start >= end) return -1;
    if (is_range_fully_deleted(doc, start, end)) return -2;
    if (version != doc->version) return -3;

    char *text = markdown_flatten(doc);
    if (!text) return -4;
    size_t length = strlen(text);
    if (end > length) {
        free(text);
        return -1;
    }
    free(text);

    int rc = markdown_insert(doc, version, end, "`");
    if (rc < 0) return rc;
    return markdown_insert(doc, version, start, "`");
}

/**
 * markdown_horizontal_rule - Insert a horizontal rule ("---") with auto-newlines.
 *
 * Ensures proper block-level formatting by adding newlines before and after
 * the rule as needed.
 */
int markdown_horizontal_rule(document *doc, uint64_t version, size_t pos) {
    if (!doc) return -4;
    if (version != doc->version) return -3;

    char *text = markdown_flatten(doc);
    if (!text) return -4;

    bool line = false;
    if (pos > 0 && text[pos - 1] != '\n') line = true;

    int rc;
    /* Append newline after rule if next char isn't already a newline */
    if (text[pos] != '\n') {
        rc = markdown_insert(doc, version, pos, "---\n");
        if (rc != 0) return rc;
    } else {
        rc = markdown_insert(doc, version, pos, "---");
        if (rc != 0) return rc;
    }
    free(text);

    /* Prepend newline before rule if not at line start */
    if (line) {
        rc = markdown_insert(doc, version, pos, "\n");
        if (rc != 0) return rc;
        pos += 1;
    }
    return rc;
}

/**
 * markdown_link - Wrap text as a Markdown hyperlink: [text](url).
 *
 * Inserts "[" before start, "](url)" after end.
 *
 * @doc:     Document to modify
 * @version: Target version
 * @start:   Start of the link text
 * @end:     End of the link text
 * @url:     URL string
 * Returns: 0 on success, negative error code on failure
 */
int markdown_link(document *doc, uint64_t version, size_t start, size_t end,
                  const char *url) {
    if (!doc || !url) return -4;
    if (start >= end) return -1;
    if (is_range_fully_deleted(doc, start, end)) return -2;
    if (version != doc->version) return -3;

    char *text = markdown_flatten(doc);
    if (!text) return -4;
    size_t length = strlen(text);
    if (end > length) {
        free(text);
        return -1;
    }
    free(text);

    /* Build link suffix: "](url)" - inserted in reverse order */
    int rc = markdown_insert(doc, version, end, ")");
    if (rc < 0) return rc;
    rc = markdown_insert(doc, version, end, url);
    if (rc < 0) return rc;
    rc = markdown_insert(doc, version, end, "](");
    if (rc < 0) return rc;
    rc = markdown_insert(doc, version, start, "[");
    return rc;
}

/* ========== Utilities ========== */

/**
 * markdown_print - Print the document content to a file stream.
 */
void markdown_print(const document *doc, FILE *stream) {
    if (!doc || !stream) return;
    char *text = markdown_flatten(doc);
    if (!text) return;
    fprintf(stream, "%s", text);
    free(text);
}

/**
 * markdown_increment_version - Advance the document to the next version.
 *
 * Called after each synchronization round to commit pending edits.
 * Chunks created with version+1 become visible after this call.
 */
void markdown_increment_version(document *doc) {
    doc->version += 1;
}
