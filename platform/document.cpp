/*****************************************************************************
 * platform/document.cpp
 *
 * Multi-document backing store for Adobe-style IMG tabs. The rest of the port
 * still talks to the active document through g_doc, so switching tabs is just
 * repointing that global at another deque element.
 *****************************************************************************/
#include "document.h"
#include "img_format.h"

#include <deque>
#include <cstring>

static std::deque<Document> g_documents;
static int g_active_document = 0;

Document *g_doc = NULL;

static void document_set_defaults(Document *doc)
{
    if (!doc) return;
    memset(doc, 0, sizeof(*doc));
    doc->file_bufscr[0] = 0xFF;
    doc->file_bufscr[1] = 0xFF;
    doc->file_bufscr[2] = 0xFF;
    doc->file_bufscr[3] = 0xFF;
    doc->ilselected = -1;
    doc->il2selected = -1;
    doc->plselected = -1;
    doc->ilpalloaded = -1;
}

static void free_img_chain(void *head)
{
    IMG *img = (IMG *)head;
    while (img) {
        IMG *next = (IMG *)img->nxt_p;
        FreeImg(img);
        img = next;
    }
}

static void free_pal_chain(void *head)
{
    PAL *pal = (PAL *)head;
    while (pal) {
        PAL *next = (PAL *)pal->nxt_p;
        FreePal(pal);
        pal = next;
    }
}

void document_clear_contents(Document *doc)
{
    if (!doc) return;

    free_img_chain(doc->img_p);
    free_img_chain(doc->img2_p);
    free_pal_chain(doc->pal_p);
    if (doc->scrseqmem_p) free(doc->scrseqmem_p);
    if (doc->damtbl_p) free(doc->damtbl_p);

    document_set_defaults(doc);
}

static void document_repoint_active(void)
{
    if (g_documents.empty()) {
        g_documents.emplace_back();
        document_set_defaults(&g_documents.back());
        g_active_document = 0;
    }
    if (g_active_document < 0) g_active_document = 0;
    if (g_active_document >= (int)g_documents.size())
        g_active_document = (int)g_documents.size() - 1;
    g_doc = &g_documents[(size_t)g_active_document];
}

void document_init(void)
{
    for (Document &doc : g_documents) document_clear_contents(&doc);
    g_documents.clear();
    g_documents.emplace_back();
    document_set_defaults(&g_documents.back());
    g_active_document = 0;
    g_doc = &g_documents.front();
}

int document_tab_count(void)
{
    return (int)g_documents.size();
}

int document_active_index(void)
{
    return g_active_document;
}

Document *document_get(int idx)
{
    if (idx < 0 || idx >= (int)g_documents.size()) return NULL;
    return &g_documents[(size_t)idx];
}

Document *document_active(void)
{
    return g_doc;
}

int document_new_tab(void)
{
    g_documents.emplace_back();
    document_set_defaults(&g_documents.back());
    g_active_document = (int)g_documents.size() - 1;
    document_repoint_active();
    return g_active_document;
}

void document_set_active(int idx)
{
    if (idx < 0 || idx >= (int)g_documents.size()) return;
    g_active_document = idx;
    document_repoint_active();
}

void document_close_tab(int idx)
{
    if (idx < 0 || idx >= (int)g_documents.size()) return;

    document_clear_contents(&g_documents[(size_t)idx]);
    g_documents.erase(g_documents.begin() + idx);

    if (g_documents.empty()) {
        g_documents.emplace_back();
        document_set_defaults(&g_documents.back());
        g_active_document = 0;
    } else if (g_active_document > idx) {
        g_active_document--;
    } else if (g_active_document >= (int)g_documents.size()) {
        g_active_document = (int)g_documents.size() - 1;
    }

    document_repoint_active();
}
