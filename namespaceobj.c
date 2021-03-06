/*
    $Id: namespaceobj.c 2136 2020-01-01 23:01:52Z soci $

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

*/
#include "namespaceobj.h"
#include <string.h>
#include "variables.h"
#include "eval.h"
#include "error.h"
#include "arguments.h"
#include "64tass.h"

#include "listobj.h"
#include "strobj.h"
#include "operobj.h"
#include "typeobj.h"
#include "noneobj.h"
#include "labelobj.h"
#include "errorobj.h"
#include "identobj.h"
#include "codeobj.h"
#include "macroobj.h"
#include "mfuncobj.h"

static Type obj;

Type *const NAMESPACE_OBJ = &obj;

static MUST_CHECK Obj *create(Obj *v1, linepos_t epoint) {
    switch (v1->obj->type) {
    case T_NONE:
    case T_ERROR:
    case T_NAMESPACE: return val_reference(v1);
    case T_CODE:
        return val_reference(&((Code *)v1)->names->v);
    case T_UNION:
    case T_STRUCT:
        return val_reference(&((Struct *)v1)->names->v);
    case T_MFUNC:
        return val_reference(&((Mfunc *)v1)->names->v);
    default: break;
    }
    return (Obj *)new_error_conv(v1, NAMESPACE_OBJ, epoint);
}

static FAST_CALL void destroy(Obj *o1) {
    Namespace *v1 = (Namespace *)o1;
    size_t i;
    if (v1->data == NULL) return;
    for (i = 0; i <= v1->mask; i++) {
        if (v1->data[i] != NULL) val_destroy(&v1->data[i]->v);
    }
    free(v1->data);
}

static FAST_CALL void garbage(Obj *o1, int j) {
    Namespace *v1 = (Namespace *)o1;
    size_t i;
    if (v1->data == NULL) return;
    switch (j) {
    case -1:
        for (i = 0; i <= v1->mask; i++) {
            if (v1->data[i] != NULL) v1->data[i]->v.refcount--;
        }
        return;
    case 0:
        free(v1->data);
        return;
    case 1:
        for (i = 0; i <= v1->mask; i++) {
            Obj *v = &v1->data[i]->v;
            if (v == NULL) continue;
            if ((v->refcount & SIZE_MSB) != 0) {
                v->refcount -= SIZE_MSB - 1;
                v->obj->garbage(v, 1);
            } else v->refcount++;
        }
        return;
    }
}

static Label *namespace_lookup(const Namespace *ns, const Label *p) {
    size_t mask = ns->mask;
    size_t hash = (size_t)p->hash;
    size_t offs = hash & mask;
    if (ns->data == NULL) return NULL;
    while (ns->data[offs] != NULL) {
        Label *d = ns->data[offs];
        if (p->hash == d->hash && p->strength == d->strength) {
            if (d->defpass == pass || (d->constant && (!fixeddig || d->defpass == pass - 1))) {
                const str_t *s1 = &p->cfname;
                const str_t *s2 = &d->cfname;
                if (s1->len == s2->len && (s1->data == s2->data || memcmp(s1->data, s2->data, s1->len) == 0)) {
                    return d;
                }
            }
        }
        hash >>= 5;
        offs = (5 * offs + hash + 1) & mask;
    }
    return NULL;
}

static bool namespace_issubset(Namespace *v1, const Namespace *v2) {
    size_t n, ln;
    bool ret = true;
    if (v1->len == 0) return ret;
    ln = v1->len; v1->len = 0;
    for (n = 0; n <= v1->mask; n++) {
        const Label *p2, *p = v1->data[n];
        if (p == NULL) continue;
        if (p->defpass == pass || (p->constant && (!fixeddig || p->defpass == pass - 1))) {
            p2 = namespace_lookup(v2, p);
            if (p2 == NULL) {
                ret = false;
                break;
            }
            if (!p->v.obj->same(&p->v, &p2->v)) {
                ret = false;
                break;
            }
        }
    }
    v1->len = ln;
    return ret;
}

static FAST_CALL bool same(const Obj *o1, const Obj *o2) {
    Namespace *v1 = (Namespace *)o1, *v2 = (Namespace *)o2;
    if (o2->obj != NAMESPACE_OBJ) return false;
    return namespace_issubset(v1, v2) && namespace_issubset(v2, v1);
}

static MUST_CHECK Obj *repr(Obj *o1, linepos_t epoint, size_t maxsize) {
    const Namespace *v1 = (const Namespace *)o1;
    size_t i = 0, j, ln = 13, chars = 13;
    Obj **vals;
    Str *str;
    Tuple *tuple = NULL;
    uint8_t *s;

    if (epoint == NULL) return NULL;
    if (v1->len != 0) {
        size_t n;
        ln = v1->len;
        chars = ln + 12;
        if (chars < 12) return NULL; /* overflow */
        if (chars > maxsize) return NULL;
        tuple = new_tuple(ln);
        vals = tuple->data;
        ln = chars;
        for (n = 0; n <= v1->mask; n++) {
            Label *p = v1->data[n];
            Obj *v;
            if (p == NULL) continue;
            if (p->defpass != pass && !(p->constant && (!fixeddig || p->defpass == pass - 1))) {
                ln--;
                chars--;
                continue;
            }
            v = p->v.obj->repr(&p->v, epoint, maxsize - chars);
            if (v == NULL || v->obj != STR_OBJ) goto error;
            str = (Str *)v;
            ln += str->len;
            if (ln < str->len) goto error2; /* overflow */
            chars += str->chars;
            if (chars > maxsize) {
            error2:
                val_destroy(v);
                v = NULL;
            error:
                tuple->len = i;
                val_destroy(&tuple->v);
                return v;
            }
            vals[i++] = v;
        }
        tuple->len = i;
        if (i == 0) { ln++; chars++; }
    }
    str = new_str2(ln);
    if (str == NULL) {
        if (tuple != NULL) val_destroy(&tuple->v);
        return NULL;
    }
    str->chars = chars;
    s = str->data;
    memcpy(s, "namespace({", 11);
    s += 11;
    for (j = 0; j < i; j++) {
        Str *str2 = (Str *)vals[j];
        if (j != 0) *s++ = ',';
        if (str2->len != 0) {
            memcpy(s, str2->data, str2->len);
            s += str2->len;
        }
    }
    *s++ = '}';
    *s = ')';
    if (tuple != NULL) val_destroy(&tuple->v);
    return &str->v;
}

MUST_CHECK Obj *namespace_member(oper_t op, Namespace *v1) {
    Obj *o2 = op->v2;
    Error *err;
    Label *l;
    switch (o2->obj->type) {
    case T_IDENT:
        {
            Ident *v2 = (Ident *)o2;
            l = find_label2(&v2->name, v1);
            if (l != NULL) {
                if (diagnostics.case_symbol && str_cmp(&v2->name, &l->name) != 0) err_msg_symbol_case(&v2->name, l, op->epoint2);
                touch_label(l);
                return val_reference(l->value);
            }
            if (!referenceit || (constcreated && pass < max_pass)) {
                return (Obj *)ref_none();
            }
            err = new_error(ERROR___NOT_DEFINED, op->epoint2);
            err->u.notdef.names = ref_namespace(v1);
            err->u.notdef.ident = (Obj *)ref_ident(v2);
            err->u.notdef.down = false;
            return &err->v;
        }
    case T_ANONIDENT:
        {
            Anonident *v2 = (Anonident *)o2;
            l = find_anonlabel2(v2->count, v1);
            if (l != NULL) {
                touch_label(l);
                return val_reference(l->value);
            }
            if (!referenceit || (constcreated && pass < max_pass)) {
                return (Obj *)ref_none();
            }
            err = new_error(ERROR___NOT_DEFINED, op->epoint2);
            err->u.notdef.names = ref_namespace(v1);
            err->u.notdef.ident = (Obj *)ref_anonident(v2);
            err->u.notdef.down = false;
            return &err->v;
        }
    case T_TUPLE:
    case T_LIST: return o2->obj->rcalc2(op);
    case T_ERROR:
    case T_NONE: return val_reference(o2);
    default: return obj_oper_error(op);
    }
}

MALLOC Namespace *new_namespace(const struct file_list_s *file_list, linepos_t epoint) {
    Namespace *val = (Namespace *)val_alloc(NAMESPACE_OBJ);
    val->data = NULL;
    val->mask = 0;
    val->file_list = file_list;
    val->epoint = *epoint;
    val->len = 0;
    val->backr = 0;
    val->forwr = 0;
    return val;
}

static MUST_CHECK Obj *calc2(oper_t op) {
    if (op->op == &o_MEMBER) {
        return namespace_member(op, (Namespace *)op->v1);
    }
    if (op->v2 == &none_value->v || op->v2->obj == ERROR_OBJ) return val_reference(op->v2);
    return obj_oper_error(op);
}

void namespaceobj_init(void) {
    new_type(&obj, T_NAMESPACE, "namespace", sizeof(Namespace));
    obj.create = create;
    obj.destroy = destroy;
    obj.garbage = garbage;
    obj.same = same;
    obj.repr = repr;
    obj.calc2 = calc2;
}

void namespaceobj_names(void) {
    new_builtin("namespace", val_reference(&NAMESPACE_OBJ->v));
}

Namespace *get_namespace(const Obj *o) {
    switch (o->obj->type) {
    case T_CODE:
        return ((Code *)o)->names;
    case T_UNION:
    case T_STRUCT:
        return ((Struct *)o)->names;
    case T_MFUNC:
        return ((Mfunc *)o)->names;
    case T_NAMESPACE:
        return (Namespace *)o;
    default:
        return NULL;
    }
}
