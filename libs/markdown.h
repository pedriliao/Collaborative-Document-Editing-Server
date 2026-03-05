#ifndef MARKDOWN_H
#define MARKDOWN_H

/**
 * markdown.h - Public API for the Markdown document engine.
 *
 * This module provides all document manipulation functions used by both
 * the server and client. Functions are organized into three categories:
 *
 *   1. Edit Commands:     INSERT and DEL for raw text manipulation
 *   2. Formatting Commands: Markdown-aware formatting (headings, bold,
 *                          italic, lists, code, blockquotes, links, rules)
 *   3. Utilities:         Document flattening, printing, and versioning
 *
 * Return codes for edit/formatting functions:
 *   SUCCESS            =  0   Command executed successfully
 *   INVALID_CURSOR_POS = -1   Cursor position out of bounds
 *   DELETED_POSITION   = -2   Cursor points to deleted text
 *   OUTDATED_VERSION   = -3   Command targets a stale version
 */

#include <stdio.h>
#include <stdint.h>
#include "document.h"

/* --- Lifecycle --- */

/** Initialize an empty document. Returns NULL on allocation failure. */
document *markdown_init(void);

/** Free all memory associated with the document (chunks, buffer, struct). */
void markdown_free(document *doc);

/* --- Edit Commands --- */

/** Insert text content at the given cursor position for the specified version. */
int markdown_insert(document *doc, uint64_t version, size_t pos, const char *content);

/** Delete len characters starting from pos for the specified version. */
int markdown_delete(document *doc, uint64_t version, size_t pos, size_t len);

/* --- Formatting Commands --- */

/** Insert a newline character at pos, handling ordered list renumbering. */
int markdown_newline(document *doc, uint64_t version, size_t pos);

/** Insert a Markdown heading (level 1-3) at pos with auto-newline. */
int markdown_heading(document *doc, uint64_t version, int level, size_t pos);

/** Wrap text between start and end with ** for bold formatting. */
int markdown_bold(document *doc, uint64_t version, size_t start, size_t end);

/** Wrap text between start and end with * for italic formatting. */
int markdown_italic(document *doc, uint64_t version, size_t start, size_t end);

/** Insert "> " blockquote prefix at pos with auto-newline. */
int markdown_blockquote(document *doc, uint64_t version, size_t pos);

/** Insert ordered list item at pos with automatic renumbering. */
int markdown_ordered_list(document *doc, uint64_t version, size_t pos);

/** Insert "- " unordered list marker at pos with auto-newline. */
int markdown_unordered_list(document *doc, uint64_t version, size_t pos);

/** Wrap text between start and end with backticks for inline code. */
int markdown_code(document *doc, uint64_t version, size_t start, size_t end);

/** Insert "---" horizontal rule at pos with auto-newlines. */
int markdown_horizontal_rule(document *doc, uint64_t version, size_t pos);

/** Wrap text [start,end) as Markdown link: [text](url). */
int markdown_link(document *doc, uint64_t version, size_t start, size_t end, const char *url);

/* --- Utilities --- */

/** Print the current document content to the given stream. */
void markdown_print(const document *doc, FILE *stream);

/** Flatten the document into a newly allocated string (caller must free). */
char *markdown_flatten(const document *doc);

/* --- Versioning --- */

/** Increment the document version by 1 (called after each sync round). */
void markdown_increment_version(document *doc);

#endif /* MARKDOWN_H */
