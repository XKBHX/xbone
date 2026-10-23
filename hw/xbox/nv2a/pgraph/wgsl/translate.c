/*
 * Geforce NV2A PGRAPH WGSL Shader Generator
 *
 * Copyright (c) 2026 xemu Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "translate.h"

/*
 * Translates the WebGPU-targeted GLSL emitted by glsl/ into WGSL.
 * The GLSL generator stays the description of NV2A shading; this pass
 * covers the subset that generator actually emits.
 */

#define MAX_MACRO 160
#define MAX_PARAMS 8
#define MAX_FN 96
#define MAX_SYM 192
#define MAX_IO 40
#define MAX_ARGS 16
#define MAX_COND 32
#define MAX_DISABLED 32

enum {
    TY_NONE = 0,
    TY_ABSTRACT,
    TY_BOOL,
    TY_I32,
    TY_U32,
    TY_F32,
    TY_BVEC2,
    TY_BVEC3,
    TY_BVEC4,
    TY_IVEC2,
    TY_IVEC3,
    TY_IVEC4,
    TY_UVEC2,
    TY_UVEC3,
    TY_UVEC4,
    TY_VEC2,
    TY_VEC3,
    TY_VEC4,
    TY_MAT2,
    TY_MAT3,
    TY_MAT4,
    TY_SAMPLER,
};

enum {
    SK_2D = 0,
    SK_3D,
    SK_CUBE,
    SK_2D_UINT,
};

enum {
    INTERP_SMOOTH = 0,
    INTERP_LINEAR,
    INTERP_FLAT,
};

typedef struct Expr {
    char *text;
    int ty;
    bool array;
    bool is_sampler;
    int skind;
    char *tex;
    char *smp;
    /* Set when text is a swizzle of base. Chrome rejects stores through a
     * swizzle, so an assignment rebuilds the whole vector. */
    bool swizzle;
    int base_ty;
    char sw[5];
    char *base;
} Expr;

typedef struct Macro {
    char *name;
    bool func;
    int nparams;
    char *params[MAX_PARAMS];
    char *body;
} Macro;

typedef struct Cond {
    bool parent_active;
    bool active;
    bool seen_else;
} Cond;

typedef struct Pre {
    Macro macros[MAX_MACRO];
    int nmacros;
    Cond conds[MAX_COND];
    int ncond;
    const char *disabled[MAX_DISABLED];
    int ndisabled;
    bool failed;
} Pre;

typedef struct Sym {
    char *name;
    int ty;
    bool array;
    int scope;
} Sym;

typedef struct Fn {
    char *name;
    char *wgsl_name;
    int ret;
    int nparams;
    int params[MAX_PARAMS];
} Fn;

typedef struct IO {
    char *name;
    int ty;
    int location;
    int interp;
    bool input;
} IO;

typedef struct Tr {
    const char *s;
    size_t i;
    bool failed;
    bool vertex;
    GString *decls;
    GString *preamble;
    GString *main_body;
    GString *cur;
    int depth;
    bool uses_vertex_index;
    bool uses_frag_coord;
    bool uses_frag_depth;
    bool saw_main;
    Sym globals[MAX_SYM];
    int nglobals;
    Sym locals[MAX_SYM];
    int nlocals;
    Sym uniforms[MAX_SYM];
    int nuniforms;
    Fn fns[MAX_FN];
    int nfns;
    IO ios[MAX_IO];
    int nios;
} Tr;

static bool is_id_start(char c)
{
    return g_ascii_isalpha(c) || c == '_';
}

static bool is_id(char c)
{
    return g_ascii_isalnum(c) || c == '_';
}

static char *trim_dup(const char *s, size_t n)
{
    while (n && g_ascii_isspace(s[0])) {
        s++;
        n--;
    }
    while (n && g_ascii_isspace(s[n - 1])) {
        n--;
    }
    return g_strndup(s, n);
}

static void e_clear(Expr *e)
{
    g_free(e->text);
    g_free(e->tex);
    g_free(e->smp);
    g_free(e->base);
    memset(e, 0, sizeof(*e));
}

static Expr e_take(int ty, char *text)
{
    Expr e = { .text = text ? text : g_strdup(""), .ty = ty };
    return e;
}

static Expr e_none(void)
{
    return e_take(TY_NONE, g_strdup(""));
}

static void free_args(Expr *args, int n)
{
    for (int i = 0; i < n; i++) {
        e_clear(&args[i]);
    }
}

static int scalar_of(int ty)
{
    switch (ty) {
    case TY_BOOL:
    case TY_BVEC2:
    case TY_BVEC3:
    case TY_BVEC4:
        return TY_BOOL;
    case TY_I32:
    case TY_IVEC2:
    case TY_IVEC3:
    case TY_IVEC4:
        return TY_I32;
    case TY_U32:
    case TY_UVEC2:
    case TY_UVEC3:
    case TY_UVEC4:
        return TY_U32;
    case TY_F32:
    case TY_VEC2:
    case TY_VEC3:
    case TY_VEC4:
    case TY_MAT2:
    case TY_MAT3:
    case TY_MAT4:
        return TY_F32;
    case TY_ABSTRACT:
        return TY_ABSTRACT;
    default:
        return TY_NONE;
    }
}

static int width_of(int ty)
{
    switch (ty) {
    case TY_BVEC2:
    case TY_IVEC2:
    case TY_UVEC2:
    case TY_VEC2:
    case TY_MAT2:
        return 2;
    case TY_BVEC3:
    case TY_IVEC3:
    case TY_UVEC3:
    case TY_VEC3:
    case TY_MAT3:
        return 3;
    case TY_BVEC4:
    case TY_IVEC4:
    case TY_UVEC4:
    case TY_VEC4:
    case TY_MAT4:
        return 4;
    default:
        return 1;
    }
}

static bool is_mat(int ty)
{
    return ty == TY_MAT2 || ty == TY_MAT3 || ty == TY_MAT4;
}

static int vec_of(int scalar, int n)
{
    if (n <= 1) {
        return scalar;
    }
    switch (scalar) {
    case TY_BOOL:
        return n == 2 ? TY_BVEC2 : n == 3 ? TY_BVEC3 : TY_BVEC4;
    case TY_I32:
        return n == 2 ? TY_IVEC2 : n == 3 ? TY_IVEC3 : TY_IVEC4;
    case TY_U32:
        return n == 2 ? TY_UVEC2 : n == 3 ? TY_UVEC3 : TY_UVEC4;
    case TY_F32:
        return n == 2 ? TY_VEC2 : n == 3 ? TY_VEC3 : TY_VEC4;
    default:
        return TY_NONE;
    }
}

static int mat_col(int ty)
{
    switch (ty) {
    case TY_MAT2:
        return TY_VEC2;
    case TY_MAT3:
        return TY_VEC3;
    case TY_MAT4:
        return TY_VEC4;
    default:
        return TY_NONE;
    }
}

static const char *ty_tag(int ty)
{
    switch (ty) {
    case TY_BOOL:
        return "bool";
    case TY_I32:
        return "i32";
    case TY_U32:
        return "u32";
    case TY_F32:
        return "f32";
    case TY_BVEC2:
        return "vec2b";
    case TY_BVEC3:
        return "vec3b";
    case TY_BVEC4:
        return "vec4b";
    case TY_IVEC2:
        return "vec2i";
    case TY_IVEC3:
        return "vec3i";
    case TY_IVEC4:
        return "vec4i";
    case TY_UVEC2:
        return "vec2u";
    case TY_UVEC3:
        return "vec3u";
    case TY_UVEC4:
        return "vec4u";
    case TY_VEC2:
        return "vec2f";
    case TY_VEC3:
        return "vec3f";
    case TY_VEC4:
        return "vec4f";
    case TY_MAT2:
        return "mat2";
    case TY_MAT3:
        return "mat3";
    case TY_MAT4:
        return "mat4";
    default:
        return "x";
    }
}

static const char *ty_wgsl(int ty)
{
    switch (ty) {
    case TY_BOOL:
        return "bool";
    case TY_I32:
        return "i32";
    case TY_U32:
        return "u32";
    case TY_F32:
    case TY_ABSTRACT:
        return "f32";
    case TY_BVEC2:
        return "vec2<bool>";
    case TY_BVEC3:
        return "vec3<bool>";
    case TY_BVEC4:
        return "vec4<bool>";
    case TY_IVEC2:
        return "vec2i";
    case TY_IVEC3:
        return "vec3i";
    case TY_IVEC4:
        return "vec4i";
    case TY_UVEC2:
        return "vec2u";
    case TY_UVEC3:
        return "vec3u";
    case TY_UVEC4:
        return "vec4u";
    case TY_VEC2:
        return "vec2f";
    case TY_VEC3:
        return "vec3f";
    case TY_VEC4:
        return "vec4f";
    case TY_MAT2:
        return "mat2x2f";
    case TY_MAT3:
        return "mat3x3f";
    case TY_MAT4:
        return "mat4x4f";
    default:
        return "f32";
    }
}

static int type_from_name(const char *name)
{
    const struct {
        const char *name;
        int ty;
    } tab[] = {
        { "void", TY_NONE },   { "bool", TY_BOOL },   { "int", TY_I32 },
        { "uint", TY_U32 },    { "float", TY_F32 },   { "bvec2", TY_BVEC2 },
        { "bvec3", TY_BVEC3 }, { "bvec4", TY_BVEC4 }, { "ivec2", TY_IVEC2 },
        { "ivec3", TY_IVEC3 }, { "ivec4", TY_IVEC4 }, { "uvec2", TY_UVEC2 },
        { "uvec3", TY_UVEC3 }, { "uvec4", TY_UVEC4 }, { "vec2", TY_VEC2 },
        { "vec3", TY_VEC3 },   { "vec4", TY_VEC4 },   { "mat2", TY_MAT2 },
        { "mat3", TY_MAT3 },   { "mat4", TY_MAT4 },
    };
    for (int i = 0; i < ARRAY_SIZE(tab); i++) {
        if (!strcmp(name, tab[i].name)) {
            return strcmp(name, "void") ? tab[i].ty : TY_NONE;
        }
    }
    return -1;
}

static bool is_type_name(const char *name)
{
    return type_from_name(name) >= 0;
}

static int sampler_kind(const char *name)
{
    if (!strcmp(name, "sampler2D")) {
        return SK_2D;
    }
    if (!strcmp(name, "sampler3D")) {
        return SK_3D;
    }
    if (!strcmp(name, "samplerCube")) {
        return SK_CUBE;
    }
    if (!strcmp(name, "usampler2D")) {
        return SK_2D_UINT;
    }
    return -1;
}

static bool is_concrete_int(int ty)
{
    int s = scalar_of(ty);
    return s == TY_I32 || s == TY_U32;
}

static bool is_floaty(int ty)
{
    return scalar_of(ty) == TY_F32;
}

static Expr cast_to(int ty, Expr e)
{
    if (e.ty == ty || ty == TY_NONE || ty == TY_ABSTRACT || e.ty == TY_NONE) {
        return e;
    }
    char *text = g_strdup_printf("%s(%s)", ty_wgsl(ty), e.text);
    e_clear(&e);
    return e_take(ty, text);
}

static Expr coerce(int ty, Expr e)
{
    if (ty == TY_NONE || e.ty == TY_NONE || e.ty == ty) {
        return e;
    }
    if (e.ty == TY_ABSTRACT && width_of(ty) == 1 && scalar_of(ty) != TY_BOOL &&
        !is_mat(ty)) {
        return e;
    }
    if (scalar_of(e.ty) == scalar_of(ty) && width_of(e.ty) == width_of(ty) &&
        !is_mat(e.ty) && !is_mat(ty)) {
        return e;
    }
    return cast_to(ty, e);
}

/* Preprocessor */

static Macro *find_macro(Pre *p, const char *name)
{
    for (int i = 0; i < p->nmacros; i++) {
        if (!strcmp(p->macros[i].name, name)) {
            return &p->macros[i];
        }
    }
    return NULL;
}

static bool pre_active(Pre *p)
{
    return p->ncond == 0 || p->conds[p->ncond - 1].active;
}

static bool pre_disabled(Pre *p, const char *name)
{
    for (int i = 0; i < p->ndisabled; i++) {
        if (!strcmp(p->disabled[i], name)) {
            return true;
        }
    }
    return false;
}

static void pre_fail(Pre *p, const char *msg)
{
    if (!p->failed) {
        p->failed = true;
        fprintf(stderr, "[WebGPU] WGSL translate: %s\n", msg);
    }
}

static char *expand_all(Pre *p, const char *s, int depth);

static char *subst_params(Macro *m, char **args)
{
    GString *o = g_string_new("");

    for (size_t i = 0; m->body[i];) {
        if (is_id_start(m->body[i])) {
            size_t j = i + 1;
            while (is_id(m->body[j])) {
                j++;
            }
            char *name = g_strndup(m->body + i, j - i);
            int hit = -1;
            for (int a = 0; a < m->nparams; a++) {
                if (!strcmp(m->params[a], name)) {
                    hit = a;
                    break;
                }
            }
            g_string_append(o, hit >= 0 ? args[hit] : name);
            g_free(name);
            i = j;
        } else {
            g_string_append_c(o, m->body[i++]);
        }
    }
    return g_string_free(o, FALSE);
}

static char *expand_all(Pre *p, const char *s, int depth)
{
    GString *o = g_string_new("");

    if (depth > 64) {
        pre_fail(p, "macro expansion nested too deeply");
        return g_string_free(o, FALSE);
    }

    for (size_t i = 0; s[i] && !p->failed;) {
        if (!is_id_start(s[i])) {
            g_string_append_c(o, s[i++]);
            continue;
        }
        size_t j = i + 1;
        while (is_id(s[j])) {
            j++;
        }
        char *name = g_strndup(s + i, j - i);
        i = j;
        Macro *m = find_macro(p, name);
        if (!m || pre_disabled(p, name)) {
            g_string_append(o, name);
            g_free(name);
            continue;
        }
        if (m->func) {
            size_t k = i;
            while (s[k] == ' ' || s[k] == '\t') {
                k++;
            }
            if (s[k] != '(') {
                g_string_append(o, name);
                g_free(name);
                continue;
            }
            k++;
            char *args[MAX_PARAMS];
            int n = 0;
            while (s[k] && s[k] != ')' && n < MAX_PARAMS) {
                size_t start = k;
                int paren = 0, brack = 0, brace = 0;
                while (s[k]) {
                    char c = s[k];
                    if (c == '(') {
                        paren++;
                    } else if (c == ')') {
                        if (!paren && !brack && !brace) {
                            break;
                        }
                        if (paren) {
                            paren--;
                        }
                    } else if (c == '[') {
                        brack++;
                    } else if (c == ']') {
                        if (brack) {
                            brack--;
                        }
                    } else if (c == '{') {
                        brace++;
                    } else if (c == '}') {
                        if (brace) {
                            brace--;
                        }
                    } else if (c == ',' && !paren && !brack && !brace) {
                        break;
                    }
                    k++;
                }
                char *raw = trim_dup(s + start, k - start);
                args[n++] = expand_all(p, raw, depth + 1);
                g_free(raw);
                if (s[k] == ',') {
                    k++;
                }
            }
            if (s[k] == ')') {
                k++;
            } else {
                pre_fail(p, "unclosed macro argument list");
            }
            if (n != m->nparams) {
                pre_fail(p, "macro argument count does not match");
            }
            i = k;
            if (!p->failed) {
                char *sub = subst_params(m, args);
                if (p->ndisabled < MAX_DISABLED) {
                    p->disabled[p->ndisabled++] = m->name;
                }
                char *rec = expand_all(p, sub, depth + 1);
                if (p->ndisabled) {
                    p->ndisabled--;
                }
                g_string_append(o, rec);
                g_free(rec);
                g_free(sub);
            }
            for (int a = 0; a < n; a++) {
                g_free(args[a]);
            }
            g_free(name);
            continue;
        }
        if (p->ndisabled < MAX_DISABLED) {
            p->disabled[p->ndisabled++] = m->name;
        }
        char *rec = expand_all(p, m->body, depth + 1);
        if (p->ndisabled) {
            p->ndisabled--;
        }
        g_string_append(o, rec);
        g_free(rec);
        g_free(name);
    }
    return g_string_free(o, FALSE);
}

static void define_macro(Pre *p, const char *line)
{
    const char *s = line;
    if (!is_id_start(*s)) {
        pre_fail(p, "invalid #define");
        return;
    }
    const char *ns = s;
    while (is_id(*s)) {
        s++;
    }
    char *name = g_strndup(ns, s - ns);
    Macro *m = find_macro(p, name);
    if (!m) {
        if (p->nmacros >= MAX_MACRO) {
            pre_fail(p, "too many macros");
            g_free(name);
            return;
        }
        m = &p->macros[p->nmacros++];
        memset(m, 0, sizeof(*m));
        m->name = name;
    } else {
        g_free(name);
        g_free(m->body);
        m->body = NULL;
        for (int i = 0; i < m->nparams; i++) {
            g_free(m->params[i]);
            m->params[i] = NULL;
        }
        m->nparams = 0;
        m->func = false;
    }
    if (*s == '(') {
        m->func = true;
        s++;
        while (*s && *s != ')') {
            while (g_ascii_isspace(*s)) {
                s++;
            }
            if (*s == ')') {
                break;
            }
            if (!is_id_start(*s) || m->nparams >= MAX_PARAMS) {
                pre_fail(p, "invalid macro parameter");
                break;
            }
            const char *ps = s;
            while (is_id(*s)) {
                s++;
            }
            m->params[m->nparams++] = g_strndup(ps, s - ps);
            while (g_ascii_isspace(*s)) {
                s++;
            }
            if (*s == ',') {
                s++;
            }
        }
        if (*s == ')') {
            s++;
        }
    }
    while (g_ascii_isspace(*s)) {
        s++;
    }
    m->body = g_strdup(s);
}

static bool handle_directive(Pre *p, const char *line)
{
    const char *s = line;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s != '#') {
        return false;
    }
    s++;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char word[32];
    size_t n = 0;
    while (is_id(*s) && n + 1 < sizeof(word)) {
        word[n++] = *s++;
    }
    word[n] = 0;
    while (*s == ' ' || *s == '\t') {
        s++;
    }

    if (!strcmp(word, "version") || !strcmp(word, "extension")) {
        return true;
    }
    if (!strcmp(word, "define")) {
        if (pre_active(p)) {
            define_macro(p, s);
        }
        return true;
    }
    if (!strcmp(word, "ifdef") || !strcmp(word, "ifndef")) {
        char name[128];
        size_t k = 0;
        while (is_id(*s) && k + 1 < sizeof(name)) {
            name[k++] = *s++;
        }
        name[k] = 0;
        bool def = find_macro(p, name) != NULL;
        if (!strcmp(word, "ifndef")) {
            def = !def;
        }
        if (p->ncond >= MAX_COND) {
            pre_fail(p, "macros nested too deeply");
            return true;
        }
        bool parent = pre_active(p);
        p->conds[p->ncond++] = (Cond){ parent, parent && def, false };
        return true;
    }
    if (!strcmp(word, "else")) {
        if (!p->ncond) {
            pre_fail(p, "#else without #if");
            return true;
        }
        Cond *c = &p->conds[p->ncond - 1];
        if (c->seen_else) {
            pre_fail(p, "duplicate #else");
            return true;
        }
        c->seen_else = true;
        c->active = c->parent_active && !c->active;
        return true;
    }
    if (!strcmp(word, "endif")) {
        if (!p->ncond) {
            pre_fail(p, "#endif without #if");
            return true;
        }
        p->ncond--;
        return true;
    }
    if (!strcmp(word, "if")) {
        pre_fail(p, "unsupported #if in shader source");
        return true;
    }
    pre_fail(p, "unsupported preprocessor directive");
    return true;
}

static char *strip_comments(const char *s)
{
    GString *o = g_string_new("");

    for (size_t i = 0; s[i];) {
        if (s[i] == '/' && s[i + 1] == '/') {
            while (s[i] && s[i] != '\n') {
                i++;
            }
        } else if (s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (s[i] && !(s[i] == '*' && s[i + 1] == '/')) {
                g_string_append_c(o, s[i] == '\n' ? '\n' : ' ');
                i++;
            }
            if (s[i]) {
                i += 2;
            }
        } else {
            g_string_append_c(o, s[i++]);
        }
    }
    return g_string_free(o, FALSE);
}

static char *preprocess(const char *src)
{
    Pre pre = { 0 };
    g_autofree char *bare = strip_comments(src);
    GString *o = g_string_new("");

    for (size_t i = 0; bare[i] && !pre.failed;) {
        size_t start = i;
        while (bare[i] && bare[i] != '\n') {
            i++;
        }
        char *line = g_strndup(bare + start, i - start);
        if (bare[i] == '\n') {
            i++;
        }
        if (!handle_directive(&pre, line) && pre_active(&pre)) {
            char *ex = expand_all(&pre, line, 0);
            g_string_append(o, ex);
            g_string_append_c(o, '\n');
            g_free(ex);
        }
        g_free(line);
    }
    if (pre.ncond && !pre.failed) {
        pre_fail(&pre, "unclosed #if");
    }
    for (int i = 0; i < pre.nmacros; i++) {
        g_free(pre.macros[i].name);
        g_free(pre.macros[i].body);
        for (int a = 0; a < pre.macros[i].nparams; a++) {
            g_free(pre.macros[i].params[a]);
        }
    }
    if (pre.failed) {
        g_string_free(o, TRUE);
        return NULL;
    }
    return g_string_free(o, FALSE);
}

/* Parser */

static void fail(Tr *t, const char *msg)
{
    if (t->failed) {
        return;
    }
    t->failed = true;
    size_t len = strlen(t->s);
    size_t at = MIN(t->i, len);
    size_t from = at > 48 ? at - 48 : 0;
    fprintf(stderr, "[WebGPU] WGSL translate: %s near '%.160s'\n", msg,
            t->s + from);
}

static void skip_ws(Tr *t)
{
    while (g_ascii_isspace(t->s[t->i])) {
        t->i++;
    }
}

static char curc(Tr *t)
{
    skip_ws(t);
    return t->s[t->i];
}

static bool consume(Tr *t, char c)
{
    if (curc(t) == c) {
        t->i++;
        return true;
    }
    return false;
}

static bool expect(Tr *t, char c)
{
    if (consume(t, c)) {
        return true;
    }
    fail(t, "unexpected token");
    return false;
}

static bool word_at(Tr *t, const char *w, bool eat)
{
    skip_ws(t);
    size_t n = strlen(w);
    if (strncmp(t->s + t->i, w, n) == 0 && !is_id(t->s[t->i + n])) {
        if (eat) {
            t->i += n;
        }
        return true;
    }
    return false;
}

static bool consume_word(Tr *t, const char *w)
{
    return word_at(t, w, true);
}

static bool peek_word(Tr *t, const char *w)
{
    return word_at(t, w, false);
}

static char *parse_ident(Tr *t)
{
    skip_ws(t);
    if (!is_id_start(t->s[t->i])) {
        fail(t, "expected a name");
        return g_strdup("");
    }
    size_t start = t->i;
    t->i++;
    while (is_id(t->s[t->i])) {
        t->i++;
    }
    return g_strndup(t->s + start, t->i - start);
}

static void emit(Tr *t, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

static void emit(Tr *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_string_append_vprintf(t->cur, fmt, ap);
    va_end(ap);
}

static bool add_sym(Sym *tab, int *n, int max, const char *name, int ty,
                    bool array, int scope)
{
    if (*n >= max) {
        return false;
    }
    tab[*n] = (Sym){ g_strdup(name), ty, array, scope };
    (*n)++;
    return true;
}

static Sym *find_sym(Sym *tab, int n, const char *name)
{
    for (int i = n - 1; i >= 0; i--) {
        if (!strcmp(tab[i].name, name)) {
            return &tab[i];
        }
    }
    return NULL;
}

static void push_scope(Tr *t)
{
    t->depth++;
}

static void pop_scope(Tr *t)
{
    while (t->nlocals && t->locals[t->nlocals - 1].scope == t->depth) {
        g_free(t->locals[t->nlocals - 1].name);
        t->nlocals--;
    }
    if (t->depth) {
        t->depth--;
    }
}

static void add_local(Tr *t, const char *name, int ty, bool array)
{
    if (!add_sym(t->locals, &t->nlocals, MAX_SYM, name, ty, array, t->depth)) {
        fail(t, "too many locals");
    }
}

static void add_global(Tr *t, const char *name, int ty, bool array)
{
    if (!add_sym(t->globals, &t->nglobals, MAX_SYM, name, ty, array, 0)) {
        fail(t, "too many globals");
    }
}

static void add_uniform(Tr *t, const char *name, int ty, bool array)
{
    if (!add_sym(t->uniforms, &t->nuniforms, MAX_SYM, name, ty, array, 0)) {
        fail(t, "too many uniforms");
    }
}

static void add_fn(Tr *t, const char *name, int ret, int nparams,
                   const int *params)
{
    if (t->nfns >= MAX_FN) {
        fail(t, "too many functions");
        return;
    }
    bool overload = false;
    for (int i = 0; i < t->nfns; i++) {
        if (!strcmp(t->fns[i].name, name)) {
            overload = true;
            break;
        }
    }
    Fn *fn = &t->fns[t->nfns++];
    fn->name = g_strdup(name);
    fn->ret = ret;
    fn->nparams = nparams;
    for (int i = 0; i < nparams; i++) {
        fn->params[i] = params[i];
    }
    if (!overload) {
        fn->wgsl_name = g_strdup(name);
    } else {
        GString *mangled = g_string_new(name);
        for (int i = 0; i < nparams; i++) {
            g_string_append_printf(mangled, "_%s", ty_tag(params[i]));
        }
        fn->wgsl_name = g_string_free(mangled, FALSE);
    }
}

static Fn *find_fn(Tr *t, const char *name, Expr *args, int n)
{
    Fn *loose = NULL;
    for (int i = 0; i < t->nfns; i++) {
        Fn *fn = &t->fns[i];
        if (strcmp(fn->name, name) || fn->nparams != n) {
            continue;
        }
        bool match = true;
        for (int a = 0; a < n; a++) {
            if (args[a].ty != TY_NONE && args[a].ty != TY_ABSTRACT &&
                fn->params[a] != args[a].ty) {
                match = false;
                break;
            }
        }
        if (match) {
            return fn;
        }
        if (!loose) {
            loose = fn;
        }
    }
    return loose;
}

static bool is_swizzle(const char *s, char *out)
{
    if (!s[0] || strlen(s) > 4) {
        return false;
    }
    const char *sets = NULL;
    if (strchr("xyzw", s[0])) {
        sets = "xyzw";
    } else if (strchr("rgba", s[0])) {
        sets = "rgba";
    } else if (strchr("stpq", s[0])) {
        sets = "xyzw";
    } else {
        return false;
    }
    const char *map_from =
        sets[0] == 'x' && strchr("stpq", s[0]) ? "stpq" : sets;
    if (strchr("stpq", s[0])) {
        map_from = "stpq";
    }
    for (size_t i = 0; s[i]; i++) {
        const char *hit = strchr(map_from, s[i]);
        if (!hit) {
            return false;
        }
        out[i] = "xyzw"[hit - map_from];
    }
    out[strlen(s)] = 0;
    return true;
}

static int swizzle_ty(int base, const char *sw)
{
    int n = strlen(sw);
    int sc = scalar_of(base);
    if (sc == TY_NONE || sc == TY_ABSTRACT || is_mat(base)) {
        return n == 1 ? TY_F32 : vec_of(TY_F32, n);
    }
    return vec_of(sc, n);
}

static Expr parse_expr(Tr *t);

static int parse_arg_list(Tr *t, Expr *args)
{
    int n = 0;
    expect(t, '(');
    if (consume(t, ')')) {
        return 0;
    }
    while (!t->failed && n < MAX_ARGS) {
        args[n++] = parse_expr(t);
        if (consume(t, ',')) {
            continue;
        }
        break;
    }
    expect(t, ')');
    return n;
}

static char *join_args(Expr *args, int n)
{
    GString *g = g_string_new("");
    for (int i = 0; i < n; i++) {
        if (i) {
            g_string_append(g, ", ");
        }
        g_string_append(g, args[i].text ? args[i].text : "");
    }
    return g_string_free(g, FALSE);
}

static void splat_scalars(Expr *args, int n, int ty)
{
    if (width_of(ty) <= 1) {
        return;
    }
    for (int i = 0; i < n; i++) {
        if (width_of(args[i].ty) == 1 && args[i].ty != TY_NONE) {
            args[i] = cast_to(ty, args[i]);
        }
    }
}

static Expr parse_number(Tr *t)
{
    skip_ws(t);
    const char *s = t->s;
    size_t i = t->i;
    bool is_float = false;
    bool is_hex = false;
    bool has_u = false;
    uint64_t hex = 0;

    if (s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        is_hex = true;
        i += 2;
        if (!g_ascii_isxdigit(s[i])) {
            fail(t, "bad hex literal");
            return e_none();
        }
        while (g_ascii_isxdigit(s[i])) {
            hex = hex * 16 + g_ascii_xdigit_value(s[i]);
            i++;
        }
    } else {
        if (!g_ascii_isdigit(s[i]) &&
            !(s[i] == '.' && g_ascii_isdigit(s[i + 1]))) {
            fail(t, "expected a number");
            return e_none();
        }
        while (g_ascii_isdigit(s[i])) {
            i++;
        }
        if (s[i] == '.' && g_ascii_isdigit(s[i + 1])) {
            is_float = true;
            i++;
            while (g_ascii_isdigit(s[i])) {
                i++;
            }
        }
        if (s[i] == 'e' || s[i] == 'E') {
            is_float = true;
            i++;
            if (s[i] == '+' || s[i] == '-') {
                i++;
            }
            while (g_ascii_isdigit(s[i])) {
                i++;
            }
        }
    }
    if ((s[i] == 'f' || s[i] == 'F') && !is_id(s[i + 1])) {
        is_float = true;
        i++;
    } else if ((s[i] == 'u' || s[i] == 'U') && !is_id(s[i + 1])) {
        has_u = true;
        i++;
    }

    char *text = g_strndup(t->s + t->i, i - t->i);
    if (is_float) {
        size_t len = strlen(text);
        if (len && (text[len - 1] == 'f' || text[len - 1] == 'F')) {
            text[len - 1] = 0;
        }
    }
    t->i = i;
    if (is_float) {
        return e_take(TY_F32, text);
    }
    if (has_u || (is_hex && hex > 0x7fffffff)) {
        if (!(has_u)) {
            char *with = g_strdup_printf("%su", text);
            g_free(text);
            text = with;
        }
        return e_take(TY_U32, text);
    }
    return e_take(TY_ABSTRACT, text);
}

static Expr parse_ctor(Tr *t, const char *tname)
{
    int ty = type_from_name(tname);
    if (consume(t, '[')) {
        Expr n = parse_number(t);
        int count = (int)strtol(n.text, NULL, 0);
        e_clear(&n);
        expect(t, ']');
        expect(t, '(');
        Expr args[MAX_ARGS];
        int nargs = 0;
        if (!consume(t, ')')) {
            while (!t->failed && nargs < MAX_ARGS) {
                args[nargs++] = parse_expr(t);
                if (!consume(t, ',')) {
                    break;
                }
            }
            expect(t, ')');
        }
        char *joined = join_args(args, nargs);
        char *text =
            g_strdup_printf("array<%s, %d>(%s)", ty_wgsl(ty), count, joined);
        g_free(joined);
        free_args(args, nargs);
        return e_take(TY_NONE, text);
    }

    Expr args[MAX_ARGS];
    int n = parse_arg_list(t, args);
    if (n == 1 && is_floaty(ty) && !is_mat(ty) && width_of(ty) > 1 &&
        scalar_of(args[0].ty) == TY_BOOL &&
        width_of(args[0].ty) == width_of(ty)) {
        char *text = g_strdup_printf("select(%s(0.0), %s(1.0), %s)",
                                     ty_wgsl(ty), ty_wgsl(ty), args[0].text);
        free_args(args, n);
        return e_take(ty, text);
    }
    if (is_floaty(ty) && !is_mat(ty) && n > 1) {
        for (int i = 0; i < n; i++) {
            if (is_concrete_int(args[i].ty)) {
                args[i] =
                    cast_to(vec_of(TY_F32, width_of(args[i].ty)), args[i]);
            }
        }
    }
    char *joined = join_args(args, n);
    char *text = g_strdup_printf("%s(%s)", ty_wgsl(ty), joined);
    g_free(joined);
    free_args(args, n);
    return e_take(ty, text);
}

static Expr parse_sampler(Tr *t, int kind)
{
    Expr args[MAX_ARGS];
    int n = parse_arg_list(t, args);
    Expr e = { 0 };
    if (n >= 2) {
        e.is_sampler = true;
        e.skind = kind;
        e.tex = g_strdup(args[0].text);
        e.smp = g_strdup(args[1].text);
    } else {
        fail(t, "sampler constructor needs a texture and a sampler");
    }
    free_args(args, n);
    return e;
}

static Expr builtin_call(Tr *t, const char *name, Expr *args, int n)
{
    const char *renamed = NULL;
    if (!strcmp(name, "dFdx")) {
        renamed = "dpdx";
    } else if (!strcmp(name, "dFdy")) {
        renamed = "dpdy";
    } else if (!strcmp(name, "inversesqrt")) {
        renamed = "inverseSqrt";
    }
    if (renamed) {
        char *joined = join_args(args, n);
        char *text = g_strdup_printf("%s(%s)", renamed, joined);
        int ty = n ? args[0].ty : TY_F32;
        g_free(joined);
        return e_take(ty, text);
    }

    if (!strcmp(name, "abs") || !strcmp(name, "sign") ||
        !strcmp(name, "floor") || !strcmp(name, "ceil") ||
        !strcmp(name, "round") || !strcmp(name, "trunc") ||
        !strcmp(name, "fract") || !strcmp(name, "normalize") ||
        !strcmp(name, "log2") || !strcmp(name, "exp2") ||
        !strcmp(name, "exp")) {
        char *joined = join_args(args, n);
        char *text = g_strdup_printf("%s(%s)", name, joined);
        int ty = n ? args[0].ty : TY_F32;
        g_free(joined);
        return e_take(ty == TY_ABSTRACT ? TY_F32 : ty, text);
    }

    if (!strcmp(name, "min") || !strcmp(name, "max") ||
        !strcmp(name, "clamp") || !strcmp(name, "pow") ||
        !strcmp(name, "fma") || !strcmp(name, "reflect")) {
        int ty = n ? args[0].ty : TY_F32;
        for (int i = 0; i < n; i++) {
            if (width_of(args[i].ty) > width_of(ty)) {
                ty = args[i].ty;
            }
        }
        splat_scalars(args, n, ty);
        char *joined = join_args(args, n);
        char *text = g_strdup_printf("%s(%s)", name, joined);
        g_free(joined);
        return e_take(ty, text);
    }

    if (!strcmp(name, "step") && n == 2) {
        if (width_of(args[1].ty) > 1 && width_of(args[0].ty) == 1) {
            args[0] = cast_to(args[1].ty, args[0]);
        }
        char *text =
            g_strdup_printf("step(%s, %s)", args[0].text, args[1].text);
        return e_take(args[1].ty, text);
    }

    if ((!strcmp(name, "dot") || !strcmp(name, "length") ||
         !strcmp(name, "distance")) &&
        n >= 1) {
        char *joined = join_args(args, n);
        char *text = g_strdup_printf("%s(%s)", name, joined);
        g_free(joined);
        return e_take(TY_F32, text);
    }

    if ((!strcmp(name, "any") || !strcmp(name, "all")) && n == 1) {
        char *text = g_strdup_printf("%s(%s)", name, args[0].text);
        return e_take(TY_BOOL, text);
    }

    if (!strcmp(name, "not") && n == 1) {
        char *text = g_strdup_printf("(!%s)", args[0].text);
        return e_take(args[0].ty == TY_NONE ? TY_BOOL : args[0].ty, text);
    }

    const char *cmp = NULL;
    if (!strcmp(name, "lessThan")) {
        cmp = "<";
    } else if (!strcmp(name, "greaterThan")) {
        cmp = ">";
    } else if (!strcmp(name, "lessThanEqual")) {
        cmp = "<=";
    } else if (!strcmp(name, "greaterThanEqual")) {
        cmp = ">=";
    } else if (!strcmp(name, "equal")) {
        cmp = "==";
    } else if (!strcmp(name, "notEqual")) {
        cmp = "!=";
    }
    if (cmp && n == 2) {
        int w = MAX(width_of(args[0].ty), width_of(args[1].ty));
        char *text =
            g_strdup_printf("(%s %s %s)", args[0].text, cmp, args[1].text);
        return e_take(vec_of(TY_BOOL, w), text);
    }

    if (!strcmp(name, "mix") && n == 3) {
        if (scalar_of(args[2].ty) == TY_BOOL) {
            if (width_of(args[0].ty) > 1 && width_of(args[2].ty) == 1) {
                args[2] =
                    cast_to(vec_of(TY_BOOL, width_of(args[0].ty)), args[2]);
            }
            char *text = g_strdup_printf("select(%s, %s, %s)", args[0].text,
                                         args[1].text, args[2].text);
            return e_take(args[0].ty, text);
        }
        if (width_of(args[0].ty) > 1 && width_of(args[2].ty) == 1) {
            args[2] = cast_to(args[0].ty, args[2]);
        }
        char *text = g_strdup_printf("mix(%s, %s, %s)", args[0].text,
                                     args[1].text, args[2].text);
        return e_take(args[0].ty, text);
    }

    if (!strcmp(name, "floatBitsToUint") && n == 1) {
        int dst = vec_of(TY_U32, width_of(args[0].ty));
        char *text =
            g_strdup_printf("bitcast<%s>(%s)", ty_wgsl(dst), args[0].text);
        return e_take(dst, text);
    }
    if (!strcmp(name, "uintBitsToFloat") && n == 1) {
        int w = width_of(args[0].ty);
        if (scalar_of(args[0].ty) != TY_U32) {
            args[0] = cast_to(vec_of(TY_U32, w), args[0]);
        }
        int dst = vec_of(TY_F32, width_of(args[0].ty));
        char *text =
            g_strdup_printf("bitcast<%s>(%s)", ty_wgsl(dst), args[0].text);
        return e_take(dst, text);
    }
    if (!strcmp(name, "bitfieldExtract") && n == 3) {
        if (args[1].ty == TY_I32) {
            args[1] = cast_to(TY_U32, args[1]);
        }
        if (args[2].ty == TY_I32) {
            args[2] = cast_to(TY_U32, args[2]);
        }
        char *text = g_strdup_printf("extractBits(%s, %s, %s)", args[0].text,
                                     args[1].text, args[2].text);
        int ty = is_concrete_int(args[0].ty) ? args[0].ty : TY_I32;
        return e_take(ty, text);
    }

    return e_none();
}

static bool is_builtin(const char *name)
{
    const char *names[] = {
        "dFdx",
        "dFdy",
        "inversesqrt",
        "abs",
        "sign",
        "floor",
        "ceil",
        "round",
        "trunc",
        "fract",
        "normalize",
        "log2",
        "exp2",
        "exp",
        "min",
        "max",
        "clamp",
        "pow",
        "fma",
        "reflect",
        "step",
        "dot",
        "length",
        "distance",
        "any",
        "all",
        "not",
        "lessThan",
        "greaterThan",
        "lessThanEqual",
        "greaterThanEqual",
        "equal",
        "notEqual",
        "mix",
        "floatBitsToUint",
        "uintBitsToFloat",
        "bitfieldExtract",
        "texture",
        "textureProj",
        "textureSize",
        "texelFetch",
    };
    for (int i = 0; i < ARRAY_SIZE(names); i++) {
        if (!strcmp(name, names[i])) {
            return true;
        }
    }
    return false;
}

static Expr texture_call(Tr *t, const char *name, Expr *args, int n)
{
    if (n < 2 || !args[0].is_sampler) {
        fail(t, "texture call is missing its sampler");
        return e_none();
    }
    const char *tex = args[0].tex;
    const char *smp = args[0].smp;
    int kind = args[0].skind;

    if (!strcmp(name, "textureSize")) {
        /* textureSize(sampler, lod). The generator always passes lod 0. */
        bool lod0 = n < 2 || !strcmp(args[1].text, "0");
        int ty = kind == SK_3D ? TY_IVEC3 : TY_IVEC2;
        char *text;
        if (lod0) {
            text =
                g_strdup_printf("%s(textureDimensions(%s))", ty_wgsl(ty), tex);
        } else {
            text = g_strdup_printf("%s(textureDimensions(%s, %s))", ty_wgsl(ty),
                                   tex, args[1].text);
        }
        return e_take(ty, text);
    }

    if (!strcmp(name, "texelFetch")) {
        const char *coord = n > 1 ? args[1].text : "vec2i(0)";
        const char *lod = n > 2 ? args[2].text : "0";
        char *text =
            g_strdup_printf("textureLoad(%s, %s, %s)", tex, coord, lod);
        return e_take(kind == SK_2D_UINT ? TY_UVEC4 : TY_VEC4, text);
    }

    Expr coord = args[1];
    bool owned = false;
    if (!strcmp(name, "textureProj")) {
        char *text;
        int ty;
        if (kind == SK_3D) {
            text = g_strdup_printf("(%s.xyz / %s.w)", coord.text, coord.text);
            ty = TY_VEC3;
        } else if (width_of(coord.ty) >= 4) {
            text = g_strdup_printf("(%s.xy / %s.w)", coord.text, coord.text);
            ty = TY_VEC2;
        } else {
            text = g_strdup_printf("(%s.xy / %s.z)", coord.text, coord.text);
            ty = TY_VEC2;
        }
        coord = e_take(ty, text);
        owned = true;
    }

    char *text;
    if (n >= 3) {
        text = g_strdup_printf("textureSampleBias(%s, %s, %s, %s)", tex, smp,
                               coord.text, args[2].text);
    } else {
        text =
            g_strdup_printf("textureSample(%s, %s, %s)", tex, smp, coord.text);
    }
    if (owned) {
        e_clear(&coord);
    }
    return e_take(TY_VEC4, text);
}

static Expr parse_call(Tr *t, char *name)
{
    Expr args[MAX_ARGS];
    int n = parse_arg_list(t, args);
    Expr result = e_none();

    if (!strcmp(name, "texture") || !strcmp(name, "textureProj") ||
        !strcmp(name, "textureSize") || !strcmp(name, "texelFetch")) {
        result = texture_call(t, name, args, n);
    } else if (is_builtin(name)) {
        Expr built = builtin_call(t, name, args, n);
        if (built.text && built.text[0]) {
            result = built;
        } else {
            e_clear(&built);
            fail(t, "unsupported builtin call");
        }
    } else {
        Fn *fn = find_fn(t, name, args, n);
        if (!fn) {
            fail(t, "unknown function");
        } else {
            char *joined = join_args(args, n);
            result = e_take(fn->ret,
                            g_strdup_printf("%s(%s)", fn->wgsl_name, joined));
            g_free(joined);
        }
    }
    free_args(args, n);
    g_free(name);
    return result;
}

static Expr parse_value(Tr *t, char *name)
{
    if (!strcmp(name, "gl_VertexIndex")) {
        t->uses_vertex_index = true;
        g_free(name);
        return e_take(TY_I32, g_strdup("gl_VertexIndex"));
    }
    if (!strcmp(name, "gl_FragCoord")) {
        t->uses_frag_coord = true;
        g_free(name);
        return e_take(TY_VEC4, g_strdup("frag_coord"));
    }
    if (!strcmp(name, "gl_FragDepth")) {
        if (!t->uses_frag_depth) {
            t->uses_frag_depth = true;
            g_string_append(t->decls, "var<private> gl_FragDepth: f32;\n");
        }
        g_free(name);
        return e_take(TY_F32, g_strdup("gl_FragDepth"));
    }
    if (!strcmp(name, "gl_Position")) {
        g_free(name);
        return e_take(TY_VEC4, g_strdup("gl_Position"));
    }
    if (!strcmp(name, "true")) {
        g_free(name);
        return e_take(TY_BOOL, g_strdup("true"));
    }
    if (!strcmp(name, "false")) {
        g_free(name);
        return e_take(TY_BOOL, g_strdup("false"));
    }

    Sym *sym = find_sym(t->locals, t->nlocals, name);
    if (!sym) {
        sym = find_sym(t->globals, t->nglobals, name);
    }
    if (sym) {
        Expr e = e_take(sym->ty, g_strdup(name));
        e.array = sym->array;
        g_free(name);
        return e;
    }
    sym = find_sym(t->uniforms, t->nuniforms, name);
    if (sym) {
        Expr e = e_take(sym->ty, g_strdup_printf("u_blk.%s", name));
        e.array = sym->array;
        g_free(name);
        return e;
    }
    fail(t, "unknown name");
    Expr e = e_take(TY_NONE, name);
    return e;
}

static Expr parse_primary(Tr *t)
{
    if (t->failed) {
        return e_none();
    }
    char c = curc(t);
    if (c == '(') {
        t->i++;
        Expr e = parse_expr(t);
        expect(t, ')');
        char *text = g_strdup_printf("(%s)", e.text);
        int ty = e.ty;
        bool array = e.array;
        bool samp = e.is_sampler;
        int skind = e.skind;
        bool swizzle = e.swizzle;
        int base_ty = e.base_ty;
        char sw[5];
        memcpy(sw, e.sw, sizeof(sw));
        char *tex = e.tex ? g_strdup(e.tex) : NULL;
        char *smp = e.smp ? g_strdup(e.smp) : NULL;
        char *base = e.base ? g_strdup(e.base) : NULL;
        e_clear(&e);
        Expr out = e_take(ty, text);
        out.array = array;
        out.is_sampler = samp;
        out.skind = skind;
        out.swizzle = swizzle;
        out.base_ty = base_ty;
        memcpy(out.sw, sw, sizeof(out.sw));
        out.tex = tex;
        out.smp = smp;
        out.base = base;
        return out;
    }
    if (g_ascii_isdigit(c) || (c == '.' && g_ascii_isdigit(t->s[t->i + 1]))) {
        return parse_number(t);
    }
    if (!is_id_start(c)) {
        fail(t, "expected an expression");
        return e_none();
    }
    char *name = parse_ident(t);
    int skind = sampler_kind(name);
    if (skind >= 0 && curc(t) == '(') {
        Expr e = parse_sampler(t, skind);
        g_free(name);
        return e;
    }
    if (is_type_name(name) && (curc(t) == '(' || curc(t) == '[')) {
        Expr e = parse_ctor(t, name);
        g_free(name);
        return e;
    }
    if (curc(t) == '(') {
        return parse_call(t, name);
    }
    return parse_value(t, name);
}

static Expr parse_postfix(Tr *t)
{
    Expr e = parse_primary(t);
    while (!t->failed) {
        if (consume(t, '.')) {
            char *field = parse_ident(t);
            char sw[8];
            if (!is_swizzle(field, sw)) {
                fail(t, "expected a swizzle");
                g_free(field);
                break;
            }
            char composed[5];
            char *base;
            int base_ty;
            if (e.swizzle && e.base) {
                base = g_strdup(e.base);
                base_ty = e.base_ty;
                size_t n = strlen(sw);
                for (size_t i = 0; i < n; i++) {
                    int comp = sw[i] - 'x';
                    composed[i] = (comp >= 0 && comp < (int)strlen(e.sw)) ?
                                      e.sw[comp] :
                                      'x';
                }
                composed[n] = 0;
            } else {
                base = g_strdup(e.text ? e.text : "");
                base_ty = e.ty;
                g_strlcpy(composed, sw, sizeof(composed));
            }
            char *text = g_strdup_printf("%s.%s", base, composed);
            int ty = swizzle_ty(e.ty, sw);
            e_clear(&e);
            e = e_take(ty, text);
            e.swizzle = width_of(base_ty) > 1;
            e.base_ty = base_ty;
            memcpy(e.sw, composed, sizeof(e.sw));
            e.base = base;
            g_free(field);
            continue;
        }
        if (consume(t, '[')) {
            Expr idx = parse_expr(t);
            expect(t, ']');
            int ty = e.ty;
            if (e.array) {
                e.array = false;
            } else if (is_mat(e.ty)) {
                ty = mat_col(e.ty);
            } else if (width_of(e.ty) > 1) {
                ty = scalar_of(e.ty);
            }
            char *text = g_strdup_printf("%s[%s]", e.text, idx.text);
            e_clear(&idx);
            e_clear(&e);
            e = e_take(ty, text);
            continue;
        }
        if (t->s[t->i] == '+' && t->s[t->i + 1] == '+') {
            t->i += 2;
            char *text = g_strdup_printf("%s++", e.text);
            int ty = e.ty;
            e_clear(&e);
            e = e_take(ty, text);
            continue;
        }
        if (t->s[t->i] == '-' && t->s[t->i + 1] == '-') {
            t->i += 2;
            char *text = g_strdup_printf("%s--", e.text);
            int ty = e.ty;
            e_clear(&e);
            e = e_take(ty, text);
            continue;
        }
        break;
    }
    return e;
}

static Expr parse_unary(Tr *t)
{
    if (t->failed) {
        return e_none();
    }
    skip_ws(t);
    if (t->s[t->i] == '+' && t->s[t->i + 1] == '+') {
        t->i += 2;
        Expr e = parse_unary(t);
        char *text = g_strdup_printf("++%s", e.text);
        int ty = e.ty;
        e_clear(&e);
        return e_take(ty, text);
    }
    if (t->s[t->i] == '-' && t->s[t->i + 1] == '-') {
        t->i += 2;
        Expr e = parse_unary(t);
        char *text = g_strdup_printf("--%s", e.text);
        int ty = e.ty;
        e_clear(&e);
        return e_take(ty, text);
    }
    char c = curc(t);
    if (c == '!' || c == '-' || c == '+' || c == '~') {
        t->i++;
        Expr e = parse_unary(t);
        char *text = g_strdup_printf("(%c%s)", c, e.text);
        int ty = c == '!' ?
                     (width_of(e.ty) > 1 ? vec_of(TY_BOOL, width_of(e.ty)) :
                                           TY_BOOL) :
                     e.ty;
        e_clear(&e);
        return e_take(ty, text);
    }
    return parse_postfix(t);
}

static int bin_prec(Tr *t, const char **op)
{
    skip_ws(t);
    const char *s = t->s + t->i;
    const struct {
        const char *op;
        int prec;
    } ops[] = {
        { "||", 1 }, { "&&", 2 }, { "|", 3 },  { "^", 4 },  { "&", 5 },
        { "==", 6 }, { "!=", 6 }, { "<=", 7 }, { ">=", 7 }, { "<", 7 },
        { ">", 7 },  { "<<", 8 }, { ">>", 8 }, { "+", 9 },  { "-", 9 },
        { "*", 10 }, { "/", 10 }, { "%", 10 },
    };
    for (int i = 0; i < ARRAY_SIZE(ops); i++) {
        size_t n = strlen(ops[i].op);
        if (strncmp(s, ops[i].op, n)) {
            continue;
        }
        if ((ops[i].op[0] == '<' || ops[i].op[0] == '>' ||
             ops[i].op[0] == '+' || ops[i].op[0] == '-' ||
             ops[i].op[0] == '=' || ops[i].op[0] == '!' ||
             ops[i].op[0] == '*' || ops[i].op[0] == '/' ||
             ops[i].op[0] == '%' || ops[i].op[0] == '&' ||
             ops[i].op[0] == '|' || ops[i].op[0] == '^') &&
            s[n] == '=') {
            continue;
        }
        if ((ops[i].op[0] == '<' && s[1] == '<') ||
            (ops[i].op[0] == '>' && s[1] == '>')) {
            if (n == 1) {
                continue;
            }
        }
        if ((ops[i].op[0] == '+' || ops[i].op[0] == '-') &&
            s[1] == ops[i].op[0]) {
            continue;
        }
        *op = ops[i].op;
        return ops[i].prec;
    }
    return 0;
}

static Expr parse_bin(Tr *t, int min_prec)
{
    Expr left = parse_unary(t);
    while (!t->failed) {
        const char *op = NULL;
        int prec = bin_prec(t, &op);
        if (prec < min_prec) {
            break;
        }
        t->i += strlen(op);
        Expr right = parse_bin(t, prec + 1);
        bool bitwise = !strcmp(op, "&") || !strcmp(op, "|") ||
                       !strcmp(op, "^") || !strcmp(op, "<<") ||
                       !strcmp(op, ">>");
        bool compare = !strcmp(op, "<") || !strcmp(op, ">") ||
                       !strcmp(op, "<=") || !strcmp(op, ">=") ||
                       !strcmp(op, "==") || !strcmp(op, "!=");
        bool logical = !strcmp(op, "&&") || !strcmp(op, "||");

        if (!bitwise && !compare && !logical && !is_mat(left.ty) &&
            !is_mat(right.ty)) {
            if (is_floaty(left.ty) && is_concrete_int(right.ty)) {
                right = cast_to(vec_of(TY_F32, width_of(right.ty)), right);
            } else if (is_floaty(right.ty) && is_concrete_int(left.ty)) {
                left = cast_to(vec_of(TY_F32, width_of(left.ty)), left);
            }
        }

        if (!strcmp(op, "*") && (is_mat(left.ty) || is_mat(right.ty))) {
            int ty = is_mat(left.ty) && is_mat(right.ty) ? left.ty :
                     is_mat(left.ty)                     ? mat_col(left.ty) :
                                                           mat_col(right.ty);
            char *text =
                g_strdup_printf("(%s %s %s)", left.text, op, right.text);
            e_clear(&left);
            e_clear(&right);
            left = e_take(ty, text);
            continue;
        }

        if (bitwise || compare) {
            if (width_of(left.ty) > 1 && width_of(right.ty) == 1) {
                int sty = scalar_of(left.ty);
                if (right.ty == TY_ABSTRACT) {
                    sty = scalar_of(left.ty);
                }
                right = cast_to(vec_of(sty == TY_ABSTRACT ? TY_I32 : sty,
                                       width_of(left.ty)),
                                right);
            } else if (width_of(right.ty) > 1 && width_of(left.ty) == 1) {
                int sty = scalar_of(right.ty);
                left = cast_to(vec_of(sty == TY_ABSTRACT ? TY_I32 : sty,
                                      width_of(right.ty)),
                               left);
            }
        }

        int ty = TY_NONE;
        bool vec_eq = false;
        if (logical) {
            ty = TY_BOOL;
        } else if (compare) {
            int w = MAX(width_of(left.ty), width_of(right.ty));
            if ((!strcmp(op, "==") || !strcmp(op, "!=")) &&
                width_of(left.ty) > 1 && width_of(right.ty) > 1) {
                vec_eq = true;
                ty = TY_BOOL;
            } else {
                ty = vec_of(TY_BOOL, w);
            }
        } else if (bitwise) {
            ty = width_of(left.ty) >= width_of(right.ty) ? left.ty : right.ty;
            if (ty == TY_ABSTRACT) {
                ty = left.ty == TY_ABSTRACT ? right.ty : left.ty;
            }
        } else {
            if (width_of(left.ty) >= width_of(right.ty) &&
                left.ty != TY_ABSTRACT) {
                ty = left.ty;
            } else if (right.ty != TY_ABSTRACT) {
                ty = right.ty;
            } else {
                ty = left.ty;
            }
            if (ty == TY_ABSTRACT) {
                ty = TY_ABSTRACT;
            }
        }

        char *inner = g_strdup_printf("(%s %s %s)", left.text, op, right.text);
        char *text = inner;
        if (vec_eq) {
            text = g_strdup_printf("%s(%s)", !strcmp(op, "==") ? "all" : "any",
                                   inner);
            g_free(inner);
        }
        e_clear(&left);
        e_clear(&right);
        left = e_take(ty, text);
    }
    return left;
}

static Expr parse_ternary(Tr *t)
{
    Expr cond = parse_bin(t, 1);
    if (!consume(t, '?')) {
        return cond;
    }
    Expr yes = parse_ternary(t);
    expect(t, ':');
    Expr no = parse_ternary(t);
    int ty = yes.ty != TY_NONE && yes.ty != TY_ABSTRACT ? yes.ty : no.ty;
    no = coerce(ty, no);
    yes = coerce(ty, yes);
    if (width_of(ty) > 1 && width_of(cond.ty) <= 1) {
        cond = cast_to(vec_of(TY_BOOL, width_of(ty)), cond);
    }
    char *text =
        g_strdup_printf("select(%s, %s, %s)", no.text, yes.text, cond.text);
    e_clear(&cond);
    e_clear(&yes);
    e_clear(&no);
    return e_take(ty, text);
}

static Expr parse_expr(Tr *t)
{
    return parse_ternary(t);
}

static const char *assign_op(Tr *t)
{
    skip_ws(t);
    const char *s = t->s + t->i;
    const char *ops[] = { "+=", "-=", "*=", "/=", "=" };
    for (int i = 0; i < ARRAY_SIZE(ops); i++) {
        size_t n = strlen(ops[i]);
        if (!strncmp(s, ops[i], n) && (ops[i][0] != '=' || s[1] != '=')) {
            return ops[i];
        }
    }
    return NULL;
}

static void parse_stmt(Tr *t);
static void parse_block(Tr *t);

static void parse_stmt_block(Tr *t)
{
    if (curc(t) == '{') {
        parse_block(t);
        return;
    }
    emit(t, "{\n");
    parse_stmt(t);
    emit(t, "}\n");
}

static bool parse_type(Tr *t, int *ty)
{
    skip_ws(t);
    if (!is_id_start(t->s[t->i])) {
        return false;
    }
    size_t save = t->i;
    char *name = parse_ident(t);
    int found = type_from_name(name);
    g_free(name);
    if (found < 0) {
        t->i = save;
        return false;
    }
    *ty = found;
    return true;
}

static void parse_var(Tr *t, int ty, char *name, bool global, bool eat_semi,
                      char **out_text)
{
    bool array = false;
    int count = 0;
    if (consume(t, '[')) {
        Expr n = parse_number(t);
        count = (int)strtol(n.text, NULL, 0);
        e_clear(&n);
        expect(t, ']');
        array = true;
    }
    Expr init = { 0 };
    bool has_init = consume(t, '=');
    if (has_init) {
        init = parse_expr(t);
        init = coerce(array ? TY_NONE : ty, init);
    }
    if (eat_semi) {
        expect(t, ';');
    }
    if (global) {
        add_global(t, name, ty, array);
        if (array) {
            emit(t, "var<private> %s: array<%s, %d>;\n", name, ty_wgsl(ty),
                 count);
        } else {
            emit(t, "var<private> %s: %s;\n", name, ty_wgsl(ty));
        }
        if (has_init) {
            g_string_append_printf(t->preamble, "%s = %s;\n", name, init.text);
        }
    } else if (out_text) {
        add_local(t, name, ty, array);
        *out_text = has_init ? g_strdup_printf("var %s: %s = %s", name,
                                               ty_wgsl(ty), init.text) :
                               g_strdup_printf("var %s: %s", name, ty_wgsl(ty));
    } else {
        add_local(t, name, ty, array);
        if (has_init) {
            emit(t, "var %s: %s = %s;\n", name, ty_wgsl(ty), init.text);
        } else {
            emit(t, "var %s: %s;\n", name, ty_wgsl(ty));
        }
    }
    e_clear(&init);
    g_free(name);
}

static void parse_for(Tr *t)
{
    expect(t, '(');
    push_scope(t);
    char *init = g_strdup("");
    if (!consume(t, ';')) {
        int ty = 0;
        if (parse_type(t, &ty)) {
            char *name = parse_ident(t);
            char *text = NULL;
            parse_var(t, ty, name, false, false, &text);
            g_free(init);
            init = text ? text : g_strdup("");
        } else {
            Expr e = parse_expr(t);
            g_free(init);
            init = g_strdup(e.text);
            e_clear(&e);
        }
        expect(t, ';');
    }
    char *cond = g_strdup("true");
    if (!consume(t, ';')) {
        Expr e = parse_expr(t);
        g_free(cond);
        cond = g_strdup(e.text);
        e_clear(&e);
        expect(t, ';');
    }
    char *step = g_strdup("");
    if (curc(t) != ')') {
        Expr e = parse_expr(t);
        g_free(step);
        step = g_strdup(e.text);
        e_clear(&e);
    }
    expect(t, ')');
    emit(t, "for (%s; %s; %s) ", init, cond, step);
    g_free(init);
    g_free(cond);
    g_free(step);
    parse_stmt_block(t);
    pop_scope(t);
}

/* Chrome rejects `v.xyz =` and `v.xy *=`. Write the whole vector instead.
 * A repeated swizzle component keeps the last write. */
static void emit_swizzle_store(Tr *t, const Expr *lhs, const char *op,
                               const Expr *rhs)
{
    int width = width_of(lhs->base_ty);
    int n = strlen(lhs->sw);
    if (!lhs->base || n < 1 || n > 4 || width < n || width > 4) {
        emit(t, "%s %s %s;\n", lhs->text, op, rhs->text);
        return;
    }
    emit(t, "{\n");
    if (!strcmp(op, "=")) {
        emit(t, "let _swz_new = %s;\n", rhs->text);
    } else {
        emit(t, "let _swz_new = (%s %c %s);\n", lhs->text, op[0], rhs->text);
    }
    emit(t, "%s = %s(", lhs->base, ty_wgsl(lhs->base_ty));
    for (int i = 0; i < width; i++) {
        int src = -1;
        if (i) {
            emit(t, ", ");
        }
        for (int j = 0; j < n; j++) {
            if (lhs->sw[j] == "xyzw"[i]) {
                src = j;
            }
        }
        if (src < 0) {
            emit(t, "%s.%c", lhs->base, "xyzw"[i]);
        } else if (n == 1) {
            emit(t, "_swz_new");
        } else {
            emit(t, "_swz_new.%c", "xyzw"[src]);
        }
    }
    emit(t, ");\n}\n");
}

static void parse_stmt(Tr *t)
{
    if (t->failed) {
        return;
    }
    size_t before = t->i;
    if (consume(t, ';')) {
        return;
    }
    if (curc(t) == '{') {
        parse_block(t);
        return;
    }
    if (consume_word(t, "if")) {
        expect(t, '(');
        Expr cond = parse_expr(t);
        expect(t, ')');
        emit(t, "if (%s) ", cond.text);
        e_clear(&cond);
        parse_stmt_block(t);
        if (consume_word(t, "else")) {
            emit(t, "else ");
            parse_stmt_block(t);
        }
        return;
    }
    if (consume_word(t, "for")) {
        parse_for(t);
        return;
    }
    if (consume_word(t, "while")) {
        expect(t, '(');
        Expr cond = parse_expr(t);
        expect(t, ')');
        emit(t, "while (%s) ", cond.text);
        e_clear(&cond);
        parse_stmt_block(t);
        return;
    }
    if (consume_word(t, "break")) {
        expect(t, ';');
        emit(t, "break;\n");
        return;
    }
    if (consume_word(t, "continue")) {
        expect(t, ';');
        emit(t, "continue;\n");
        return;
    }
    if (consume_word(t, "discard")) {
        expect(t, ';');
        emit(t, "discard;\n");
        return;
    }
    if (consume_word(t, "return")) {
        if (consume(t, ';')) {
            emit(t, "return;\n");
            return;
        }
        Expr e = parse_expr(t);
        expect(t, ';');
        emit(t, "return %s;\n", e.text);
        e_clear(&e);
        return;
    }
    if (consume_word(t, "switch") || consume_word(t, "do")) {
        fail(t, "unsupported statement");
        return;
    }

    bool was_precise = consume_word(t, "precise");
    size_t save = t->i;
    int ty = 0;
    if (parse_type(t, &ty)) {
        if (is_id_start(curc(t))) {
            char *name = parse_ident(t);
            parse_var(t, ty, name, false, true, NULL);
            return;
        }
        t->i = save;
    } else if (was_precise) {
        t->i = save;
    }

    Expr e = parse_expr(t);
    const char *op = assign_op(t);
    if (op) {
        t->i += strlen(op);
        Expr rhs = parse_expr(t);
        if (!strcmp(op, "=") || is_floaty(e.ty)) {
            if (is_floaty(e.ty) && is_concrete_int(rhs.ty)) {
                rhs = cast_to(vec_of(TY_F32, width_of(rhs.ty)), rhs);
            } else if (!strcmp(op, "=")) {
                rhs = coerce(e.ty, rhs);
            }
        }
        expect(t, ';');
        if (e.swizzle) {
            emit_swizzle_store(t, &e, op, &rhs);
        } else {
            emit(t, "%s %s %s;\n", e.text, op, rhs.text);
        }
        e_clear(&rhs);
    } else {
        expect(t, ';');
        emit(t, "%s;\n", e.text);
    }
    e_clear(&e);
    if (!t->failed && t->i == before) {
        fail(t, "statement parser made no progress");
    }
}

static void parse_block(Tr *t)
{
    expect(t, '{');
    emit(t, "{\n");
    push_scope(t);
    while (!t->failed && curc(t) && curc(t) != '}') {
        size_t before = t->i;
        parse_stmt(t);
        if (!t->failed && t->i == before) {
            fail(t, "block parser made no progress");
            break;
        }
    }
    expect(t, '}');
    emit(t, "}\n");
    pop_scope(t);
}

static void parse_params_and_body(Tr *t, const char *name, int ret)
{
    expect(t, '(');
    push_scope(t);
    int params[MAX_PARAMS];
    char *pnames[MAX_PARAMS];
    int nparams = 0;
    if (!consume(t, ')')) {
        while (!t->failed && nparams < MAX_PARAMS) {
            int ty = 0;
            if (!parse_type(t, &ty)) {
                fail(t, "expected a parameter type");
                break;
            }
            pnames[nparams] = parse_ident(t);
            params[nparams] = ty;
            add_local(t, pnames[nparams], ty, false);
            nparams++;
            if (!consume(t, ',')) {
                break;
            }
        }
        expect(t, ')');
    }
    add_fn(t, name, ret, nparams, params);
    if (t->failed) {
        for (int i = 0; i < nparams; i++) {
            g_free(pnames[i]);
        }
        pop_scope(t);
        return;
    }

    GString *saved = t->cur;
    GString *body = g_string_new("");
    t->cur = body;
    parse_block(t);
    t->cur = saved;
    pop_scope(t);

    GString *sig = g_string_new("");
    g_string_append_printf(sig, "fn %s(", t->fns[t->nfns - 1].wgsl_name);
    for (int i = 0; i < nparams; i++) {
        if (i) {
            g_string_append(sig, ", ");
        }
        g_string_append_printf(sig, "_arg_%s: %s", pnames[i],
                               ty_wgsl(params[i]));
    }
    g_string_append_printf(sig, ") -> %s {\n", ty_wgsl(ret));
    for (int i = 0; i < nparams; i++) {
        g_string_append_printf(sig, "var %s: %s = _arg_%s;\n", pnames[i],
                               ty_wgsl(params[i]), pnames[i]);
        g_free(pnames[i]);
    }
    g_string_append_printf(sig, "%s}\n", body->str);
    g_string_append(t->decls, sig->str);
    g_string_free(sig, TRUE);
    g_string_free(body, TRUE);
}

static int interp_from_word(Tr *t)
{
    if (consume_word(t, "flat")) {
        return INTERP_FLAT;
    }
    if (consume_word(t, "noperspective")) {
        return INTERP_LINEAR;
    }
    consume_word(t, "smooth");
    return INTERP_SMOOTH;
}

static void parse_io(Tr *t, int location, int preset_interp, bool have_interp)
{
    int interp = have_interp ? preset_interp : interp_from_word(t);
    bool input;
    if (consume_word(t, "in")) {
        input = true;
    } else if (consume_word(t, "out")) {
        input = false;
    } else {
        fail(t, "expected in or out");
        return;
    }
    int ty = 0;
    if (!parse_type(t, &ty)) {
        fail(t, "expected an IO type");
        return;
    }
    char *name = parse_ident(t);
    expect(t, ';');
    if (t->nios >= MAX_IO) {
        fail(t, "too many shader inputs");
        g_free(name);
        return;
    }
    t->ios[t->nios++] = (IO){ g_strdup(name), ty, location, interp, input };
    if (!input) {
        g_string_append_printf(t->decls, "var<private> %s: %s;\n", name,
                               ty_wgsl(ty));
        add_global(t, name, ty, false);
    }
    g_free(name);
}

static void parse_uniform_block(Tr *t, int set, int binding, char *bname)
{
    expect(t, '{');
    GString *body = g_string_new("");
    while (!t->failed && curc(t) && curc(t) != '}') {
        int ty = 0;
        if (!parse_type(t, &ty)) {
            fail(t, "expected a uniform type");
            break;
        }
        char *name = parse_ident(t);
        bool array = false;
        int count = 0;
        if (consume(t, '[')) {
            Expr n = parse_number(t);
            count = (int)strtol(n.text, NULL, 0);
            e_clear(&n);
            expect(t, ']');
            array = true;
        }
        expect(t, ';');
        add_uniform(t, name, ty, array);
        if (array) {
            g_string_append_printf(body, "    %s: array<%s, %d>,\n", name,
                                   ty_wgsl(ty), count);
        } else {
            g_string_append_printf(body, "    %s: %s,\n", name, ty_wgsl(ty));
        }
        g_free(name);
    }
    expect(t, '}');
    expect(t, ';');
    g_string_append_printf(t->decls,
                           "struct %s {\n%s}\n@group(%d) @binding(%d) "
                           "var<uniform> u_blk: %s;\n",
                           bname, body->str, set, binding, bname);
    g_string_free(body, TRUE);
    g_free(bname);
}

static int texture_ty(const char *name)
{
    if (!strcmp(name, "texture2D")) {
        return SK_2D;
    }
    if (!strcmp(name, "texture3D")) {
        return SK_3D;
    }
    if (!strcmp(name, "textureCube")) {
        return SK_CUBE;
    }
    if (!strcmp(name, "utexture2D")) {
        return SK_2D_UINT;
    }
    if (!strcmp(name, "sampler")) {
        return -2;
    }
    return -1;
}

static void parse_resource(Tr *t, int set, int binding)
{
    char *tname = parse_ident(t);
    int kind = texture_ty(tname);
    char *name = parse_ident(t);
    expect(t, ';');
    const char *wgsl = "f32";
    if (kind == -2) {
        wgsl = NULL;
        g_string_append_printf(t->decls,
                               "@group(%d) @binding(%d) var %s: sampler;\n",
                               set, binding, name);
        add_global(t, name, TY_SAMPLER, false);
    } else if (kind == SK_3D) {
        g_string_append_printf(
            t->decls, "@group(%d) @binding(%d) var %s: texture_3d<f32>;\n", set,
            binding, name);
        add_global(t, name, TY_F32, false);
    } else if (kind == SK_CUBE) {
        g_string_append_printf(
            t->decls, "@group(%d) @binding(%d) var %s: texture_cube<f32>;\n",
            set, binding, name);
        add_global(t, name, TY_F32, false);
    } else if (kind == SK_2D_UINT) {
        g_string_append_printf(
            t->decls, "@group(%d) @binding(%d) var %s: texture_2d<u32>;\n", set,
            binding, name);
        add_global(t, name, TY_U32, false);
    } else {
        g_string_append_printf(
            t->decls, "@group(%d) @binding(%d) var %s: texture_2d<%s>;\n", set,
            binding, name, wgsl);
        add_global(t, name, TY_F32, false);
    }
    g_free(tname);
    g_free(name);
}

static void parse_layout(Tr *t)
{
    expect(t, '(');
    int set = 0, binding = 0, location = -1;
    while (!t->failed && curc(t) && curc(t) != ')') {
        char *key = parse_ident(t);
        int value = 0;
        bool have_value = false;
        if (consume(t, '=')) {
            if (g_ascii_isdigit(curc(t))) {
                Expr n = parse_number(t);
                value = (int)strtol(n.text, NULL, 0);
                e_clear(&n);
                have_value = true;
            } else {
                char *v = parse_ident(t);
                g_free(v);
            }
        }
        if (!strcmp(key, "set") && have_value) {
            set = value;
        } else if (!strcmp(key, "binding") && have_value) {
            binding = value;
        } else if (!strcmp(key, "location") && have_value) {
            location = value;
        }
        g_free(key);
        if (!consume(t, ',')) {
            break;
        }
    }
    expect(t, ')');

    if (consume_word(t, "uniform")) {
        char *next = parse_ident(t);
        int tres = texture_ty(next);
        if (tres != -1) {
            /* parse_resource expects to read the type name itself */
            size_t back = strlen(next);
            t->i -= back;
            g_free(next);
            parse_resource(t, set, binding);
            return;
        }
        if (curc(t) == '{') {
            parse_uniform_block(t, set, binding, next);
            return;
        }
        fail(t, "unsupported uniform declaration");
        g_free(next);
        return;
    }
    parse_io(t, location, INTERP_SMOOTH, false);
}

static void parse_const_decl(Tr *t)
{
    int ty = 0;
    if (!parse_type(t, &ty)) {
        fail(t, "expected a const type");
        return;
    }
    bool array = false;
    if (consume(t, '[')) {
        Expr n = parse_number(t);
        e_clear(&n);
        expect(t, ']');
        array = true;
    }
    char *name = parse_ident(t);
    expect(t, '=');
    Expr init = parse_expr(t);
    expect(t, ';');
    add_global(t, name, ty, array);
    if (array) {
        emit(t, "const %s = %s;\n", name, init.text);
    } else {
        emit(t, "const %s: %s = %s;\n", name, ty_wgsl(ty), init.text);
    }
    e_clear(&init);
    g_free(name);
}

static void parse_main(Tr *t)
{
    expect(t, '(');
    expect(t, ')');
    t->saw_main = true;
    push_scope(t);
    for (int i = 0; i < t->nios; i++) {
        if (t->ios[i].input) {
            add_local(t, t->ios[i].name, t->ios[i].ty, false);
        }
    }
    GString *saved = t->cur;
    t->cur = t->main_body;
    parse_block(t);
    t->cur = saved;
    pop_scope(t);
}

static void parse_toplevel(Tr *t)
{
    while (!t->failed && curc(t)) {
        size_t before = t->i;
        if (consume_word(t, "layout")) {
            parse_layout(t);
        } else if (peek_word(t, "in") || peek_word(t, "out") ||
                   peek_word(t, "flat") || peek_word(t, "noperspective") ||
                   peek_word(t, "smooth")) {
            parse_io(t, t->nios, INTERP_SMOOTH, false);
        } else if (consume_word(t, "const")) {
            parse_const_decl(t);
        } else {
            consume_word(t, "precise");
            int ty = 0;
            if (!parse_type(t, &ty)) {
                fail(t, "expected a declaration");
                break;
            }
            char *name = parse_ident(t);
            if (curc(t) == '(') {
                if (!strcmp(name, "main")) {
                    g_free(name);
                    parse_main(t);
                } else {
                    parse_params_and_body(t, name, ty);
                    g_free(name);
                }
            } else {
                parse_var(t, ty, name, true, true, NULL);
            }
        }
        if (!t->failed && t->i == before) {
            fail(t, "declaration parser made no progress");
            break;
        }
    }
}

static const char *interp_attr(int interp)
{
    if (interp == INTERP_FLAT) {
        return "@interpolate(flat) ";
    }
    if (interp == INTERP_LINEAR) {
        return "@interpolate(linear) ";
    }
    return "";
}

static char *assemble(Tr *t)
{
    GString *out = g_string_new("");
    if (!t->vertex) {
        g_string_append(out, "diagnostic(off, derivative_uniformity);\n\n");
    }
    if (t->vertex && t->uses_vertex_index) {
        g_string_append(out, "var<private> gl_VertexIndex: i32;\n");
    }
    g_string_append(out, t->decls->str);

    GString *params = g_string_new("");
    int np = 0;
    for (int i = 0; i < t->nios; i++) {
        if (!t->ios[i].input) {
            continue;
        }
        if (np++) {
            g_string_append(params, ", ");
        }
        if (t->vertex) {
            g_string_append_printf(params, "@location(%d) %s: %s",
                                   t->ios[i].location, t->ios[i].name,
                                   ty_wgsl(t->ios[i].ty));
        } else {
            g_string_append_printf(params, "@location(%d) %s%s: %s",
                                   t->ios[i].location,
                                   interp_attr(t->ios[i].interp),
                                   t->ios[i].name, ty_wgsl(t->ios[i].ty));
        }
    }
    if (t->vertex && t->uses_vertex_index) {
        if (np++) {
            g_string_append(params, ", ");
        }
        g_string_append(params, "@builtin(vertex_index) vertex_index: u32");
    }
    if (!t->vertex && t->uses_frag_coord) {
        if (np++) {
            g_string_append(params, ", ");
        }
        g_string_append(params, "@builtin(position) frag_coord: vec4f");
    }

    const char *ret = t->vertex ? "VSOut" : "FSOut";
    g_string_append_printf(out, "struct %s {\n", ret);
    for (int i = 0; i < t->nios; i++) {
        if (t->ios[i].input) {
            continue;
        }
        if (t->vertex) {
            g_string_append_printf(out, "    @location(%d) %s%s: %s,\n",
                                   t->ios[i].location,
                                   interp_attr(t->ios[i].interp),
                                   t->ios[i].name, ty_wgsl(t->ios[i].ty));
        } else {
            g_string_append_printf(out, "    @location(%d) %s: %s,\n",
                                   t->ios[i].location, t->ios[i].name,
                                   ty_wgsl(t->ios[i].ty));
        }
    }
    if (t->vertex) {
        g_string_append(out, "    @builtin(position) position: vec4f,\n");
    }
    if (t->uses_frag_depth) {
        g_string_append(out, "    @builtin(frag_depth) frag_depth: f32,\n");
    }
    g_string_append(out, "}\n\n");

    g_string_append_printf(out, "%s\nfn %s(%s) -> %s {\n",
                           t->vertex ? "@vertex" : "@fragment",
                           t->vertex ? "vs" : "fs", params->str, ret);
    g_string_free(params, TRUE);
    if (t->vertex && t->uses_vertex_index) {
        g_string_append(out, "gl_VertexIndex = i32(vertex_index);\n");
    }
    g_string_append(out, t->preamble->str);
    g_string_append(out, t->main_body->str);
    g_string_append_printf(out, "var _out: %s;\n", ret);
    for (int i = 0; i < t->nios; i++) {
        if (!t->ios[i].input) {
            g_string_append_printf(out, "_out.%s = %s;\n", t->ios[i].name,
                                   t->ios[i].name);
        }
    }
    if (t->vertex) {
        g_string_append(out, "_out.position = gl_Position;\n");
    }
    if (t->uses_frag_depth) {
        g_string_append(out, "_out.frag_depth = gl_FragDepth;\n");
    }
    g_string_append(out, "return _out;\n}\n");
    return g_string_free(out, FALSE);
}

static void tr_free(Tr *t)
{
    if (!t) {
        return;
    }
    if (t->decls) {
        g_string_free(t->decls, TRUE);
    }
    if (t->preamble) {
        g_string_free(t->preamble, TRUE);
    }
    if (t->main_body) {
        g_string_free(t->main_body, TRUE);
    }
    for (int i = 0; i < t->nglobals; i++) {
        g_free(t->globals[i].name);
    }
    for (int i = 0; i < t->nlocals; i++) {
        g_free(t->locals[i].name);
    }
    for (int i = 0; i < t->nuniforms; i++) {
        g_free(t->uniforms[i].name);
    }
    for (int i = 0; i < t->nfns; i++) {
        g_free(t->fns[i].name);
        g_free(t->fns[i].wgsl_name);
    }
    for (int i = 0; i < t->nios; i++) {
        g_free(t->ios[i].name);
    }
}

MString *pgraph_wgsl_translate(const char *glsl, bool vertex)
{
    g_autofree char *expanded = preprocess(glsl);
    if (!expanded) {
        return NULL;
    }

    Tr t = { 0 };
    t.s = expanded;
    t.vertex = vertex;
    t.decls = g_string_new("");
    t.preamble = g_string_new("");
    t.main_body = g_string_new("");
    t.cur = t.decls;
    if (vertex) {
        g_string_append(t.decls, "var<private> gl_Position: vec4f;\n");
    }
    parse_toplevel(&t);
    if (!t.failed && !t.saw_main) {
        fail(&t, "shader has no main");
    }

    MString *result = NULL;
    if (!t.failed) {
        g_autofree char *wgsl = assemble(&t);
        result = mstring_from_str(wgsl);
    }
    tr_free(&t);
    return result;
}
