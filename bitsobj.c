/*
    $Id: bitsobj.c 2215 2020-05-21 20:52:43Z soci $

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
#include "bitsobj.h"
#include <string.h>
#include "math.h"
#include "eval.h"
#include "variables.h"
#include "unicode.h"
#include "encoding.h"
#include "error.h"
#include "arguments.h"

#include "codeobj.h"
#include "boolobj.h"
#include "floatobj.h"
#include "strobj.h"
#include "bytesobj.h"
#include "intobj.h"
#include "listobj.h"
#include "operobj.h"
#include "typeobj.h"
#include "noneobj.h"
#include "errorobj.h"
#include "addressobj.h"

#define SHIFT (8 * sizeof(bdigit_t))

static Type obj;

Type *const BITS_OBJ = &obj;
Bits *null_bits;
Bits *inv_bits;
Bits *bits_value[2];

static MUST_CHECK Obj *bits_from_int(const Int *, linepos_t);

static MUST_CHECK Obj *create(Obj *v1, linepos_t epoint) {
    Obj *err, *ret;
    switch (v1->obj->type) {
    case T_NONE:
    case T_ERROR:
    case T_BITS: return val_reference(v1);
    case T_BOOL: return (Obj *)ref_bits(bits_value[(Bool *)v1 == true_value ? 1 : 0]);
    case T_STR: return bits_from_str((Str *)v1, epoint);
    case T_BYTES: return bits_from_bytes((Bytes *)v1, epoint);
    case T_INT: return bits_from_int((Int *)v1, epoint);
    case T_CODE: return bits_from_code((Code *)v1, epoint);
    case T_ADDRESS: return bits_from_address((Address *)v1, epoint);
    case T_FLOAT:
         err = int_from_float((Float *)v1, epoint);
         if (err->obj != INT_OBJ) return err;
         ret = bits_from_int((Int *)err, epoint);
         val_destroy(err);
         return ret;
    default: break;
    }
    return (Obj *)new_error_conv(v1, BITS_OBJ, epoint);
}

static inline size_t bitslen(const Bits *v1) {
    ssize_t len = v1->len;
    return (len < 0) ? (size_t)~len : (size_t)len;
}

static FAST_CALL NO_INLINE void bits_destroy(Bits *v1) {
    free(v1->data);
}

static FAST_CALL void destroy(Obj *o1) {
    Bits *v1 = (Bits *)o1;
    if (v1->u.val != v1->data) bits_destroy(v1);
}

static MALLOC Bits *new_bits2(size_t len) {
    Bits *v = (Bits *)val_alloc(BITS_OBJ);
    if (len > lenof(v->u.val)) {
        v->u.hash = -1;
        v->data = (len <= SIZE_MAX / sizeof *v->data) ? (bdigit_t *)malloc(len * sizeof *v->data) : NULL;
        if (v->data == NULL) {
            val_destroy(&v->v);
            v = NULL;
        }
    } else {
        v->data = v->u.val;
    }
    return v;
}

static MUST_CHECK Obj *invert(const Bits *v1, linepos_t epoint) {
    size_t sz = bitslen(v1);
    Bits *v = new_bits2(sz);
    if (v == NULL) return (Obj *)new_error_mem(epoint);
    v->bits = v1->bits;
    v->len = ~v1->len;
    if (sz != 0) {
        memcpy(v->data, v1->data, sz * sizeof *v->data);
    } else {
        v->data[0] = 0;
    }
    return &v->v;
}

static MUST_CHECK Obj *normalize(Bits *v, size_t sz, bool neg) {
    bdigit_t *d = v->data;
    while (sz != 0 && d[sz - 1] == 0) sz--;
    if (sz <= lenof(v->u.val) && v->u.val != d) {
        if (sz != 0) {
            memcpy(v->u.val, d, sz * sizeof *d);
        } else {
            v->u.val[0] = 0;
        }
        free(d);
        v->data = v->u.val;
    }
    /*if (sz >= SSIZE_MAX) err_msg_out_of_memory();*/ /* overflow */
    v->len = neg ? (ssize_t)~sz : (ssize_t)sz;
    return &v->v;
}

static MUST_CHECK Obj *negate(Bits *v1, linepos_t epoint) {
    size_t i, sz = bitslen(v1);
    Bits *v;
    bool ext = false;
    if (v1->len == 0) return val_reference(&v1->v);
    if (v1->len < 0) {
        size_t bs = (v1->bits % SHIFT);
        ext = true;
        for (i = 0; i < sz; i++) {
            if (v1->data[i] != (bdigit_t)~0) {
                ext = false;
                break;
            }
        }
        if (i == v1->bits / SHIFT) {
            if (bs == 0) return NULL;
            if (v1->data[i] == ~((~(v1->data[i] >> bs)) << bs)) return NULL;
        }
        v = new_bits2(ext ? sz + 1 : sz);
        if (v == NULL) goto failed;
        if (ext) {
            for (i = 0; i < sz; i++) v->data[i] = 0;
            v->data[i] = 1;
        } else {
            for (i = 0; i < sz; i++) {
                if (v1->data[i] != (bdigit_t)~0) {
                    v->data[i] = v1->data[i] + 1;
                    i++;
                    break;
                }
                v->data[i] = 0;
            }
        }
    } else {
        v = new_bits2(sz);
        if (v == NULL) goto failed;
        for (i = 0; i < sz; i++) {
            if (v1->data[i] != 0) {
                v->data[i] = v1->data[i] - 1;
                i++;
                break;
            }
            v->data[i] = ~(bdigit_t)0;
        }
    }
    for (; i < sz; i++) v->data[i] = v1->data[i];
    v->bits = v1->bits;
    return normalize(v, ext ? sz + 1 : sz, v1->len > 0);
failed:
    return (Obj *)new_error_mem(epoint);
}

static MALLOC Bits *return_bits(bdigit_t c, size_t blen, bool neg) {
    ssize_t l;
    bdigit_t *v;
    Bits *vv = (Bits *)val_alloc(BITS_OBJ);
    vv->data = v = vv->u.val;
    v[0] = c;
    l = (c != 0) ? 1 : 0;
    vv->len = neg ? ~l : l;
    vv->bits = blen;
    return vv;
}

static FAST_CALL NO_INLINE bool bits_same(const Bits *v1, const Bits *v2) {
    return memcmp(v1->data, v2->data, bitslen(v1) * sizeof *v1->data) == 0;
}

static FAST_CALL bool same(const Obj *o1, const Obj *o2) {
    const Bits *v1 = (const Bits *)o1, *v2 = (const Bits *)o2;
    if (o1->obj != o2->obj || ((v1->len ^ v2->len) | (v1->bits ^ v2->bits)) != 0) return false;
    switch (v1->len) {
    case 0: return true;
    case -1:
    case 1: return v1->data[0] == v2->data[0];
    default: return bits_same(v1, v2);
    }
}

static MUST_CHECK Obj *truth(Obj *o1, Truth_types type, linepos_t epoint) {
    const Bits *v1 = (const Bits *)o1;
    size_t i, sz, sz2;
    bdigit_t b, inv;
    switch (type) {
    case TRUTH_ALL:
        if (diagnostics.strict_bool) err_msg_bool(ERROR_____CANT_BOOL, o1, epoint);
        sz = bitslen(v1);
        sz2 = v1->bits / SHIFT;
        if (sz2 > sz) sz2 = sz;
        inv = (v1->len >= 0) ? ~(bdigit_t)0 : 0;
        for (i = 0; i < sz2; i++) {
            if (v1->data[i] != inv) return (Obj *)ref_bool(false_value);
        }
        b = (i >= sz) ? 0 : v1->data[i];
        b ^= inv;
        b <<= SHIFT - v1->bits % SHIFT;
        return truth_reference(b == 0);
    case TRUTH_ANY:
        if (diagnostics.strict_bool) err_msg_bool(ERROR_____CANT_BOOL, o1, epoint);
        return truth_reference(v1->len != 0 && v1->len != ~0);
    default:
        return truth_reference(v1->len != 0);
    }
    return DEFAULT_OBJ->truth(o1, type, epoint);
}

static MUST_CHECK Obj *repr(Obj *o1, linepos_t UNUSED(epoint), size_t maxsize) {
    const Bits *v1 = (const Bits *)o1;
    size_t len, i, len2, sz;
    uint8_t *s;
    bool inv;
    Str *v;

    len2 = v1->bits;
    sz = bitslen(v1);
    inv = (v1->len < 0);
    len = inv ? 2 : 1;
    if ((len2 & 3) != 0) {
        len += len2;
        if (len < len2) return NULL; /* overflow */
        if (len > maxsize) return NULL;
        v = new_str2(len);
        if (v == NULL) return NULL;
        v->chars = len;
        s = v->data;

        if (inv) *s++ = '~';
        *s++ = '%';
        for (i = len2; (i--) != 0;) {
            size_t j = i / SHIFT;
            *s++ = (j >= sz) ? 0x30 : 0x30 | ((v1->data[j] >> (i % SHIFT)) & 1);
        }
        return &v->v;
    }
    len2 /= 4;
    len += len2;
    if (len < len2) return NULL; /* overflow */
    if (len > maxsize) return NULL;
    v = new_str2(len);
    if (v == NULL) return NULL;
    v->chars = len;
    s = v->data;

    if (inv) *s++ = '~';
    *s++ = '$';
    for (i = len2; (i--) != 0;) {
        size_t j = i / (2 * sizeof *v1->data);
        *s++ = (j >= sz) ? 0x30 : (uint8_t)"0123456789abcdef"[(v1->data[j] >> ((i & (2 * (sizeof *v1->data) - 1)) * 4)) & 15];
    }
    return &v->v;
}

static MUST_CHECK Error *hash(Obj *o1, int *hs, linepos_t UNUSED(epoint)) {
    Bits *v1 = (Bits *)o1;
    size_t l;
    int h;

    switch (v1->len) {
    case ~1: *hs = (~v1->data[0]) & ((~0U) >> 1); return NULL;
    case ~0: *hs = ((~0U) >> 1); return NULL;
    case 0: *hs = 0; return NULL;
    case 1: *hs = v1->data[0] & ((~0U) >> 1); return NULL;
    default: break;
    }
    if (v1->data != v1->u.val && v1->u.hash >= 0) {
        *hs = v1->u.hash;
        return NULL;
    }
    if (v1->len < 0) {
        l = (size_t)~v1->len;
        h = -1;
        while ((l--) != 0) {
            h -= v1->data[l];
        }
    } else {
        l = (size_t)v1->len;
        h = 0;
        while ((l--) != 0) {
            h += v1->data[l];
        }
    }
    h &= ((~0U) >> 1);
    if (v1->data != v1->u.val) v1->u.hash = h;
    *hs = h;
    return NULL;
}

static bool uvalx(Obj *o1, uval_t *uv, unsigned int bits) {
    Bits *v1 = (Bits *)o1;
    switch (v1->len) {
    case ~1:
        *uv = ~v1->data[0];
        if (v1->bits > bits) break;
        return true;
    case ~0:
        *uv = ~(uval_t)0;
        if (v1->bits > bits) break;
        return true;
    case 0:
        *uv = 0;
        return true;
    case 1:
        *uv = v1->data[0];
        if (bits < SHIFT && (*uv >> bits) != 0) break;
        return true;
    default:
        break;
    }
    return false;
}

static MUST_CHECK Error *ival(Obj *o1, ival_t *iv, unsigned int bits, linepos_t epoint) {
    Error *v;
    if (uvalx(o1, (uval_t *)iv, bits)) return NULL;
    v = new_error(ERROR_____CANT_IVAL, epoint);
    v->u.intconv.bits = bits;
    v->u.intconv.val = val_reference(o1);
    return v;
}

static MUST_CHECK Error *uval(Obj *o1, uval_t *uv, unsigned int bits, linepos_t epoint) {
    Error *v;
    if (uvalx(o1, uv, bits)) return NULL;
    v = new_error(ERROR_____CANT_UVAL, epoint);
    v->u.intconv.bits = bits;
    v->u.intconv.val = val_reference(o1);
    return v;
}

static MUST_CHECK Error *uval2(Obj *o1, uval_t *uv, unsigned int bits, linepos_t epoint) {
    if (((Bits *)o1)->len < 0) {
        Error *v = new_error(ERROR______NOT_UVAL, epoint);
        v->u.intconv.val = val_reference(o1);
        return v;
    }
    return uval(o1, uv, bits, epoint);
}

MUST_CHECK Obj *float_from_bits(const Bits *v1, linepos_t epoint) {
    double d;
    size_t i, len1;
    switch (v1->len) {
    case ~1: d = -1.0 - v1->data[0]; break;
    case ~0: d = -1.0; break;
    case 0: d = 0.0; break;
    case 1: d = v1->data[0]; break;
    default:
        d = ((v1->len < 0) ? 1.0 : 0.0) + v1->data[0];
        len1 = bitslen(v1);
        for (i = 1; i < len1; i++) {
            d += ldexp(v1->data[i], (int)(i * SHIFT));
        }
        if (v1->len < 0) d = -d;
        return float_from_double(d, epoint);
    }
    return (Obj *)new_float(d);
}

static MUST_CHECK Obj *sign(Obj *o1, linepos_t UNUSED(epoint)) {
    Bits *v1 = (Bits *)o1;
    ssize_t len = v1->len;
    if (len < 0) return (Obj *)ref_int(minus1_value);
    return (Obj *)ref_int(int_value[(len > 0) ? 1 : 0]);
}

static MUST_CHECK Obj *function(oper_t op) {
    Bits *v1 = (Bits *)op->v2;
    Obj *tmp, *ret;
    op->v2 = tmp = int_from_bits(v1, op->epoint2);
    op->inplace = tmp->refcount == 1 ? tmp : NULL;
    ret = tmp->obj->function(op);
    val_destroy(tmp);
    return ret;
}

static MUST_CHECK Obj *len(oper_t op) {
    Bits *v1 = (Bits *)op->v2;
    return (Obj *)int_from_size(v1->bits);
}

MUST_CHECK Bits *ibits_from_bool(bool i) {
    return return_bits(i ? 1 : 0, 1, true);
}

MUST_CHECK Bits *bits_from_bools(bool i, bool j) {
    return return_bits((i ? 2U : 0U) | (j ? 1U : 0U), 2, false);
}

static MUST_CHECK Bits *bits_from_u24(uint32_t i) {
    return return_bits(i & 0xffffff, 24, false);
}

MUST_CHECK Bits *bits_from_uval(uval_t i, unsigned int bits) {
    return return_bits(i, bits, false);
}

MUST_CHECK Obj *bits_from_hexstr(const uint8_t *s, size_t *ln, linepos_t epoint) {
    size_t i, j, k, sz;
    int bits;
    bdigit_t *d, uv;
    Bits *v;

    i = k = 0;
    if (s[0] != '_') {
        uv = 0;
        for (;;k++) {
            uint8_t c2, c = s[k] ^ 0x30;
            if (c < 10) {
                uv = (uv << 4) | c;
                continue;
            }
            c2 = (c | 0x20) - 0x71;
            if (c2 < 6) {
                uv = (uv << 4) | (c2 + 10U);
                continue;
            }
            if (c != ('_' ^ 0x30)) break;
            i++;
        }
        while (k != 0 && s[k - 1] == '_') {
            k--;
            i--;
        }
    }
    *ln = k;
    i = k - i;

    if (i <= 2 * sizeof *d) {
        return (i == 0) ? val_reference(&null_bits->v) : (Obj *)return_bits(uv, i * 4, false);
    }

    if (i > SIZE_MAX / 4) goto failed; /* overflow */

    sz = i / (2 * sizeof *d);
    if ((i % (2 * sizeof *d)) != 0) sz++;
    v = new_bits2(sz);
    if (v == NULL) goto failed;
    v->bits = i * 4;
    d = v->data;

    uv = j = 0; bits = 0;
    while ((k--) != 0) {
        uint8_t c = s[k] ^ 0x30;
        if (c < 10) uv |= (bdigit_t)c << bits;
        else if (c == ('_' ^ 0x30)) continue;
        else uv |= (((bdigit_t)c & 7) + 9) << bits;
        if (bits == SHIFT - 4) {
            d[j++] = uv;
            bits = 0; uv = 0;
        } else bits += 4;
    }
    if (bits != 0) d[j] = uv;

    return normalize(v, sz, false);
failed:
    return (Obj *)new_error_mem(epoint);
}

MUST_CHECK Obj *bits_from_binstr(const uint8_t *s, size_t *ln, linepos_t epoint) {
    size_t i, j, k, sz;
    int bits;
    bdigit_t *d, uv;
    Bits *v;

    i = k = 0;
    if (s[0] != '_') {
        uv = 0;
        for (;;k++) {
            uint8_t c = s[k] ^ 0x30;
            if (c < 2) {
                uv = (uv << 1) | c;
                continue;
            }
            if (c != ('_' ^ 0x30)) break;
            i++;
        }
        while (k != 0 && s[k - 1] == '_') {
            k--;
            i--;
        }
    }
    *ln = k;
    i = k - i;

    if (i <= 8 * sizeof *d) {
        return (i == 0) ? val_reference(&null_bits->v) : (Obj *)return_bits(uv, i, false);
    }

    sz = i / SHIFT;
    if ((i % SHIFT) != 0) sz++;
    v = new_bits2(sz);
    if (v == NULL) return (Obj *)new_error_mem(epoint);
    v->bits = i;
    d = v->data;

    uv = j = 0; bits = 0;
    while ((k--) != 0) {
        uint8_t c = s[k];
        if (c == 0x31) uv |= 1U << bits;
        else if (c == '_') continue;
        if (bits == SHIFT - 1) {
            d[j++] = uv;
            bits = 0; uv = 0;
        } else bits++;
    }
    if (bits != 0) d[j] = uv;

    return normalize(v, sz, false);
}

MUST_CHECK Obj *bits_from_str(const Str *v1, linepos_t epoint) {
    int ch;
    Bits *v;
    unsigned int bits;
    size_t j, sz, osz;
    bdigit_t *d, uv;

    if (actual_encoding == NULL) {
        if (v1->chars == 1) {
            uchar_t ch2 = v1->data[0];
            if ((ch2 & 0x80) != 0) utf8in(v1->data, &ch2);
            return (Obj *)bits_from_u24(ch2);
        }
        return (Obj *)new_error((v1->chars == 0) ? ERROR__EMPTY_STRING : ERROR__NOT_ONE_CHAR, epoint);
    }

    if (v1->len == 0) {
        return (Obj *)ref_bits(null_bits);
    }

    if (v1->len <= sizeof v->u.val) sz = lenof(v->u.val);
    else {
        sz = v1->len / sizeof *d;
        if ((v1->len % sizeof *d) != 0) sz++;
    }
    v = new_bits2(sz);
    if (v == NULL) goto failed;
    d = v->data;

    uv = bits = j = 0;
    encode_string_init(v1, epoint);
    while ((ch = encode_string()) != EOF) {
        uv |= (bdigit_t)(ch & 0xff) << bits;
        if (bits == SHIFT - 8) {
            if (j >= sz) {
                if (v->u.val == d) {
                    sz = 16 / sizeof *d;
                    d = (bdigit_t *)malloc(sz * sizeof *d);
                    if (d == NULL) goto failed2;
                    v->data = d;
                    memcpy(d, v->u.val, j * sizeof *d);
                    v->u.hash = -1;
                } else {
                    sz += 1024 / sizeof *d;
                    if (/*sz < 1024 / sizeof *d ||*/ sz > SIZE_MAX / sizeof *d) goto failed2; /* overflow */
                    d = (bdigit_t *)realloc(d, sz * sizeof *d);
                    if (d == NULL) goto failed2;
                    v->data = d;
                }
            }
            d[j++] = uv;
            bits = uv = 0;
        } else bits += 8;
    }
    if (bits != 0) {
        if (j >= sz) {
            sz++;
            if (v->u.val == d) {
                d = (bdigit_t *)malloc(sz * sizeof *d);
                if (d == NULL) goto failed2;
                v->data = d;
                memcpy(d, v->u.val, j * sizeof *d);
                v->u.hash = -1;
            } else {
                if (/*sz < 1 ||*/ sz > SIZE_MAX / sizeof *d) goto failed2; /* overflow */
                d = (bdigit_t *)realloc(d, sz * sizeof *d);
                if (d == NULL) goto failed2;
                v->data = d;
            }
        }
        d[j] = uv;
        osz = j + 1;
    } else osz = j;

    while (osz != 0 && d[osz - 1] == 0) osz--;
    if (v->u.val != d) {
        if (osz <= lenof(v->u.val)) {
            if (osz != 0) {
                memcpy(v->u.val, d, osz * sizeof *d);
            } else {
                v->u.val[0] = 0;
            }
            free(d);
            v->data = v->u.val;
        } else if (osz < sz) {
            bdigit_t *d2 = (bdigit_t *)realloc(d, osz * sizeof *d);
            v->data = (d2 != NULL) ? d2 : d;
        }
    }
    v->len = (ssize_t)osz;
    v->bits = j * SHIFT + bits;
    if (/*v->bits < bits ||*/ j > SIZE_MAX / SHIFT /*|| osz > SSIZE_MAX*/) goto failed2; /* overflow */
    return &v->v;
failed2:
    val_destroy(&v->v);
failed:
    return (Obj *)new_error_mem(epoint);
}

MUST_CHECK Obj *bits_from_bytes(const Bytes *v1, linepos_t epoint) {
    int bits;
    size_t i, j, sz, len1;
    bdigit_t *d, uv;
    Bits *v;
    bool inv = (v1->len < 0);

    len1 = inv ? (size_t)~v1->len : (size_t)v1->len;
    if (len1 == 0) {
        return val_reference(inv ? &inv_bits->v : &null_bits->v);
    }

    if (len1 > SIZE_MAX / 8) goto failed; /* overflow */

    sz = len1 / sizeof *d;
    if ((len1 % sizeof *d) != 0) sz++;
    v = new_bits2(sz);
    if (v == NULL) goto failed;
    v->bits = len1 * 8;
    d = v->data;

    uv = j = i = 0; bits = 0;
    while (len1 > i) {
        uv |= (bdigit_t)v1->data[i++] << bits;
        if (bits == SHIFT - 8) {
            d[j++] = uv;
            bits = 0; uv = 0;
        } else bits += 8;
    }
    if (bits != 0) d[j] = uv;

    return normalize(v, sz, inv);
failed:
    return (Obj *)new_error_mem(epoint);
}

static MUST_CHECK Obj *bits_from_int(const Int *v1, linepos_t epoint) {
    bool inv;
    size_t i, sz, bits;
    bdigit_t *d, d2;
    const digit_t *b;
    Bits *v;

    if (v1->len == 0) return (Obj *)ref_bits(null_bits);
    if (v1->len == -1 && v1->data[0] == 1) return (Obj *)ref_bits(inv_bits);

    inv = (v1->len < 0);
    sz = inv ? (size_t)-v1->len : (size_t)v1->len;
    v = new_bits2(sz);
    if (v == NULL) return (Obj *)new_error_mem(epoint);
    d = v->data;

    b = v1->data;
    if (inv) {
        digit_t c = 0;
        for (i = 0; c == 0 && i < sz; i++) {
            d[i] = (c = b[i]) - 1;
        }
        for (; i < sz; i++) {
            d[i] = b[i];
        }
    } else memcpy(d, b, sz * sizeof *d);

    d2 = d[sz - 1];
    for (bits = 0; d2 != 0; bits++) d2 >>= 1;

    v->bits = (sz - 1) * SHIFT + bits;

    return normalize(v, sz, inv);
}

static ssize_t icmp(oper_t op) {
    const Bits *vv1 = (Bits *)op->v1, *vv2 = (Bits *)op->v2;
    ssize_t i;
    size_t j;
    bdigit_t a, b;
    i = vv1->len - vv2->len;
    if (i != 0) return i;
    j = bitslen(vv1);
    while ((j--) != 0) {
        a = vv1->data[j]; b = vv2->data[j];
        if (a > b) return (vv1->len < 0) ? -1 : 1;
        if (a < b) return (vv1->len < 0) ? 1 : -1;
    }
    return 0;
}

static inline unsigned int ldigit(const Bits *v1) {
    ssize_t ln = v1->len;
    if (ln > 0) return v1->data[0];
    if (ln == 0) return 0;
    ln = ~ln;
    if (ln == 0) return ~(bdigit_t)0;
    return ~v1->data[0];
}

static MUST_CHECK Obj *calc1(oper_t op) {
    Bits *v1 = (Bits *)op->v1;
    Obj *tmp, *v;
    switch (op->op->op) {
    case O_BANK:
    case O_HIGHER:
    case O_LOWER:
    case O_HWORD:
    case O_WORD:
    case O_BSWORD:
        return bytes_calc1(op->op->op, ldigit(v1));
    case O_INV:
        if (op->inplace != &v1->v) return invert(v1, op->epoint3);
        v1->len = ~v1->len;
        if (v1->data != v1->u.val) v1->u.hash = -1;
        return val_reference(&v1->v);
    case O_POS: return val_reference(&v1->v);
    case O_NEG:
        v = negate(v1, op->epoint3);
        if (v != NULL) return v;
        /* fall through */
    case O_STRING:
        tmp = int_from_bits(v1, op->epoint);
        op->v1 = tmp;
        op->inplace = (tmp->refcount == 1) ? tmp : NULL;
        v = tmp->obj->calc1(op);
        val_destroy(tmp);
        return v;
    case O_LNOT:
        if (diagnostics.strict_bool) err_msg_bool_oper(op);
        return truth_reference(v1->len == 0);
    default:
        break;
    }
    return obj_oper_error(op);
}

static MALLOC Bits *bits_inplace(oper_t op) {
    Bits *vv;
    if (op->inplace == op->v1) {
        return (Bits *)val_reference(op->v1);
    }
    vv = (Bits *)val_alloc(BITS_OBJ);
    vv->data = vv->u.val;
    return vv;
}

static inline MUST_CHECK Obj *and_(oper_t op) {
    Bits *vv1 = (Bits *)op->v1, *vv2 = (Bits *)op->v2;
    size_t blen1, blen2, blen;
    size_t i, len1, len2, sz;
    bool neg1, neg2;
    bdigit_t *v1, *v2, *v;
    Bits *vv;
    blen1 = vv1->bits;
    blen2 = vv2->bits;
    if (blen1 < blen2) {
        blen = (vv1->len < 0) ? blen2 : blen1;
    } else {
        blen = (vv2->len < 0) ? blen1 : blen2;
    }
    len1 = bitslen(vv1);
    len2 = bitslen(vv2);

    if (len1 <= 1 && len2 <= 1) {
        bdigit_t c;
        vv = bits_inplace(op);
        vv->bits = blen;
        neg1 = (vv1->len < 0); neg2 = (vv2->len < 0);
        if (neg1) {
            if (neg2) {
                c = vv1->u.val[0] | vv2->u.val[0];
            } else {
                c = ~vv1->u.val[0] & vv2->u.val[0];
            }
        } else {
            c = vv1->u.val[0] & (neg2 ? ~vv2->u.val[0] : vv2->u.val[0]);
        }
        vv->u.val[0] = c;
        vv->len = (c != 0) ? 1 : 0;
        if (neg1 && neg2) vv->len = ~vv->len;
        return &vv->v;
    }
    if (len1 < len2) {
        Bits *tmp = vv1; vv1 = vv2; vv2 = tmp;
        i = len1; len1 = len2; len2 = i;
    }
    v1 = vv1->data; v2 = vv2->data;
    neg1 = (vv1->len < 0); neg2 = (vv2->len < 0);

    sz = neg2 ? len1 : len2;
    if (op->inplace == &vv1->v) {
        vv = ref_bits(vv1);
        if (vv->data != vv->u.val) vv->u.hash = -1;
    } else if (op->inplace == &vv2->v && sz <= len2) {
        vv = ref_bits(vv2);
        if (vv->data != vv->u.val) vv->u.hash = -1;
    } else {
        vv = new_bits2(sz);
        if (vv == NULL) return (Obj *)new_error_mem(op->epoint3);
    }
    vv->bits = blen;
    v = vv->data;

    if (sz == 0) {
        v[0] = 0;
    } else if (neg1) {
        if (neg2) {
            for (i = 0; i < len2; i++) v[i] = v1[i] | v2[i];
            for (; i < len1; i++) v[i] = v1[i];
        } else {
            for (i = 0; i < len2; i++) v[i] = ~v1[i] & v2[i];
        }
    } else {
        if (neg2) {
            for (i = 0; i < len2; i++) v[i] = v1[i] & ~v2[i];
            for (; i < len1; i++) v[i] = v1[i];
        } else {
            for (i = 0; i < len2; i++) v[i] = v1[i] & v2[i];
        }
    }

    return normalize(vv, sz, neg1 && neg2);
}

static inline MUST_CHECK Obj *or_(oper_t op) {
    Bits *vv1 = (Bits *)op->v1, *vv2 = (Bits *)op->v2;
    size_t blen1, blen2, blen;
    size_t i, len1, len2, sz;
    bool neg1, neg2;
    bdigit_t *v1, *v2, *v;
    Bits *vv;
    blen1 = vv1->bits;
    blen2 = vv2->bits;
    if (blen1 < blen2) {
        blen = (vv1->len < 0) ? blen1 : blen2;
    } else {
        blen = (vv2->len < 0) ? blen2 : blen1;
    }
    len1 = bitslen(vv1);
    len2 = bitslen(vv2);

    if (len1 <= 1 && len2 <= 1) {
        bdigit_t c;
        vv = bits_inplace(op);
        vv->bits = blen;
        neg1 = (vv1->len < 0); neg2 = (vv2->len < 0);
        if (neg1) {
            c = vv1->u.val[0] & (neg2 ? vv2->u.val[0] : ~vv2->u.val[0]);
        } else {
            if (neg2) {
                c = ~vv1->u.val[0] & vv2->u.val[0];
            } else {
                c = vv1->u.val[0] | vv2->u.val[0];
            }
        }
        vv->u.val[0] = c;
        vv->len = (c != 0) ? 1 : 0;
        if (neg1 || neg2) vv->len = ~vv->len;
        return &vv->v;
    }
    if (len1 < len2) {
        Bits *tmp = vv1; vv1 = vv2; vv2 = tmp;
        i = len1; len1 = len2; len2 = i;
    }
    v1 = vv1->data; v2 = vv2->data;
    neg1 = (vv1->len < 0); neg2 = (vv2->len < 0);

    sz = neg2 ? len2 : len1;
    if (op->inplace == &vv1->v) {
        vv = ref_bits(vv1);
        if (vv->data != vv->u.val) vv->u.hash = -1;
    } else if (op->inplace == &vv2->v && sz <= len2) {
        vv = ref_bits(vv2);
        if (vv->data != vv->u.val) vv->u.hash = -1;
    } else {
        vv = new_bits2(sz);
        if (vv == NULL) return (Obj *)new_error_mem(op->epoint3);
    }
    vv->bits = blen;
    v = vv->data;

    if (sz == 0) {
        v[0] = 0;
    } else if (neg1) {
        if (neg2) {
            for (i = 0; i < len2; i++) v[i] = v1[i] & v2[i];
        } else {
            for (i = 0; i < len2; i++) v[i] = v1[i] & ~v2[i];
            for (; i < len1; i++) v[i] = v1[i];
        }
    } else {
        if (neg2) {
            for (i = 0; i < len2; i++) v[i] = ~v1[i] & v2[i];
        } else {
            for (i = 0; i < len2; i++) v[i] = v1[i] | v2[i];
            for (; i < len1; i++) v[i] = v1[i];
        }
    }

    return normalize(vv, sz, neg1 || neg2);
}

static inline MUST_CHECK Obj *xor_(oper_t op) {
    Bits *vv1 = (Bits *)op->v1, *vv2 = (Bits *)op->v2;
    size_t blen1, blen2, blen;
    size_t i, len1, len2, sz;
    bool neg1, neg2;
    bdigit_t *v1, *v2, *v;
    Bits *vv;
    blen1 = vv1->bits;
    blen2 = vv2->bits;
    blen = (blen1 < blen2) ? blen2 : blen1;
    len1 = bitslen(vv1);
    len2 = bitslen(vv2);

    if (len1 <= 1 && len2 <= 1) {
        bdigit_t c;
        vv = bits_inplace(op);
        vv->bits = blen;
        c = vv1->u.val[0] ^ vv2->u.val[0];
        vv->u.val[0] = c;
        vv->len = (c != 0) ? 1 : 0;
        neg1 = (vv1->len < 0); neg2 = (vv2->len < 0);
        if (neg1 != neg2) vv->len = ~vv->len;
        return &vv->v;
    }
    if (len1 < len2) {
        Bits *tmp = vv1; vv1 = vv2; vv2 = tmp;
        i = len1; len1 = len2; len2 = i;
    }
    v1 = vv1->data; v2 = vv2->data;
    neg1 = (vv1->len < 0); neg2 = (vv2->len < 0);

    sz = len1;
    if (op->inplace == &vv1->v) {
        vv = ref_bits(vv1);
        if (vv->data != vv->u.val) vv->u.hash = -1;
    } else if (op->inplace == &vv2->v && sz <= len2) {
        vv = ref_bits(vv2);
        if (vv->data != vv->u.val) vv->u.hash = -1;
    } else {
        vv = new_bits2(sz);
        if (vv == NULL) return (Obj *)new_error_mem(op->epoint3);
    }
    vv->bits = blen;
    v = vv->data;

    for (i = 0; i < len2; i++) v[i] = v1[i] ^ v2[i];
    for (; i < len1; i++) v[i] = v1[i];

    return normalize(vv, sz, neg1 != neg2);
}

static inline MUST_CHECK Obj *concat(oper_t op) {
    Bits *vv1 = (Bits *)op->v1, *vv2 = (Bits *)op->v2;
    size_t blen;
    size_t sz, bits, i, j, rbits, l;
    bdigit_t *v1, *v, uv;
    bdigit_t inv;
    Bits *vv;

    if (vv1->bits == 0) {
        return (Obj *)ref_bits(vv2);
    }
    if (vv2->bits == 0) {
        return (Obj *)ref_bits(vv1);
    }
    blen = vv1->bits + vv2->bits;
    if (blen < vv2->bits) goto failed; /* overflow */
    sz = blen / SHIFT;
    if ((blen % SHIFT) != 0) sz++;
    i = vv2->bits / SHIFT + bitslen(vv1);
    if ((vv2->bits % SHIFT) != 0) i++;
    if (i < sz) sz = i;
    vv = new_bits2(sz);
    if (vv == NULL) goto failed;
    v = vv->data;
    inv = ((vv2->len ^ vv1->len) < 0) ? ~(bdigit_t)0 : 0;

    v1 = vv2->data;
    rbits = vv2->bits / SHIFT;
    bits = vv2->bits & (SHIFT - 1);
    l = bitslen(vv2);
    for (i = 0; i < rbits && i < l; i++) v[i] = v1[i] ^ inv;
    for (; i < rbits; i++) v[i] = inv;
    if (i < l) uv = v1[i] ^ inv; else uv = inv;
    if (inv != 0) uv &= (1U << bits) - 1;

    v1 = vv1->data;

    l = bitslen(vv1);
    if (bits != 0) {
        for (j = 0; j < l; j++) {
            v[i++] = uv | (v1[j] << bits);
            uv = v1[j] >> (SHIFT - bits);
        }
        if (i < sz) v[i++] = uv;
    } else {
        for (j = 0; j < l; j++) v[i++] = v1[j];
    }

    vv->bits = blen;
    return normalize(vv, i, (vv1->len < 0));
failed:
    return (Obj *)new_error_mem(op->epoint3);
}

static MUST_CHECK Obj *lshift(oper_t op, uval_t s) {
    Bits *vv1 = (Bits *)op->v1;
    size_t i, sz, bits, len1;
    unsigned int bit, word;
    bdigit_t *v1, *v, *v2;
    Bits *vv;

    sz = word = s / SHIFT;
    bit = s % SHIFT;
    if (bit != 0) sz++;
    bits = vv1->bits + s;
    if (bits < s) goto failed; /* overflow */
    len1 = bitslen(vv1);
    sz += len1;
    if (sz < len1) goto failed; /* overflow */
    if (op->inplace == &vv1->v && sz <= lenof(vv->u.val)) {
        vv = ref_bits(vv1);
    } else {
        vv = new_bits2(sz);
        if (vv == NULL) goto failed;
    }
    vv->bits = bits;
    v = vv->data;
    v1 = vv1->data;
    v2 = v + word;
    if (bit != 0) {
        bdigit_t d = 0;
        for (i = len1; i != 0;) {
            bdigit_t d2 = v1[--i];
            v2[i + 1] = d | (d2 >> (SHIFT - bit));
            d = d2 << bit;
        }
        if (vv1->len < 0) d |= ((bdigit_t)1 << bit) - 1;
        v2[0] = d;
    } else if (len1 != 0) memmove(v2, v1, len1 * sizeof *v2);
    if (word != 0) memset(v, (vv1->len < 0) ? ~0 : 0, word * sizeof *v);

    return normalize(vv, sz, (vv1->len < 0));
failed:
    return (Obj *)new_error_mem(op->epoint3);
}

static MUST_CHECK Obj *rshift(oper_t op, uval_t s) {
    Bits *vv1 = (Bits *)op->v1;
    size_t i, sz, bits, len1;
    unsigned int bit, word;
    Bits *vv;

    word = s / SHIFT;
    bit = s % SHIFT;
    bits = vv1->bits;
    if (bits <= s) {
        return (Obj *)ref_bits((vv1->len < 0) ? inv_bits : null_bits);
    }
    bits -= s;
    len1 = bitslen(vv1);
    sz = (len1 > word) ? (len1 - word) : 0;
    if (op->inplace == &vv1->v) {
        vv = ref_bits(vv1);
        if (vv->data != vv->u.val) vv->u.hash = -1;
    } else {
        vv = new_bits2(sz);
        if (vv == NULL) return (Obj *)new_error_mem(op->epoint3);
    }
    vv->bits = bits;
    if (sz == 0) {
        vv->data[0] = 0;
    } else {
        bdigit_t *v = vv->data;
        bdigit_t *v1 = vv1->data + word;
        if (bit != 0) {
            bdigit_t d = v1[0] >> bit;
            for (i = 0; i < sz - 1; i++) {
                bdigit_t d2 = v1[i + 1];
                v[i] = d | (d2 << (SHIFT - bit));
                d = d2 >> bit;
            }
            v[i] = d;
        } else memmove(v, v1, sz * sizeof *v);
    }
    return normalize(vv, sz, (vv1->len < 0));
}

static MUST_CHECK Obj *calc2_bits(oper_t op) {
    ssize_t val;
    switch (op->op->op) {
    case O_CMP:
        val = icmp(op);
        if (val < 0) return (Obj *)ref_int(minus1_value);
        return (Obj *)ref_int(int_value[(val > 0) ? 1 : 0]);
    case O_EQ: return truth_reference(icmp(op) == 0);
    case O_NE: return truth_reference(icmp(op) != 0);
    case O_MIN:
    case O_LT: return truth_reference(icmp(op) < 0);
    case O_LE: return truth_reference(icmp(op) <= 0);
    case O_MAX:
    case O_GT: return truth_reference(icmp(op) > 0);
    case O_GE: return truth_reference(icmp(op) >= 0);
    case O_AND: return and_(op);
    case O_OR: return or_(op);
    case O_XOR: return xor_(op);
    case O_CONCAT: return concat(op);
    case O_IN: return obj_oper_error(op); /* TODO */
    default: return NULL;
    }
}

static inline MUST_CHECK Obj *repeat(oper_t op) {
    Bits *vv1 = (Bits *)op->v1, *vv;
    bdigit_t *v, *v1;
    bdigit_t uv;
    size_t sz, i, j, rbits, abits, bits, l;
    size_t blen = vv1->bits;
    uval_t rep;
    Error *err;

    err = op->v2->obj->uval(op->v2, &rep, 8 * sizeof rep, op->epoint2);
    if (err != NULL) return &err->v;

    if (rep == 0 || blen == 0) {
        return (Obj *)ref_bits((vv1->len < 0) ? inv_bits : null_bits);
    }
    if (rep == 1) {
        return (Obj *)ref_bits(vv1);
    }

    if (blen > SIZE_MAX / rep) goto failed; /* overflow */
    blen *= rep;
    sz = blen / SHIFT;
    if ((blen % SHIFT) != 0) sz++;

    vv = new_bits2(sz);
    if (vv == NULL) goto failed;
    v = vv->data;
    v1 = vv1->data;

    i = 0;
    bits = 0;
    uv = 0;
    rbits = vv1->bits / SHIFT;
    abits = vv1->bits % SHIFT;
    l = bitslen(vv1);
    for (; rep != 0; rep--) {
        if (bits != 0) {
            for (j = 0; j < rbits && j < l; j++) {
                v[i++] = uv | (v1[j] << bits);
                uv = (v1[j] >> (SHIFT - bits));
            }
            if (j < l) uv |= v1[j] << bits;
            if (j < rbits) {
                v[i++] = uv; uv = 0; j++;
                for (; j < rbits; j++) v[i++] = 0;
            }
            bits += abits;
            if (bits >= SHIFT) {
                bits -= SHIFT;
                v[i++] = uv;
                if (bits != 0 && j < l) uv = v1[j] >> (abits - bits);
                else uv = 0;
            }
        } else {
            for (j = 0; j < rbits && j < l; j++) v[i++] = v1[j];
            for (; j < rbits; j++) v[i++] = 0;
            uv = (j < l) ? v1[j] : 0;
            bits = abits;
        }
    }
    if (i < sz) v[i] = uv;

    vv->bits = blen;
    return normalize(vv, sz, (vv1->len < 0));
failed:
    return (Obj *)new_error_mem(op->epoint3);
}

static MUST_CHECK Obj *slice(oper_t op, size_t indx) {
    size_t offs2, ln, sz;
    size_t i, o;
    Bits *vv, *vv1 = (Bits *)op->v1;
    Obj *o2 = op->v2;
    bdigit_t *v;
    bdigit_t uv;
    bdigit_t inv = (vv1->len < 0) ? ~(bdigit_t)0 : 0;
    int bits;
    Error *err;
    Funcargs *args = (Funcargs *)o2;
    linepos_t epoint2;

    if (args->len < 1 || args->len > indx + 1) {
        return (Obj *)new_error_argnum(args->len, 1, indx + 1, op->epoint2);
    }

    o2 = args->val[indx].val;
    epoint2 = &args->val[indx].epoint;

    ln = vv1->bits;

    if (o2->obj->iterable) {
        struct iter_s iter;
        iter.data = o2; o2->obj->getiter(&iter);

        if (iter.len == 0) {
            iter_destroy(&iter);
            return (Obj *)ref_bits(null_bits);
        }
        sz = (iter.len + SHIFT - 1) / SHIFT;

        vv = new_bits2(sz);
        if (vv == NULL) {
            iter_destroy(&iter);
            goto failed;
        }
        v = vv->data;

        uv = inv;
        bits = 0; sz = 0;
        for (i = 0; i < iter.len && (o2 = iter.next(&iter)) != NULL; i++) {
            err = indexoffs(o2, ln, &offs2, epoint2);
            if (err != NULL) {
                val_destroy(&vv->v);
                iter_destroy(&iter);
                return &err->v;
            }
            o = offs2 / SHIFT;
            if (o < bitslen(vv1) && ((vv1->data[o] >> (offs2 % SHIFT)) & 1) != 0) {
                uv ^= 1U << bits;
            }
            bits++;
            if (bits == SHIFT) {
                v[sz++] = uv;
                uv = inv;
                bits = 0;
            }
        }
        iter_destroy(&iter);
        if (bits != 0) v[sz++] = uv & ((1U << bits) - 1);

        vv->bits = i;
        return normalize(vv, sz, false);
    }
    if (o2->obj == COLONLIST_OBJ) {
        uval_t length;
        ival_t offs, end, step;
        size_t bo, wo, bl, wl, wl2, l;
        bdigit_t *v1;

        err = (Error *)sliceparams((Colonlist *)o2, ln, &length, &offs, &end, &step, epoint2);
        if (err != NULL) return &err->v;

        if (length == 0) {
            return (Obj *)ref_bits(null_bits);
        }
        if (step == 1) {
            if (length == vv1->bits && inv == 0) {
                return (Obj *)ref_bits(vv1); /* original bits */
            }

            bo = (uval_t)offs % SHIFT;
            wo = (uval_t)offs / SHIFT;
            bl = length % SHIFT;
            wl = length / SHIFT;

            sz = (bl > 0) ? (wl + 1) : wl;
            vv = new_bits2(sz);
            if (vv == NULL) goto failed;
            v = vv->data;

            l = bitslen(vv1);
            v1 = vv1->data + wo;
            wl2 = (wo > l) ? 0 : (l - wo);
            if (bo != 0) {
                for (sz = 0; sz < wl; sz++) {
                    v[sz] = inv ^ (sz < wl2 ? (v1[sz] >> bo) : 0) ^ ((sz + 1 < wl2) ? (v1[sz + 1] << (SHIFT - bo)) : 0);
                }
                if (bl != 0) {
                    v[sz] = sz < wl2 ? (v1[sz] >> bo) : 0;
                    if (bl > (SHIFT - bo)) v[sz] |= v1[sz + 1] << (SHIFT - bo);
                    v[sz] ^= inv;
                }
            } else {
                for (sz = 0; sz < wl2 && sz < wl; sz++) v[sz] = v1[sz] ^ inv;
                for (; sz < wl; sz++) v[sz] = inv;
                if (bl != 0) v[sz] = inv ^ ((sz < wl2) ? v1[sz] : 0);
            }
            if (bl != 0) v[sz++] &= ((1U << bl) - 1);
        } else {
            sz = length / SHIFT;
            if ((length % SHIFT) != 0) sz++;
            vv = new_bits2(sz);
            if (vv == NULL) goto failed;
            v = vv->data;

            uv = inv;
            sz = 0; bits = 0;
            l = bitslen(vv1);
            for (i = 0; i < length; i++) {
                wo = (uval_t)offs / SHIFT;
                if (wo < l && ((vv1->data[wo] >> ((uval_t)offs % SHIFT)) & 1) != 0) {
                    uv ^= 1U << bits;
                }
                bits++;
                if (bits == SHIFT) {
                    v[sz++] = uv;
                    uv = inv;
                    bits = 0;
                }
                offs += step;
            }
            if (bits != 0) v[sz++] = uv & ((1U << bits) - 1);
        }

        vv->bits = length;
        return normalize(vv, sz, false);
    }
    err = indexoffs(o2, ln, &offs2, epoint2);
    if (err != NULL) return &err->v;

    uv = inv;
    o = offs2 / SHIFT;
    if (o < bitslen(vv1) && ((vv1->data[o] >> (offs2 % SHIFT)) & 1) != 0) {
        uv ^= 1;
    }
    return (Obj *)ref_bits(bits_value[uv & 1]);
failed:
    return (Obj *)new_error_mem(op->epoint3);
}

static MUST_CHECK Obj *calc2(oper_t op) {
    Bits *v1 = (Bits *)op->v1;
    Obj *o2 = op->v2;
    Obj *tmp, *result;
    Error *err;
    ival_t shift;

    if (op->op == &o_X) {
        if (o2 == &none_value->v || o2->obj == ERROR_OBJ) return val_reference(o2);
        return repeat(op);
    }
    if (op->op == &o_LAND) {
        if (diagnostics.strict_bool) err_msg_bool_oper(op);
        return val_reference((v1->len != 0) ? o2 : &v1->v);
    }
    if (op->op == &o_LOR) {
        if (diagnostics.strict_bool) err_msg_bool_oper(op);
        return val_reference((v1->len != 0) ? &v1->v : o2);
    }
    switch (o2->obj->type) {
    case T_BOOL:
        if (diagnostics.strict_bool) err_msg_bool_oper(op);
        op->v2 = tmp = (Obj *)ref_bits(bits_value[(Bool *)o2 == true_value ? 1 : 0]);
        if (op->inplace != NULL && op->inplace->refcount != 1) op->inplace = NULL;
        result = calc2(op);
        val_destroy(tmp);
        return result;
    case T_BITS:
        result = calc2_bits(op);
        if (result != NULL) return result;
        /* fall through */
    case T_INT:
        switch (op->op->op) {
        case O_LSHIFT:
            err = o2->obj->ival(o2, &shift, 8 * sizeof shift, op->epoint2);
            if (err != NULL) return &err->v;
            if (shift == 0) return val_reference(&v1->v);
            return (shift < 0) ? rshift(op, (uval_t)-shift) : lshift(op, (uval_t)shift);
        case O_RSHIFT:
            err = o2->obj->ival(o2, &shift, 8 * sizeof shift, op->epoint2);
            if (err != NULL) return &err->v;
            if (shift == 0) return val_reference(&v1->v);
            return (shift < 0) ? lshift(op, (uval_t)-shift) : rshift(op, (uval_t)shift);
        default: break;
        }
        tmp = int_from_bits(v1, op->epoint);
        op->v1 = tmp;
        op->inplace = (tmp->refcount == 1) ? tmp : NULL;
        result = tmp->obj->calc2(op);
        val_destroy(tmp);
        return result;
    default:
        if (op->op != &o_MEMBER) {
            return o2->obj->rcalc2(op);
        }
        if (o2 == &none_value->v || o2->obj == ERROR_OBJ) return val_reference(o2);
    }
    return obj_oper_error(op);
}

static MUST_CHECK Obj *rcalc2(oper_t op) {
    Bits *v2 = (Bits *)op->v2;
    Obj *o1 = op->v1;
    Obj *tmp, *result;

    switch (o1->obj->type) {
    case T_BOOL:
        if (diagnostics.strict_bool) err_msg_bool_oper(op);
        op->v1 = tmp = (Obj *)ref_bits(bits_value[(Bool *)o1 == true_value ? 1 : 0]);
        op->inplace = NULL;
        result = calc2(op);
        val_destroy(tmp);
        return result;
    case T_BITS:
        result = calc2_bits(op);
        if (result != NULL) return result;
        /* fall through */
    case T_INT:
        tmp = int_from_bits(v2, op->epoint2);
        op->v2 = tmp;
        op->inplace = NULL;
        result = o1->obj->calc2(op);
        val_destroy(tmp);
        return result;
    default: break;
    }
    return obj_oper_error(op);
}

void bitsobj_init(void) {
    new_type(&obj, T_BITS, "bits", sizeof(Bits));
    obj.create = create;
    obj.destroy = destroy;
    obj.same = same;
    obj.truth = truth;
    obj.hash = hash;
    obj.repr = repr;
    obj.ival = ival;
    obj.uval = uval;
    obj.uval2 = uval2;
    obj.iaddress = ival;
    obj.uaddress = uval;
    obj.sign = sign;
    obj.function = function;
    obj.len = len;
    obj.calc1 = calc1;
    obj.calc2 = calc2;
    obj.rcalc2 = rcalc2;
    obj.slice = slice;

    null_bits = return_bits(0, 0, false);
    inv_bits = return_bits(0, 0, true);
    bits_value[0] = return_bits(0, 1, false);
    bits_value[1] = return_bits(1, 1, false);
}

void bitsobj_names(void) {
    new_builtin("bits", val_reference(&BITS_OBJ->v));
}

void bitsobj_destroy(void) {
#ifdef DEBUG
    if (null_bits->v.refcount != 1) fprintf(stderr, "bits %" PRIuSIZE "\n", null_bits->v.refcount - 1);
    if (inv_bits->v.refcount != 1) fprintf(stderr, "invbits %" PRIuSIZE "\n", inv_bits->v.refcount - 1);
    if (bits_value[0]->v.refcount != 1) fprintf(stderr, "bit0 %" PRIuSIZE "\n", bits_value[0]->v.refcount - 1);
    if (bits_value[1]->v.refcount != 1) fprintf(stderr, "bit1 %" PRIuSIZE "\n", bits_value[1]->v.refcount - 1);
#endif

    val_destroy(&null_bits->v);
    val_destroy(&inv_bits->v);
    val_destroy(&bits_value[0]->v);
    val_destroy(&bits_value[1]->v);
}
