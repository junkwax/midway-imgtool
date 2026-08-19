/*************************************************************
 * test/document_test.cpp
 *
 * Coverage for Document::uid as a stable identity across tab
 * close and reorder.
 *
 * This is the guarantee World View's row bindings rest on. Tabs
 * live in a std::deque that erases on close and is move-assigned
 * wholesale on reorder, so neither of the obvious ways to remember
 * "which file is this row showing" survives:
 *
 *   - a Document* is freed by the erase, and reading it afterwards
 *     returns recycled heap. Walking an IMG list off one of those
 *     is the 0xC0000005 in doc_get_img that prompted these tests.
 *   - a tab index survives the free but not the shift: closing a
 *     lower tab silently slides every higher index down one, so a
 *     remembered index quietly names its neighbour's file.
 *
 * The uid survives both, and these tests pin that down. They also
 * pin the pointer/index hazards themselves, so the reason the uid
 * exists cannot be refactored away by someone who only sees a
 * seemingly redundant lookup.
 *************************************************************/
#include "document.h"

#include <cstdio>
#include <cstring>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

/* Give each open tab a recognisable payload so a test can tell which
   file a lookup actually landed on, not merely that it landed. */
static void TagTab(int idx, const char *name)
{
    Document *doc = document_get(idx);
    if (!doc) return;
    snprintf(doc->fname_s, sizeof(doc->fname_s), "%s", name);
}

static const char *TabName(int idx)
{
    Document *doc = document_get(idx);
    return doc ? doc->fname_s : "<none>";
}

/* Rebuild a known 4-tab session: A B C D. */
static void ResetTabs(unsigned int uid_out[4])
{
    document_init();
    while (document_tab_count() > 1)
        document_close_tab(document_tab_count() - 1);

    static const char *names[4] = { "A", "B", "C", "D" };
    TagTab(0, names[0]);
    for (int i = 1; i < 4; i++) {
        document_new_tab();
        TagTab(i, names[i]);
    }
    for (int i = 0; i < 4; i++)
        uid_out[i] = document_uid(i);
}

static void TestUidsAreDistinctAndNonZero(void)
{
    unsigned int uid[4];
    ResetTabs(uid);

    CHECK(document_tab_count() == 4);
    for (int i = 0; i < 4; i++) {
        CHECK(uid[i] != 0);
        CHECK(document_index_of_uid(uid[i]) == i);
        for (int j = i + 1; j < 4; j++)
            CHECK(uid[i] != uid[j]);
    }

    /* The "no document" answers callers rely on to mean "unbound". */
    CHECK(document_uid(-1) == 0);
    CHECK(document_uid(4) == 0);
    CHECK(document_index_of_uid(0) == -1);
    CHECK(document_get(-1) == NULL);
    CHECK(document_get(4) == NULL);
}

/* Closing a lower tab shifts every higher index down. A remembered
   index therefore names the wrong file afterwards; the uid does not. */
static void TestCloseShiftsIndexButNotUid(void)
{
    unsigned int uid[4];
    ResetTabs(uid);

    const int remembered_index = 2;                 /* tab C */
    const unsigned int remembered_uid = uid[2];
    CHECK(strcmp(TabName(remembered_index), "C") == 0);

    document_close_tab(0);                          /* drop A */
    CHECK(document_tab_count() == 3);

    /* The hazard: the same index is now a different file. */
    CHECK(strcmp(TabName(remembered_index), "D") == 0);

    /* The fix: the uid still finds C, at its new index. */
    int now = document_index_of_uid(remembered_uid);
    CHECK(now == 1);
    CHECK(strcmp(TabName(now), "C") == 0);
    Document *doc = document_get(now);
    CHECK(doc != NULL && doc->uid == remembered_uid);
}

/* A row bound to a document the user closed must resolve to nothing,
   which is what lets callers bail instead of dereferencing. */
static void TestClosedDocumentBecomesUnresolvable(void)
{
    unsigned int uid[4];
    ResetTabs(uid);

    const unsigned int closed_uid = uid[1];         /* tab B */
    document_close_tab(1);

    CHECK(document_tab_count() == 3);
    CHECK(document_index_of_uid(closed_uid) == -1);
    CHECK(document_get(document_index_of_uid(closed_uid)) == NULL);

    /* Everything else is still reachable and still itself. */
    CHECK(strcmp(TabName(document_index_of_uid(uid[0])), "A") == 0);
    CHECK(strcmp(TabName(document_index_of_uid(uid[2])), "C") == 0);
    CHECK(strcmp(TabName(document_index_of_uid(uid[3])), "D") == 0);
}

/* Reorder move-assigns a whole new deque, so every element address
   changes even though nothing was closed. Indices are permuted;
   uids follow their content. */
static void TestReorderMovesContentNotIdentity(void)
{
    unsigned int uid[4];
    ResetTabs(uid);

    /* D C B A */
    const int order[4] = { 3, 2, 1, 0 };
    document_reorder(order, 4);

    CHECK(document_tab_count() == 4);
    CHECK(strcmp(TabName(0), "D") == 0);
    CHECK(strcmp(TabName(3), "A") == 0);

    CHECK(document_index_of_uid(uid[0]) == 3);   /* A moved to the end */
    CHECK(document_index_of_uid(uid[3]) == 0);   /* D moved to the front */
    for (int i = 0; i < 4; i++) {
        Document *doc = document_get(document_index_of_uid(uid[i]));
        CHECK(doc != NULL && doc->uid == uid[i]);
    }
}

/* A malformed permutation must be refused outright rather than
   half-applied, or the tabs and everything keyed to them diverge. */
static void TestReorderRejectsBadPermutation(void)
{
    unsigned int uid[4];
    ResetTabs(uid);

    const int dup[4] = { 0, 1, 1, 3 };
    document_reorder(dup, 4);
    CHECK(strcmp(TabName(1), "B") == 0);
    CHECK(strcmp(TabName(2), "C") == 0);

    const int out_of_range[4] = { 0, 1, 2, 9 };
    document_reorder(out_of_range, 4);
    CHECK(strcmp(TabName(3), "D") == 0);

    const int wrong_count[3] = { 0, 1, 2 };
    document_reorder(wrong_count, 3);
    CHECK(document_tab_count() == 4);

    for (int i = 0; i < 4; i++)
        CHECK(document_index_of_uid(uid[i]) == i);
}

/* Emptying a document keeps the tab, so it must keep its identity —
   a row bound to it should still resolve, just to an empty file. */
static void TestClearContentsKeepsUid(void)
{
    unsigned int uid[4];
    ResetTabs(uid);

    Document *doc = document_get(2);
    CHECK(doc != NULL);
    document_clear_contents(doc);

    CHECK(document_uid(2) == uid[2]);
    CHECK(document_index_of_uid(uid[2]) == 2);
}

/* Closing the last tab leaves a fresh empty document rather than an
   empty container, and that replacement is its own file: an old uid
   must not resolve onto it. */
static void TestClosingLastTabMintsNewIdentity(void)
{
    document_init();
    while (document_tab_count() > 1)
        document_close_tab(document_tab_count() - 1);

    unsigned int only = document_uid(0);
    document_close_tab(0);

    CHECK(document_tab_count() == 1);
    CHECK(document_uid(0) != only);
    CHECK(document_index_of_uid(only) == -1);
}

int main(void)
{
    TestUidsAreDistinctAndNonZero();
    TestCloseShiftsIndexButNotUid();
    TestClosedDocumentBecomesUnresolvable();
    TestReorderMovesContentNotIdentity();
    TestReorderRejectsBadPermutation();
    TestClearContentsKeepsUid();
    TestClosingLastTabMintsNewIdentity();

    if (g_fails) {
        std::fprintf(stderr, "document_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("document_test: all checks passed\n");
    return 0;
}
