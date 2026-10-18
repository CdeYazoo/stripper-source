/** vim: set ts=4 sw=4 et tw=99:
 *
 * === Stripper for Metamod:Source ===
 * Copyright (C) 2005-2009 David "BAILOPAN" Anderson
 * No warranties of any kind.
 * Based on the original concept of Stripper2 by botman
 *
 * License: see LICENSE.TXT
 * ===================================
 */
#ifndef _INCLUDE_STRIP_PARSER_H
#define _INCLUDE_STRIP_PARSER_H

#include <string>
#include <list>
#include <stack>
#include <vector>
#include "pcre2.h"

struct CACHEABLE
{
#if defined _DEBUG
    CACHEABLE() : marked(true), last_alloc(0), last_free(0) { };
    bool marked;
    int last_alloc;
    int last_free;
#endif
};

enum pair_kind
{
    PK_LITERAL,     /* whole-value, case-insensitive compare against val */
    PK_EXACT,       /* simple pattern: ^literal$   (val holds lowercase literal) */
    PK_PREFIX,      /* simple pattern: ^literal    */
    PK_SUFFIX,      /* simple pattern: literal$    */
    PK_CONTAINS,    /* simple pattern: literal     */
    PK_REGEX        /* anything else: real PCRE2 (re/match_data set) */
};

struct parse_pair
{
    parse_pair() : kid(-1), kind(PK_LITERAL), jit(false), re(NULL), match_data(NULL) { }

    std::string key;
    std::string val;
    int kid;            /* interned, case-insensitive key id */
    pair_kind kind;
    bool jit;           /* re was successfully JIT compiled */
    pcre2_code *re;
    pcre2_match_data *match_data;
};

struct ent_prop : public CACHEABLE
{
    ent_prop() : kid(-1) { }

    std::string key;
    std::string val;
    int kid;            /* interned, case-insensitive key id */
};

struct replace_prop
{
    std::list<parse_pair *> match;
    std::list<parse_pair *> to_replace;
    std::list<parse_pair *> to_remove;
    std::list<parse_pair *> to_insert;
};

/* A queued remove:/modify: block. Consecutive ops are applied in a single
 * pass over the entity list; see Stripper::RunBatch.
 */
struct pending_op
{
    pending_op() : modify(false) { }

    bool modify;
    std::vector<parse_pair *> match;    /* cheap comparisons ordered first */
    std::list<parse_pair *> to_remove;
    std::list<parse_pair *> to_replace;
    std::list<parse_pair *> to_insert;
    std::list<parse_pair *> owned;      /* every pair above, for recycling */
};

class Stripper
{
public:
    Stripper();
    ~Stripper();
public:
    void SetEntityList(const char *ents);
    /* Returns false if the file could not be opened. */
    bool ApplyFileFilter(const char *file);
    const char *ToString();
private:
    void RunBatch(std::vector<pending_op *> &ops);
    void RunAddFilter(std::list<parse_pair *> &list);
    void Clear();
    void ParseEntityLine(const char *s, size_t n, bool &in_block, std::list<ent_prop *> *&cl);
private:
    ent_prop *AllocProp();
    void FreeProp(ent_prop *prop);
    int KeyId(const char *key, size_t len);
private:
    std::stack<ent_prop *> m_PropCache;
    void AppendToString(const char* buf, size_t len);
private:
    char* m_tostring;
    size_t m_tostring_len;
    size_t m_tostring_maxlen;
    std::list<std::list<ent_prop *> *> m_props;
    bool m_resync;
    /* case-insensitive key interner (open addressing, ids index m_key_count) */
    std::vector<int> m_key_table;
    std::vector<std::string> m_key_names;
    std::string m_key_tmp;
};

#endif /* _INCLUDE_STRIP_PARSER_H */
