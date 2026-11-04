/*
 * vars.c - per-shell variables.
 *
 * Each tab keeps its own variable table instead of using the process
 * environment (which threads would share).  Exported variables are turned
 * into an envp array only in the forked child, right before execve().
 */
#include "shell.h"

#include <stdlib.h>
#include <string.h>

static Var *find(Shell *sh, const char *name)
{
    for (Var *v = sh->vars; v; v = v->next)
        if (strcmp(v->name, name) == 0)
            return v;
    return NULL;
}

const char *var_get(Shell *sh, const char *name)
{
    Var *v = find(sh, name);
    return v ? v->value : NULL;
}

void var_set(Shell *sh, const char *name, const char *value, int export_flag)
{
    Var *v = find(sh, name);
    if (!v) {
        v = xcalloc(1, sizeof *v);
        v->name = xstrdup(name);
        v->next = sh->vars;
        sh->vars = v;
    } else {
        free(v->value);
    }
    v->value = xstrdup(value ? value : "");
    if (export_flag)
        v->exported = 1;
}

void var_unset(Shell *sh, const char *name)
{
    for (Var **pp = &sh->vars; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->name, name) == 0) {
            Var *v = *pp;
            *pp = v->next;
            free(v->name);
            free(v->value);
            free(v);
            return;
        }
    }
}

void var_export(Shell *sh, const char *name)
{
    Var *v = find(sh, name);
    if (v)
        v->exported = 1;
    else
        var_set(sh, name, "", 1);
}

void vars_init_from_env(Shell *sh, char **envp)
{
    for (char **e = envp; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq)
            continue;
        char *name = xstrndup(*e, (size_t)(eq - *e));
        var_set(sh, name, eq + 1, 1);
        free(name);
    }
}

void vars_free(Shell *sh)
{
    for (Var *v = sh->vars, *nx; v; v = nx) {
        nx = v->next;
        free(v->name);
        free(v->value);
        free(v);
    }
    sh->vars = NULL;
}

Var *vars_copy(Var *v)
{
    Var *head = NULL, **tail = &head;
    for (; v; v = v->next) {
        Var *c = xcalloc(1, sizeof *c);
        c->name = xstrdup(v->name);
        c->value = xstrdup(v->value);
        c->exported = v->exported;
        *tail = c;
        tail = &c->next;
    }
    return head;
}

/* envp for execve: exported variables plus "NAME=value" prefix
 * assignments of this one command (extra), which take precedence. */
char **vars_build_env(Shell *sh, StrVec *extra)
{
    StrVec env;
    sv_init(&env);
    for (Var *v = sh->vars; v; v = v->next) {
        if (!v->exported)
            continue;
        int overridden = 0;
        size_t nl = strlen(v->name);
        for (int i = 0; extra && i < extra->n; i++)
            if (strncmp(extra->v[i], v->name, nl) == 0 && extra->v[i][nl] == '=')
                overridden = 1;
        if (!overridden)
            sv_push(&env, xasprintf("%s=%s", v->name, v->value));
    }
    for (int i = 0; extra && i < extra->n; i++)
        sv_pushdup(&env, extra->v[i]);
    if (!env.v)
        sv_push(&env, xstrdup("TERMINATOR=1"));
    return env.v;
}
