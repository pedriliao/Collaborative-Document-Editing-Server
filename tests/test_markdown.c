/**
 * test_markdown.c - Comprehensive unit tests for the Markdown document engine.
 *
 * Tests cover all functions declared in markdown.h:
 *   - Document lifecycle (init, free)
 *   - Edit commands (insert, delete)
 *   - Formatting commands (heading, bold, italic, code, blockquote,
 *     ordered/unordered list, horizontal rule, link, newline)
 *   - Version control and synchronization
 *   - Edge cases (empty doc, boundary positions, invalid inputs)
 *   - Ordered list auto-renumbering
 *   - Multiple operations in sequence
 *   - Memory safety (no leaks when run under ASAN/Valgrind)
 *
 * Build: gcc -Wall -Wextra -std=c11 -g -fsanitize=address -Ilibs \
 *        -o test_markdown tests/test_markdown.c source/markdown.c -lpthread
 *
 * Run:   ./test_markdown
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "markdown.h"
#include "document.h"

/* ========== Test Helpers ========== */

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_STR_EQ(doc, expected) do { \
    char *_flat = markdown_flatten(doc); \
    if (strcmp(_flat, expected) != 0) { \
        fprintf(stderr, "  FAIL [%s:%d]: expected \"%s\", got \"%s\"\n", \
                __func__, __LINE__, expected, _flat); \
        tests_failed++; \
        free(_flat); \
        return; \
    } \
    free(_flat); \
} while (0)

#define ASSERT_RC(rc, expected) do { \
    if ((rc) != (expected)) { \
        fprintf(stderr, "  FAIL [%s:%d]: expected rc=%d, got rc=%d\n", \
                __func__, __LINE__, (expected), (rc)); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define TEST_PASS() do { tests_passed++; printf("  PASS: %s\n", __func__); } while (0)

/** Create a fresh document, insert initial text, and commit to version 1. */
static document *make_doc(const char *initial_text) {
    document *doc = markdown_init();
    assert(doc != NULL);
    if (initial_text && strlen(initial_text) > 0) {
        markdown_insert(doc, 0, 0, initial_text);
        markdown_increment_version(doc);
    }
    return doc;
}

/* ========== 1. Document Lifecycle Tests ========== */

void test_init_and_free(void) {
    document *doc = markdown_init();
    assert(doc != NULL);
    assert(doc->version == 0);
    ASSERT_STR_EQ(doc, "");
    markdown_free(doc);
    TEST_PASS();
}

void test_init_empty_flatten(void) {
    document *doc = markdown_init();
    char *flat = markdown_flatten(doc);
    assert(flat != NULL);
    assert(strcmp(flat, "") == 0);
    free(flat);
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 2. Basic Insert Tests ========== */

void test_insert_single(void) {
    document *doc = markdown_init();
    int rc = markdown_insert(doc, 0, 0, "Hello");
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hello");
    markdown_free(doc);
    TEST_PASS();
}

void test_insert_at_beginning(void) {
    document *doc = make_doc("World");
    int rc = markdown_insert(doc, 1, 0, "Hello ");
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hello World");
    markdown_free(doc);
    TEST_PASS();
}

void test_insert_at_end(void) {
    document *doc = make_doc("Hello");
    int rc = markdown_insert(doc, 1, 5, " World");
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hello World");
    markdown_free(doc);
    TEST_PASS();
}

void test_insert_in_middle(void) {
    document *doc = make_doc("Hllo");
    int rc = markdown_insert(doc, 1, 1, "e");
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hello");
    markdown_free(doc);
    TEST_PASS();
}

void test_insert_multiple_same_version(void) {
    document *doc = markdown_init();
    markdown_insert(doc, 0, 0, "World");
    markdown_insert(doc, 0, 0, "Hello ");
    /* Before commit, flatten returns empty (version 0, chunks at version 1) */
    ASSERT_STR_EQ(doc, "");
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hello World");
    markdown_free(doc);
    TEST_PASS();
}

void test_insert_empty_string(void) {
    document *doc = make_doc("Hello");
    int rc = markdown_insert(doc, 1, 0, "");
    /* Inserting empty string should succeed but add nothing visible */
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hello");
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 3. Basic Delete Tests ========== */

void test_delete_from_start(void) {
    document *doc = make_doc("Hello World");
    int rc = markdown_delete(doc, 1, 0, 6);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "World");
    markdown_free(doc);
    TEST_PASS();
}

void test_delete_from_end(void) {
    document *doc = make_doc("Hello World");
    int rc = markdown_delete(doc, 1, 5, 6);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hello");
    markdown_free(doc);
    TEST_PASS();
}

void test_delete_from_middle(void) {
    document *doc = make_doc("Hello World");
    int rc = markdown_delete(doc, 1, 4, 3);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hellorld");
    markdown_free(doc);
    TEST_PASS();
}

void test_delete_entire_document(void) {
    document *doc = make_doc("Hello");
    int rc = markdown_delete(doc, 1, 0, 5);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "");
    markdown_free(doc);
    TEST_PASS();
}

void test_delete_beyond_end(void) {
    /* DEL should truncate at the end if deletion flows beyond document end */
    document *doc = make_doc("Hello");
    int rc = markdown_delete(doc, 1, 3, 100);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hel");
    markdown_free(doc);
    TEST_PASS();
}

void test_delete_zero_length(void) {
    document *doc = make_doc("Hello");
    int rc = markdown_delete(doc, 1, 2, 0);
    ASSERT_RC(rc, 0);
    ASSERT_STR_EQ(doc, "Hello");
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 4. Version Control Tests ========== */

void test_version_starts_at_zero(void) {
    document *doc = markdown_init();
    assert(doc->version == 0);
    markdown_free(doc);
    TEST_PASS();
}

void test_version_increment(void) {
    document *doc = markdown_init();
    assert(doc->version == 0);
    markdown_increment_version(doc);
    assert(doc->version == 1);
    markdown_increment_version(doc);
    assert(doc->version == 2);
    markdown_free(doc);
    TEST_PASS();
}

void test_insert_wrong_version(void) {
    document *doc = make_doc("Hello");
    int rc = markdown_insert(doc, 0, 0, "Bad");  /* Version 0, but doc is at version 1 */
    assert(rc != 0);  /* Should be rejected */
    ASSERT_STR_EQ(doc, "Hello");
    markdown_free(doc);
    TEST_PASS();
}

void test_flatten_before_commit(void) {
    document *doc = markdown_init();
    markdown_insert(doc, 0, 0, "Hello");
    /* Before commit, version 0 document should be empty */
    ASSERT_STR_EQ(doc, "");
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hello");
    markdown_free(doc);
    TEST_PASS();
}

void test_delete_before_commit_retains_content(void) {
    /* Spec example: flattening at old version still shows old content */
    document *doc = make_doc("Hello World");
    markdown_delete(doc, 1, 5, 6);
    /* Before committing deletion, flatten at version 1 still shows original */
    ASSERT_STR_EQ(doc, "Hello World");
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hello");
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 5. Heading Tests ========== */

void test_heading_level1(void) {
    document *doc = make_doc("Title");
    int rc = markdown_heading(doc, 1, 1, 0);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "# Title");
    markdown_free(doc);
    TEST_PASS();
}

void test_heading_level2(void) {
    document *doc = make_doc("Subtitle");
    int rc = markdown_heading(doc, 1, 2, 0);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "## Subtitle");
    markdown_free(doc);
    TEST_PASS();
}

void test_heading_level3(void) {
    document *doc = make_doc("Section");
    int rc = markdown_heading(doc, 1, 3, 0);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "### Section");
    markdown_free(doc);
    TEST_PASS();
}

void test_heading_auto_newline(void) {
    document *doc = make_doc("HelloTitle");
    int rc = markdown_heading(doc, 1, 1, 5);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hello\n# Title");
    markdown_free(doc);
    TEST_PASS();
}

void test_heading_invalid_level(void) {
    document *doc = make_doc("Text");
    int rc = markdown_heading(doc, 1, 4, 0);
    assert(rc != 0);
    int rc2 = markdown_heading(doc, 1, 0, 0);
    assert(rc2 != 0);
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 6. Bold Tests ========== */

void test_bold_basic(void) {
    document *doc = make_doc("Hello World");
    int rc = markdown_bold(doc, 1, 0, 5);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "**Hello** World");
    markdown_free(doc);
    TEST_PASS();
}

void test_bold_entire_text(void) {
    document *doc = make_doc("text");
    int rc = markdown_bold(doc, 1, 0, 4);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "**text**");
    markdown_free(doc);
    TEST_PASS();
}

void test_bold_invalid_range(void) {
    document *doc = make_doc("Hello");
    int rc = markdown_bold(doc, 1, 5, 3);  /* start >= end */
    assert(rc != 0);
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 7. Italic Tests ========== */

void test_italic_basic(void) {
    document *doc = make_doc("Hello World");
    int rc = markdown_italic(doc, 1, 6, 11);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hello *World*");
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 8. Code Tests ========== */

void test_code_basic(void) {
    document *doc = make_doc("print hello");
    int rc = markdown_code(doc, 1, 0, 5);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "`print` hello");
    markdown_free(doc);
    TEST_PASS();
}

void test_code_invalid_range(void) {
    document *doc = make_doc("Hello");
    int rc = markdown_code(doc, 1, 3, 3);  /* start == end */
    assert(rc != 0);
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 9. Blockquote Tests ========== */

void test_blockquote_at_start(void) {
    document *doc = make_doc("Quote text");
    int rc = markdown_blockquote(doc, 1, 0);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "> Quote text");
    markdown_free(doc);
    TEST_PASS();
}

void test_blockquote_auto_newline(void) {
    document *doc = make_doc("TextQuote");
    int rc = markdown_blockquote(doc, 1, 4);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Text\n> Quote");
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 10. Unordered List Tests ========== */

void test_unordered_list_at_start(void) {
    document *doc = make_doc("Item");
    int rc = markdown_unordered_list(doc, 1, 0);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "- Item");
    markdown_free(doc);
    TEST_PASS();
}

void test_unordered_list_auto_newline(void) {
    document *doc = make_doc("TextItem");
    int rc = markdown_unordered_list(doc, 1, 4);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Text\n- Item");
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 11. Ordered List Tests ========== */

void test_ordered_list_first_item(void) {
    document *doc = make_doc("Item");
    int rc = markdown_ordered_list(doc, 1, 0);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "1. Item");
    markdown_free(doc);
    TEST_PASS();
}

void test_ordered_list_renumber(void) {
    /* Inserting a new ordered list item should renumber subsequent items */
    document *doc = make_doc("1. tomato\n2. cheese burger\n3. asparagus");
    /* Insert ORDERED_LIST between "cheese" and " burger" (position 20 = 'b' in burger) */
    int rc = markdown_ordered_list(doc, 1, 20);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    char *flat = markdown_flatten(doc);
    /* The list should be renumbered: 1. tomato, 2. cheese, 3.  burger, 4. asparagus */
    assert(strstr(flat, "1. tomato") != NULL);
    free(flat);
    markdown_free(doc);
    TEST_PASS();
}

void test_ordered_list_delete_renumber(void) {
    /* Deleting a list item should renumber remaining items.
     * String layout:
     *   "Things to buy\n1. yoyo\n2. switch\n3. ps5\n..."
     *    0             13 14     21 22       31 32
     * Delete "2. switch\n" = positions 22..31 (10 chars)
     */
    document *doc = make_doc("Things to buy\n1. yoyo\n2. switch\n3. ps5");
    int rc = markdown_delete(doc, 1, 22, 10);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    char *flat = markdown_flatten(doc);
    /* After deletion, "3. ps5" should be renumbered to "2. ps5" */
    assert(strstr(flat, "1. yoyo") != NULL);
    assert(strstr(flat, "ps5") != NULL);
    free(flat);
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 12. Horizontal Rule Tests ========== */

void test_horizontal_rule_at_start(void) {
    document *doc = make_doc("Text");
    int rc = markdown_horizontal_rule(doc, 1, 0);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "---\nText");
    markdown_free(doc);
    TEST_PASS();
}

void test_horizontal_rule_auto_newline(void) {
    document *doc = make_doc("Hello\nWorld");
    int rc = markdown_horizontal_rule(doc, 1, 5);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    char *flat = markdown_flatten(doc);
    /* Should have newline before --- and newline after */
    assert(strstr(flat, "Hello\n---\nWorld") != NULL);
    free(flat);
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 13. Link Tests ========== */

void test_link_basic(void) {
    document *doc = make_doc("Click here for info");
    int rc = markdown_link(doc, 1, 6, 10, "https://www.example.com");
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Click [here](https://www.example.com) for info");
    markdown_free(doc);
    TEST_PASS();
}

void test_link_invalid_range(void) {
    document *doc = make_doc("Hello");
    int rc = markdown_link(doc, 1, 5, 2, "https://example.com");
    assert(rc != 0);
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 14. Newline Tests ========== */

void test_newline_basic(void) {
    document *doc = make_doc("HelloWorld");
    int rc = markdown_newline(doc, 1, 5);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hello\nWorld");
    markdown_free(doc);
    TEST_PASS();
}

void test_newline_at_start(void) {
    document *doc = make_doc("Hello");
    int rc = markdown_newline(doc, 1, 0);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "\nHello");
    markdown_free(doc);
    TEST_PASS();
}

void test_newline_at_end(void) {
    document *doc = make_doc("Hello");
    int rc = markdown_newline(doc, 1, 5);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hello\n");
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 15. Combined Operations Tests ========== */

void test_insert_then_delete(void) {
    document *doc = make_doc("I love COMP2017");
    /* Insert " really" between "I " and "love" */
    markdown_insert(doc, 1, 2, "really ");
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "I really love COMP2017");
    /* Insert " so much" after COMP2017 */
    markdown_insert(doc, 2, 22, " so much");
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "I really love COMP2017 so much");
    markdown_free(doc);
    TEST_PASS();
}

void test_multiple_formatting(void) {
    document *doc = make_doc("Hello World");
    /* Bold "Hello" */
    markdown_bold(doc, 1, 0, 5);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "**Hello** World");
    /* Italic "World" (now at position 10-15) */
    markdown_italic(doc, 2, 10, 15);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "**Hello** *World*");
    markdown_free(doc);
    TEST_PASS();
}

void test_insert_delete_insert(void) {
    document *doc = make_doc("ABCDE");
    /* Delete "BCD" */
    markdown_delete(doc, 1, 1, 3);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "AE");
    /* Insert "XYZ" between A and E */
    markdown_insert(doc, 2, 1, "XYZ");
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "AXYZE");
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 16. Edge Cases ========== */

void test_large_insert(void) {
    document *doc = markdown_init();
    /* Build a large string */
    char large[2048];
    memset(large, 'A', sizeof(large) - 1);
    large[sizeof(large) - 1] = '\0';
    int rc = markdown_insert(doc, 0, 0, large);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    char *flat = markdown_flatten(doc);
    assert(strlen(flat) == 2047);
    free(flat);
    markdown_free(doc);
    TEST_PASS();
}

void test_many_small_inserts(void) {
    document *doc = markdown_init();
    for (int i = 0; i < 100; i++) {
        markdown_insert(doc, 0, 0, "x");
    }
    markdown_increment_version(doc);
    char *flat = markdown_flatten(doc);
    assert(strlen(flat) == 100);
    free(flat);
    markdown_free(doc);
    TEST_PASS();
}

void test_bold_then_delete_content(void) {
    document *doc = make_doc("Hello");
    markdown_bold(doc, 1, 0, 5);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "**Hello**");
    /* Delete the original text "Hello" (now at positions 2-7) */
    markdown_delete(doc, 2, 2, 5);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "****");
    markdown_free(doc);
    TEST_PASS();
}

void test_code_then_italic(void) {
    document *doc = make_doc("code text");
    markdown_code(doc, 1, 0, 4);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "`code` text");
    /* Italic the word "text" (positions 7-11) */
    markdown_italic(doc, 2, 7, 11);
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "`code` *text*");
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 17. Spec Examples ========== */

void test_spec_basic_insert(void) {
    /* From spec page 16: Basic Insert */
    document *doc = markdown_init();
    markdown_insert(doc, 0, 0, "World");
    markdown_insert(doc, 0, 0, "Hello ");
    /* Before commit: empty */
    ASSERT_STR_EQ(doc, "");
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hello World");
    markdown_free(doc);
    TEST_PASS();
}

void test_spec_basic_delete(void) {
    /* From spec page 16: Basic Delete */
    document *doc = make_doc("Hello World");
    markdown_delete(doc, 1, 5, 6);
    /* Before commit: still shows original */
    ASSERT_STR_EQ(doc, "Hello World");
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "Hello");
    markdown_free(doc);
    TEST_PASS();
}

void test_spec_multiple_insertions(void) {
    /* From spec page 16-17: Multiple Insertions Within a Sentence */
    document *doc = make_doc("I love COMP2017");
    /* Insert "really " between "I " and "love" */
    markdown_insert(doc, 1, 2, "really ");
    /* Insert " so much" after "COMP2017" */
    markdown_insert(doc, 1, 15, " so much");
    /* Before commit: still old version */
    ASSERT_STR_EQ(doc, "I love COMP2017");
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "I really love COMP2017 so much");
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 18. Newline with Ordered List ========== */

void test_newline_splits_ordered_list(void) {
    /* From spec: NEWLINE on an ordered list splits and renumbers */
    document *doc = make_doc("1. tomato\n2. cheese burger\n3. asparagus");
    /* Spec says: NEWLINE between "cheese" and "burger" */
    /* Position 20 is between "cheese" and " burger" in "2. cheese burger" */
    /* "1. tomato\n2. cheese burger\n3. asparagus"
     *  0         10         20         30        */
    int rc = markdown_newline(doc, 1, 20);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    char *flat = markdown_flatten(doc);
    /* Check that the list was split */
    assert(strstr(flat, "1. tomato") != NULL);
    free(flat);
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 19. Error Handling Tests ========== */

void test_heading_outdated_version(void) {
    document *doc = make_doc("Text");
    int rc = markdown_heading(doc, 0, 1, 0);  /* Outdated version */
    assert(rc == -3);
    markdown_free(doc);
    TEST_PASS();
}

void test_bold_out_of_bounds(void) {
    document *doc = make_doc("Hi");
    int rc = markdown_bold(doc, 1, 0, 10);  /* end > document length */
    assert(rc == -1);
    markdown_free(doc);
    TEST_PASS();
}

void test_heading_pos_out_of_bounds(void) {
    document *doc = make_doc("Hi");
    int rc = markdown_heading(doc, 1, 1, 100);  /* pos > document length */
    assert(rc == -1);
    markdown_free(doc);
    TEST_PASS();
}

void test_newline_pos_out_of_bounds(void) {
    document *doc = make_doc("Hi");
    int rc = markdown_newline(doc, 1, 100);
    assert(rc == -1);
    markdown_free(doc);
    TEST_PASS();
}

void test_code_out_of_bounds(void) {
    document *doc = make_doc("AB");
    int rc = markdown_code(doc, 1, 0, 10);
    assert(rc == -1);
    markdown_free(doc);
    TEST_PASS();
}

void test_italic_same_range(void) {
    document *doc = make_doc("Hello");
    int rc = markdown_italic(doc, 1, 2, 2);
    assert(rc != 0);  /* start == end should fail */
    markdown_free(doc);
    TEST_PASS();
}

void test_link_out_of_bounds(void) {
    document *doc = make_doc("Hello");
    int rc = markdown_link(doc, 1, 0, 100, "https://example.com");
    assert(rc == -1);
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 20. Horizontal Rule Edge Cases ========== */

void test_hr_middle_of_text(void) {
    document *doc = make_doc("AB");
    int rc = markdown_horizontal_rule(doc, 1, 1);
    ASSERT_RC(rc, 0);
    markdown_increment_version(doc);
    char *flat = markdown_flatten(doc);
    /* Should have A, newline, ---, newline, B */
    assert(strstr(flat, "---") != NULL);
    free(flat);
    markdown_free(doc);
    TEST_PASS();
}

/* ========== 21. Multiple Version Progression ========== */

void test_three_versions(void) {
    document *doc = markdown_init();
    /* Version 0 -> 1 */
    markdown_insert(doc, 0, 0, "A");
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "A");
    /* Version 1 -> 2 */
    markdown_insert(doc, 1, 1, "B");
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "AB");
    /* Version 2 -> 3 */
    markdown_insert(doc, 2, 2, "C");
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "ABC");
    markdown_free(doc);
    TEST_PASS();
}

void test_insert_across_versions(void) {
    /* Test sequential inserts across separate versions. */
    document *doc = make_doc("AC");
    /* Version 1 -> 2: insert "B" between A and C */
    markdown_insert(doc, 1, 1, "B");
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "ABC");
    /* Version 2 -> 3: insert "D" at end */
    markdown_insert(doc, 2, 3, "D");
    markdown_increment_version(doc);
    ASSERT_STR_EQ(doc, "ABCD");
    markdown_free(doc);
    TEST_PASS();
}

/* ========== Test Runner ========== */

int main(void) {
    printf("=== Markdown Engine Unit Tests ===\n\n");

    printf("[Lifecycle]\n");
    test_init_and_free();
    test_init_empty_flatten();

    printf("\n[Insert]\n");
    test_insert_single();
    test_insert_at_beginning();
    test_insert_at_end();
    test_insert_in_middle();
    test_insert_multiple_same_version();
    test_insert_empty_string();

    printf("\n[Delete]\n");
    test_delete_from_start();
    test_delete_from_end();
    test_delete_from_middle();
    test_delete_entire_document();
    test_delete_beyond_end();
    test_delete_zero_length();

    printf("\n[Version Control]\n");
    test_version_starts_at_zero();
    test_version_increment();
    test_insert_wrong_version();
    test_flatten_before_commit();
    test_delete_before_commit_retains_content();

    printf("\n[Heading]\n");
    test_heading_level1();
    test_heading_level2();
    test_heading_level3();
    test_heading_auto_newline();
    test_heading_invalid_level();

    printf("\n[Bold]\n");
    test_bold_basic();
    test_bold_entire_text();
    test_bold_invalid_range();

    printf("\n[Italic]\n");
    test_italic_basic();

    printf("\n[Code]\n");
    test_code_basic();
    test_code_invalid_range();

    printf("\n[Blockquote]\n");
    test_blockquote_at_start();
    test_blockquote_auto_newline();

    printf("\n[Unordered List]\n");
    test_unordered_list_at_start();
    test_unordered_list_auto_newline();

    printf("\n[Ordered List]\n");
    test_ordered_list_first_item();
    test_ordered_list_renumber();
    test_ordered_list_delete_renumber();

    printf("\n[Horizontal Rule]\n");
    test_horizontal_rule_at_start();
    test_horizontal_rule_auto_newline();

    printf("\n[Link]\n");
    test_link_basic();
    test_link_invalid_range();

    printf("\n[Newline]\n");
    test_newline_basic();
    test_newline_at_start();
    test_newline_at_end();
    test_newline_splits_ordered_list();

    printf("\n[Combined Operations]\n");
    test_insert_then_delete();
    test_multiple_formatting();
    test_insert_delete_insert();

    printf("\n[Edge Cases]\n");
    test_large_insert();
    test_many_small_inserts();
    test_bold_then_delete_content();
    test_code_then_italic();

    printf("\n[Spec Examples]\n");
    test_spec_basic_insert();
    test_spec_basic_delete();
    test_spec_multiple_insertions();

    printf("\n[Error Handling]\n");
    test_heading_outdated_version();
    test_bold_out_of_bounds();
    test_heading_pos_out_of_bounds();
    test_newline_pos_out_of_bounds();
    test_code_out_of_bounds();
    test_italic_same_range();
    test_link_out_of_bounds();
    test_hr_middle_of_text();

    printf("\n[Multiple Versions]\n");
    test_three_versions();
    test_insert_across_versions();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
