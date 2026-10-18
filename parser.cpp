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

#include <stack>
#include <vector>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include <string.h>
#if !defined(_WIN32)
#include <strings.h>
#endif
#include "parser.h"
#include <string>
#include "support.h"


/* True if PCRE2 JIT can be used in this process. Checked once. */
static bool JitAvailable()
{
    static int state = -1;
    if (state == -1)
    {
        state = (pcre2_jit_compile(NULL, PCRE2_JIT_TEST_ALLOC) == 0) ? 1 : 0;
    }
    return state == 1;
}

/* Tokenizes the first  "key" "value"  pair out of a line. Equivalent to the
 * former regex  "([^"]+)"\s+"([^"]+)"  (unanchored), without the regex engine.
 */
static bool ParseKeyValue(const char *s, size_t len,
                          size_t *key_pos, size_t *key_len,
                          size_t *val_pos, size_t *val_len)
{
    size_t i = 0;
    while (i < len)
    {
        const void *q = memchr(s + i, '"', len - i);
        if (!q)
        {
            return false;
        }
        i = (const char *)q - s;

        size_t kb = i + 1;
        const void *ke = (kb < len) ? memchr(s + kb, '"', len - kb) : NULL;
        if (!ke)
        {
            return false;   /* no closing quote anywhere after this point */
        }
        size_t k_end = (const char *)ke - s;
        if (k_end > kb)
        {
            size_t p = k_end + 1;
            size_t ws = p;
            while (p < len && isspace((unsigned char)s[p]))
            {
                p++;
            }
            if (p > ws && p < len && s[p] == '"')
            {
                size_t vb = p + 1;
                const void *ve = (vb < len) ? memchr(s + vb, '"', len - vb) : NULL;
                if (ve)
                {
                    size_t v_end = (const char *)ve - s;
                    if (v_end > vb)
                    {
                        *key_pos = kb;
                        *key_len = k_end - kb;
                        *val_pos = vb;
                        *val_len = v_end - vb;
                        return true;
                    }
                }
            }
        }
        i++;
    }
    return false;
}

static inline char LowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* needle must already be lowercase */
static bool ContainsNoCase(const char *h, size_t hl, const char *n, size_t nl)
{
    if (nl == 0)
    {
        return true;
    }
    if (hl < nl)
    {
        return false;
    }
    const char first = n[0];
    for (size_t i = 0; i + nl <= hl; i++)
    {
        if (LowerAscii(h[i]) != first)
        {
            continue;
        }
        size_t j = 1;
        while (j < nl && LowerAscii(h[i + j]) == n[j])
        {
            j++;
        }
        if (j == nl)
        {
            return true;
        }
    }
    return false;
}

static bool EqualsNoCase(const char *a, const char *lower_b, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (LowerAscii(a[i]) != lower_b[i])
        {
            return false;
        }
    }
    return true;
}

/* Recognizes patterns that are just a literal with optional ^ / $ anchors and
 * leading/trailing ".*", e.g.  prop_physics  ^func_  \.mdl$  ^info_.*
 * On success fills *needle (lowercased) and *kind. Anything with other regex
 * syntax returns false and is handed to PCRE2 as before.
 */
static bool ClassifySimplePattern(const std::string &pat, std::string *needle, pair_kind *kind)
{
    size_t i = 0;
    size_t end = pat.size();
    bool left = false, right = false;

    if (i < end && pat[i] == '^')
    {
        left = true;
        i++;
    }
    if (end - i >= 2 && pat[i] == '.' && pat[i + 1] == '*')
    {
        left = false;
        i += 2;
    }
    if (end > i && pat[end - 1] == '$')
    {
        if (end - 1 > i && pat[end - 2] == '\\')
        {
            return false;   /* escaped $ - keep it simple, use PCRE2 */
        }
        right = true;
        end--;
    }
    if (end - i >= 2 && pat[end - 2] == '.' && pat[end - 1] == '*')
    {
        if (end - i >= 3 && pat[end - 3] == '\\')
        {
            return false;   /* "\.*" is not ".*" */
        }
        right = false;
        end -= 2;
    }

    std::string lit;
    for (size_t k = i; k < end; k++)
    {
        char c = pat[k];
        if (c == '\\')
        {
            if (k + 1 >= end)
            {
                return false;
            }
            char n = pat[++k];
            if (strchr(".*+?()[]{}|^$\\/-", n) == NULL)
            {
                return false;   /* \d, \s, \w, ... */
            }
            lit.push_back(LowerAscii(n));
        }
        else if (strchr(".[](){}*+?|^$", c) != NULL)
        {
            return false;
        }
        else
        {
            lit.push_back(LowerAscii(c));
        }
    }

    *needle = lit;
    if (left && right)
        *kind = PK_EXACT;
    else if (left)
        *kind = PK_PREFIX;
    else if (right)
        *kind = PK_SUFFIX;
    else
        *kind = PK_CONTAINS;
    return true;
}

static inline bool PairValueMatches(parse_pair *p, const std::string &val)
{
    switch (p->kind)
    {
    case PK_EXACT:
        return val.size() == p->val.size() && EqualsNoCase(val.data(), p->val.data(), val.size());
    case PK_PREFIX:
        return val.size() >= p->val.size() && EqualsNoCase(val.data(), p->val.data(), p->val.size());
    case PK_SUFFIX:
        return val.size() >= p->val.size() &&
               EqualsNoCase(val.data() + (val.size() - p->val.size()), p->val.data(), p->val.size());
    case PK_CONTAINS:
        return ContainsNoCase(val.data(), val.size(), p->val.data(), p->val.size());
    default:
        break;
    }
    if (p->re)
    {
        int rc;
        if (p->jit)
        {
            rc = pcre2_jit_match(p->re, (PCRE2_SPTR)val.c_str(), (PCRE2_SIZE)val.size(),
                                 0, 0, p->match_data, NULL);
        }
        else
        {
            rc = pcre2_match(p->re, (PCRE2_SPTR)val.c_str(), (PCRE2_SIZE)val.size(),
                             0, 0, p->match_data, NULL);
        }
        return rc >= 0;
    }
    return val.size() == p->val.size() && stricmp(val.c_str(), p->val.c_str()) == 0;
}

static inline bool EntPropsMatch(parse_pair *p, ent_prop *e)
{
    return p->kid == e->kid && PairValueMatches(p, e->val);
}

/* Orders pairs so cheap literal comparisons run before regexes. */
static void BuildMatchVector(std::list<parse_pair *> &src, std::vector<parse_pair *> &out)
{
    out.reserve(src.size());
    std::list<parse_pair *>::iterator iter;
    for (iter = src.begin(); iter != src.end(); iter++)
    {
        if (!(*iter)->re)
        {
            out.push_back(*iter);
        }
    }
    for (iter = src.begin(); iter != src.end(); iter++)
    {
        if ((*iter)->re)
        {
            out.push_back(*iter);
        }
    }
}

/* An entity matches when every pair in the filter is satisfied by some property. */
static bool EntityMatchesAll(std::list<ent_prop *> *proplist, const std::vector<parse_pair *> &pairs)
{
    for (size_t i = 0; i < pairs.size(); i++)
    {
        parse_pair *p = pairs[i];
        bool found = false;
        for (std::list<ent_prop *>::iterator it = proplist->begin(); it != proplist->end(); it++)
        {
            if (EntPropsMatch(p, *it))
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
    }
    return true;
}

void f_strncpy_s(char *dest, const char *str, size_t n)
{
    while (
        n--
        &&
        (*dest++=*str++)
        );
    *dest = '\0';
}

/* UTIL_TrimLeft
 * Removes whitespace characters from left side of string
 */
void UTIL_TrimLeft(char *buffer)
{
    char *i = buffer;
    if (i && *i)
    {
        while(isspace((unsigned char)*i))
        {
            i++;
        }
        if (i != buffer)
        {
            memmove(buffer, i, (strlen(i) + 1) * sizeof(char));
        }
    }
}

/* UTIL_TrimRight
 * Removes whitespace characters from right side of string
 */
void UTIL_TrimRight(char *buffer)
{
    if (buffer)
    {
        for (int i = (int)strlen(buffer) - 1; i >= 0; i--)
        {
            if (isspace((unsigned char)buffer[i]))
            {
                buffer[i] = '\0';
            } else {
                break;
            }
        }
    }
}

Stripper::Stripper()
{
    m_resync = false;
    m_tostring = NULL;
    m_tostring_len = 0;
    m_tostring_maxlen = 0;
    m_key_table.assign(256, -1);
}

Stripper::~Stripper()
{
    Clear();

    while (!m_PropCache.empty())
    {
        delete m_PropCache.top();
        m_PropCache.pop();
    }

    free(m_tostring);
}

int Stripper::KeyId(const char *key, size_t len)
{
    m_key_tmp.resize(len);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++)
    {
        char c = key[i];
        if (c >= 'A' && c <= 'Z')
        {
            c = c - 'A' + 'a';
        }
        m_key_tmp[i] = c;
        hash = (hash ^ (unsigned char)c) * 16777619u;
    }

    size_t mask = m_key_table.size() - 1;
    size_t slot = hash & mask;
    while (m_key_table[slot] != -1)
    {
        const std::string &name = m_key_names[m_key_table[slot]];
        if (name.size() == len && memcmp(name.data(), m_key_tmp.data(), len) == 0)
        {
            return m_key_table[slot];
        }
        slot = (slot + 1) & mask;
    }

    int id = (int)m_key_names.size();
    m_key_names.push_back(m_key_tmp);
    m_key_table[slot] = id;

    if (m_key_names.size() * 2 > m_key_table.size())
    {
        /* grow and rehash */
        std::vector<int> bigger(m_key_table.size() * 2, -1);
        size_t new_mask = bigger.size() - 1;
        for (size_t n = 0; n < m_key_names.size(); n++)
        {
            const std::string &name = m_key_names[n];
            uint32_t h = 2166136261u;
            for (size_t i = 0; i < name.size(); i++)
            {
                h = (h ^ (unsigned char)name[i]) * 16777619u;
            }
            size_t s = h & new_mask;
            while (bigger[s] != -1)
            {
                s = (s + 1) & new_mask;
            }
            bigger[s] = (int)n;
        }
        m_key_table.swap(bigger);
    }
    return id;
}

ent_prop *Stripper::AllocProp()
{
    if (m_PropCache.empty())
    {
        return new ent_prop;
    } else {
        ent_prop *p = m_PropCache.top();
#if defined _DEBUG
        assert(p->marked == false);
        p->marked = true;
#endif
        m_PropCache.pop();
        return p;
    }
}

void Stripper::FreeProp(ent_prop *prop)
{
#if defined _DEBUG
    assert(prop->marked == true);
    prop->marked = false;
#endif
    m_PropCache.push(prop);
}

void Stripper::ParseEntityLine(const char *s, size_t n, bool &in_block, std::list<ent_prop *> *&cl)
{
    if (n > 0 && s[0] == '{' && !in_block)
    {
        /* if we reached a '{' and we're not in a block, begin a new block */
        in_block = true;
        if (!cl)
        {
            cl = new std::list<ent_prop *>();
        }
    } else if (n > 0 && s[0] == '}' && in_block) {
        /* end the old block and push it onto the block list */
        in_block = false;
        m_props.push_back(cl);
        cl = NULL;
        if (n > 1 && s[1] == '{')
        {
            in_block = true;
            cl = new std::list<ent_prop *>();
        }
    } else {
        size_t kp, kl, vp, vl;
        if (ParseKeyValue(s, n, &kp, &kl, &vp, &vl) && cl)
        {
            ent_prop *e = AllocProp();
#if defined _DEBUG
            e->last_alloc = __LINE__;
#endif
            e->key.assign(s + kp, kl);
            e->val.assign(s + vp, vl);
            e->kid = KeyId(s + kp, kl);
            cl->push_back(e);
        }
    }
}

void Stripper::SetEntityList(const char *ents)
{
    Clear();

    m_resync = false;
    size_t len = strlen(ents);
    AppendToString(ents, len);

    /* Single pass: split into lines (newlines inside quotes don't count) and
     * parse each line in place, with no intermediate copies.
     */
    bool in_block = false;
    std::list<ent_prop *> *cl = NULL;
    size_t pos = 0;
    char in_string = 0;
    for (size_t i = 0; i < len; i++)
    {
        char c = ents[i];
        if (c == '"')
        {
            in_string ^= 1;
        }
        else if ((c == '\n' || c == '\r') && !in_string)
        {
            ParseEntityLine(ents + pos, i - pos, in_block, cl);
            pos = i + 1;
        }
    }
    if (pos < len)
    {
        ParseEntityLine(ents + pos, len - pos, in_block, cl);
    }

    delete cl;
}

void Stripper::RunBatch(std::vector<pending_op *> &ops)
{
    if (ops.empty())
    {
        return;
    }

    std::list<std::list<ent_prop *> *>::iterator proplist_iter = m_props.begin();
    while (proplist_iter != m_props.end())
    {
        std::list<ent_prop *> *proplist = *proplist_iter;
        bool removed = false;

        for (size_t n = 0; n < ops.size() && !removed; n++)
        {
            pending_op *op = ops[n];
            if (!EntityMatchesAll(proplist, op->match))
            {
                continue;
            }

            if (!op->modify)
            {
                /* remove this entity */
                for (std::list<ent_prop *>::iterator it = proplist->begin(); it != proplist->end(); ++it)
                {
                    FreeProp(*it);
#if defined _DEBUG
                    (*it)->last_free = __LINE__;
#endif
                }
                delete proplist;
                proplist_iter = m_props.erase(proplist_iter);
                removed = true;
                break;
            }

            std::list<ent_prop *>::iterator prop_iter;
            std::list<parse_pair *>::iterator parse_iter;
            ent_prop *e;
            parse_pair *p;

            /* first do deletions */
            if (!op->to_remove.empty())
            {
                prop_iter = proplist->begin();
                while (prop_iter != proplist->end())
                {
                    e = *prop_iter;
                    bool matched = false;
                    for (parse_iter = op->to_remove.begin(); parse_iter != op->to_remove.end(); parse_iter++)
                    {
                        if (EntPropsMatch(*parse_iter, e))
                        {
                            matched = true;
                            break;
                        }
                    }
                    if (matched)
                    {
                        FreeProp(e);
                        prop_iter = proplist->erase(prop_iter);
                    } else {
                        prop_iter++;
                    }
                }
            }
            /* do replacements */
            if (!op->to_replace.empty())
            {
                for (prop_iter = proplist->begin(); prop_iter != proplist->end(); prop_iter++)
                {
                    e = *prop_iter;
                    for (parse_iter = op->to_replace.begin(); parse_iter != op->to_replace.end(); parse_iter++)
                    {
                        p = *parse_iter;
                        if (p->kid == e->kid)
                        {
                            e->val = p->val;
                            break;
                        }
                    }
                }
            }
            /* do insertions */
            for (parse_iter = op->to_insert.begin(); parse_iter != op->to_insert.end(); parse_iter++)
            {
                p = *parse_iter;
                e = AllocProp();
#if defined _DEBUG
                e->last_alloc = __LINE__;
#endif
                e->key = p->key;
                e->val = p->val;
                e->kid = p->kid;
                proplist->push_back(e);
            }
        }

        if (!removed)
        {
            ++proplist_iter;
        }
    }
}

void Stripper::RunAddFilter(std::list<parse_pair *> &list)
{
    std::list<parse_pair *>::iterator iter, end=list.end();
    iter=list.begin();

    std::list<ent_prop *> *cl = new std::list<ent_prop *>();
    ent_prop *e;
    while (iter != end)
    {
        e = AllocProp();
#if defined _DEBUG
        e->last_alloc = __LINE__;
#endif
        e->key = (*iter)->key;
        e->val = (*iter)->val;
        e->kid = (*iter)->kid;
        cl->push_back(e);
        iter++;
    }
    m_props.push_back(cl);
}

bool Stripper::ApplyFileFilter(const char *file)
{
    FILE *fp = fopen(file, "rt");
    if (!fp)
    {
        return false;
    }

    char buffer[255];

    enum Mode
    {
        Mode_Filter,
        Mode_Add,
        Mode_Replace,
    };

    enum SubMode
    {
        SubMode_None,
        SubMode_Match,
        SubMode_Replace,
        SubMode_Remove,
        SubMode_Insert
    };

    bool in_block = false;
    std::list<parse_pair *> props;
    std::stack<parse_pair *> fpairs;
    Mode mode = Mode_Filter;
    SubMode submode = SubMode_None;
    int line = 0;
    std::list<parse_pair *>::iterator iter, end;
    replace_prop replace;
    std::vector<pending_op *> pending;
    const bool use_jit = !stripper_game.should_disable_jit() && JitAvailable();

    /* Runs queued remove:/modify: blocks in one pass and recycles their pairs. */
    #define FLUSH_PENDING() \
        do { \
            RunBatch(pending); \
            for (size_t n_ = 0; n_ < pending.size(); n_++) \
            { \
                for (iter = pending[n_]->owned.begin(); iter != pending[n_]->owned.end(); iter++) \
                    fpairs.push(*iter); \
                delete pending[n_]; \
            } \
            pending.clear(); \
        } while (0)

    /* Moves the pairs in props into dst, recycling anything dst already held. */
    #define TAKE_PROPS(dst) \
        do { \
            for (iter = (dst).begin(); iter != (dst).end(); iter++) \
                fpairs.push(*iter); \
            (dst).clear(); \
            (dst).swap(props); \
        } while (0)

    while (fgets(buffer, sizeof(buffer)-1, fp))
    {
        line++;
        //strip whitespace
        UTIL_TrimRight(buffer);
        UTIL_TrimLeft(buffer);
        //strip comments
        if (buffer[0] == ';' ||
            (buffer[0] == '/' && buffer[1] == '/') ||
            (buffer[0] == '#') ||
            (buffer[0] == '\0'))
            continue;
        if (strncmp(buffer, "filter:", 7) == 0)
        {
            mode = Mode_Filter;
        } else if (strncmp(buffer, "remove:", 7) == 0) {
            mode = Mode_Filter;
        } else if (strncmp(buffer, "add:", 4) == 0) {
            mode = Mode_Add;
        } else if (strncmp(buffer, "modify:", 8) == 0) {
            mode = Mode_Replace;
            submode = SubMode_None;
        } else if (!strncmp(buffer, "match:", 6) && (mode == Mode_Replace)) {
            submode = SubMode_Match;
        } else if (!strncmp(buffer, "replace:", 8) && (mode == Mode_Replace)) {
            submode = SubMode_Replace;
        } else if (!strncmp(buffer, "delete:", 7) && (mode == Mode_Replace)) {
            submode = SubMode_Remove;
        } else if (!strncmp(buffer, "insert:", 7) && (mode == Mode_Replace)) {
            submode = SubMode_Insert;
        } else if (strcmp(buffer, "{")==0 && !in_block) {
            /* if we reach a new block and we're not in a block,
             * dump the current props table and reset.
             */
            if (mode != Mode_Replace || (mode == Mode_Replace && submode != SubMode_None))
            {
                in_block = true;
                props.clear();
            }
        } else if (strncmp(buffer, "}", 1) == 0 && in_block) {
            /* if we reach the end of a block and we're in a block,
             * mark the end and flush what we've done
             */
            in_block = false;

            if (mode == Mode_Replace)
            {
                assert(submode != SubMode_None);
                switch (submode)
                {
                case SubMode_None:
                    break;
                case SubMode_Insert:
                    TAKE_PROPS(replace.to_insert);
                    break;
                case SubMode_Remove:
                    TAKE_PROPS(replace.to_remove);
                    break;
                case SubMode_Match:
                    TAKE_PROPS(replace.match);
                    break;
                case SubMode_Replace:
                    TAKE_PROPS(replace.to_replace);
                    break;
                }
            } else {
                if (mode == Mode_Filter)
                {
                    /* queue it; it runs with its neighbours in one pass */
                    pending_op *op = new pending_op;
                    BuildMatchVector(props, op->match);
                    op->owned.splice(op->owned.end(), props);
                    pending.push_back(op);
                } else {
                    if (mode == Mode_Add)
                    {
                        /* adds append entities that later filters must see,
                         * so everything queued before it has to run first */
                        FLUSH_PENDING();
                        RunAddFilter(props);
                    }
                    end = props.end();
                    for (iter=props.begin(); iter!=end; iter++)
                    {
                        fpairs.push((*iter));
                    }
                    props.clear();
                }
            }
        } else if (strncmp(buffer, "}", 1) == 0 && !in_block && mode == Mode_Replace) {
            /* queue the modify: block */
            pending_op *op = new pending_op;
            op->modify = true;
            BuildMatchVector(replace.match, op->match);
            op->owned.splice(op->owned.end(), replace.match);
            op->to_remove.swap(replace.to_remove);
            op->to_replace.swap(replace.to_replace);
            op->to_insert.swap(replace.to_insert);
            op->owned.insert(op->owned.end(), op->to_remove.begin(), op->to_remove.end());
            op->owned.insert(op->owned.end(), op->to_replace.begin(), op->to_replace.end());
            op->owned.insert(op->owned.end(), op->to_insert.begin(), op->to_insert.end());
            pending.push_back(op);
        } else if (in_block) {
            size_t kp, kl, vp, vl;
            if (ParseKeyValue(buffer, strlen(buffer), &kp, &kl, &vp, &vl))
            {
                /* Only match-style blocks use patterns. Add/insert/replace
                 * values are literal, so don't build regexes for them.
                 */
                bool wants_regex = (mode == Mode_Filter) ||
                                   (mode == Mode_Replace &&
                                    (submode == SubMode_Match || submode == SubMode_Remove));
                pcre2_code *re = NULL;
                pcre2_match_data *match_data = NULL;
                bool jit = false;
                pair_kind kind = PK_LITERAL;
                std::string val(buffer + vp, vl);
                if (wants_regex && vl >= 1 && val[0] == '/' && val[vl - 1] == '/')
                {
                    val.erase(vl - 1);
                    std::string needle;
                    pair_kind simple_kind;
                    if (ClassifySimplePattern(val.substr(1), &needle, &simple_kind))
                    {
                        /* no regex engine needed for this one */
                        val.swap(needle);
                        kind = simple_kind;
                    }
                    else
                    {
                    kind = PK_REGEX;
                    int error_code;
                    PCRE2_SIZE error_offset;
                    re = pcre2_compile((PCRE2_SPTR)val.c_str() + 1,
                                       PCRE2_ZERO_TERMINATED,
                                       PCRE2_CASELESS,
                                       &error_code,
                                       &error_offset,
                                       NULL);
                    if (!re)
                    {
                        char error[256];
                        int error_length = pcre2_get_error_message(error_code,
                                                                   (PCRE2_UCHAR *)error,
                                                                   sizeof(error));
                        if (error_length < 0)
                        {
                            strcpy(error, "unknown PCRE2 error");
                        }
                        stripper_game.log_message("File %s parse error (line %d):", file, line);
                        stripper_game.log_message("Expression(%s): At pos %d, %s", val.c_str(), (int)error_offset, error);
                        continue;
                    }
                    if (use_jit)
                    {
                        int jit_error = pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
                        if (jit_error != 0)
                        {
                            char jit_error_msg[256];
                            if (pcre2_get_error_message(jit_error, (PCRE2_UCHAR8 *)jit_error_msg, sizeof(jit_error_msg)) < 0)
                            {
                                snprintf(jit_error_msg, sizeof(jit_error_msg), "Unknown PCRE JIT error");
                            }
                            stripper_game.log_message("Cannot request JIT compilation from PCRE, error: %s", jit_error_msg);
                        } else {
                            jit = true;
                        }
                    }
                    match_data = pcre2_match_data_create_from_pattern(re, NULL);
                    if (!match_data)
                    {
                        pcre2_code_free(re);
                        stripper_game.log_message("File %s parse error (line %d):", file, line);
                        stripper_game.log_message("Expression(%s): Unable to allocate PCRE2 match data", val.c_str());
                        continue;
                    }
                    }
                }
                /* get a parse pair from the cache */
                parse_pair *p;
                if (fpairs.empty())
                {
                    p = new parse_pair;
                } else {
                    p = fpairs.top();
                    if (p->match_data)
                    {
                        pcre2_match_data_free(p->match_data);
                    }
                    if (p->re)
                    {
                        pcre2_code_free(p->re);
                    }
                    fpairs.pop();
                }
                /* assign property information */
                p->key.assign(buffer + kp, kl);
                p->val.swap(val);
                p->kid = KeyId(buffer + kp, kl);
                p->kind = kind;
                p->re = re;
                p->match_data = match_data;
                p->jit = jit;
                props.push_back(p);
            }
        }
    }
    #undef TAKE_PROPS

    FLUSH_PENDING();
    #undef FLUSH_PENDING

    /* flush anything remaining */
    for (iter=props.begin(); iter!=props.end(); iter++)
    {
        fpairs.push( (*iter) );
    }
    /* pairs still held by an unfinished modify: block */
    #define FLUSH_LIST(l) for (iter=(l).begin(); iter!=(l).end(); iter++) fpairs.push(*iter)
    FLUSH_LIST(replace.match);
    FLUSH_LIST(replace.to_insert);
    FLUSH_LIST(replace.to_remove);
    FLUSH_LIST(replace.to_replace);
    #undef FLUSH_LIST

    /* delete cached pairs */
    parse_pair *k = NULL;
    while (!fpairs.empty())
    {
        k = fpairs.top();
        if (k->match_data)
        {
            pcre2_match_data_free(k->match_data);
        }
        if (k->re)
        {
            pcre2_code_free(k->re);
        }
        delete k;
        fpairs.pop();
    }

    fclose(fp);

    m_resync = true;
    return true;
}

const char *Stripper::ToString()
{
    if (!m_resync)
    {
        return m_tostring;
    }

    if (m_tostring != NULL)
        m_tostring[0] = '\0';
    m_tostring_len = 0;

    std::list<std::list<ent_prop *> *>::iterator iter, end=m_props.end(), begin=m_props.begin();
    std::list<ent_prop *>::iterator eiter, eend;
    bool first = true;

    for (iter=m_props.begin(); iter!=end; iter++)
    {
        eend = (*iter)->end();
        if (!first)
        {
            AppendToString("\n", 1);
        } else {
            first = true;
        }
        AppendToString("{\n", 2);
        for (eiter=(*iter)->begin(); eiter!=eend; eiter++)
        {
            AppendToString("\"", 1);
            AppendToString((*eiter)->key.c_str(), (*eiter)->key.size());
            AppendToString("\" \"", 3);
            AppendToString((*eiter)->val.c_str(), (*eiter)->val.size());
            AppendToString("\"\n", 2);
        }
        AppendToString("}", 1);
    }

    m_resync = false;

    return m_tostring;
}

void Stripper::Clear()
{
    std::list<ent_prop *>::iterator iter2, end2;
    std::list<std::list<ent_prop *> *>::iterator iter3, end3;

    iter3 = m_props.begin();
    end3 = m_props.end();
    while (iter3 != end3)
    {
        iter2 = (*iter3)->begin();
        end2 = (*iter3)->end();
        while (iter2 != end2)
        {
            FreeProp((*iter2));
#if defined _DEBUG
            (*iter2)->last_free = __LINE__;
#endif
            iter2++;
        }
        delete (*iter3);
        iter3++;
    }

    m_props.clear();
    if (m_tostring != NULL)
        m_tostring[0] = '\0';
    m_tostring_len = 0;
}

void Stripper::AppendToString(const char* buf, size_t len)
{
    if (m_tostring_len + len + 1 > m_tostring_maxlen)
    {
        if (m_tostring_maxlen == 0)
            m_tostring_maxlen = 512;
        while (m_tostring_maxlen < m_tostring_len + len + 1)
            m_tostring_maxlen += m_tostring_maxlen / 2;
        m_tostring = (char*)realloc(m_tostring, m_tostring_maxlen);
    }
    memcpy(m_tostring + m_tostring_len, buf, len);
    m_tostring_len += len;
    m_tostring[m_tostring_len] = '\0';
}
