/*
 * Helpers that need the FPU. They are kept out of swm_client.c / swm_manager.c,
 * which are compiled with -mgeneral-regs-only on arm64: Swoole's coroutine
 * context switch on arm64 (observed with Swoole 5.1.x on macOS) does not
 * preserve the callee-saved FP registers d8-d15, so code that can yield must
 * never keep floating point values in registers. Nothing in this file keeps
 * an FP value live across a call that may yield.
 */
#include "php_swmongo.h"
#include <sys/time.h>
#include <unistd.h>
#include <math.h>
#include "Zend/zend_strtod.h"

int64_t swm_now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t) tv.tv_sec * 1000000 + tv.tv_usec;
}

zend_class_entry *swm_swoole_co_ce(void)
{
    static zend_class_entry *ce = NULL;
    static zend_bool looked = 0;
    if (!looked) {
        zend_string *n = zend_string_init(ZEND_STRL("swoole\\coroutine"), 0);
        ce = zend_lookup_class_ex(n, NULL, ZEND_FETCH_CLASS_NO_AUTOLOAD);
        zend_string_release(n);
        looked = 1;
    }
    return ce;
}

zend_bool swm_in_coroutine(void)
{
    zend_class_entry *ce = swm_swoole_co_ce();
    zval r;
    zend_long cid = -1;
    if (!ce) return 0;
    zend_call_method(NULL, ce, NULL, ZEND_STRL("getcid"), &r, 0, NULL, NULL);
    if (Z_TYPE(r) == IS_LONG) cid = Z_LVAL(r);
    zval_ptr_dtor(&r);
    return cid > 0;
}

void swm_sleep_us(int64_t us)
{
    if (us < 1000) us = 1000;
    if (swm_in_coroutine()) {
        zval a, r;
        ZVAL_DOUBLE(&a, (double) us / 1e6);
        zend_call_method(NULL, swm_swoole_co_ce(), NULL, ZEND_STRL("sleep"), &r, 1, &a, NULL);
        zval_ptr_dtor(&r);
    } else {
        usleep((useconds_t) us);
    }
}

void swm_add_assoc_us_as_ms(zval *arr, const char *key, int64_t us)
{
    add_assoc_double(arr, key, (double) us / 1000.0);
}

int swm_call_function(const char *name, zval *retval, uint32_t argc, zval *argv)
{
    zval fn;
    int rc;
    ZVAL_STRING(&fn, name);
    rc = call_user_function(NULL, NULL, &fn, retval, argc, argv);
    zval_ptr_dtor(&fn);
    if (rc != SUCCESS || EG(exception)) {
        zval_ptr_dtor(retval);
        ZVAL_UNDEF(retval);
        return FAILURE;
    }
    return SUCCESS;
}

/* ---- floating point helpers used by FP-free compilation units ---- */
int64_t swm_dval_to_int64(zval *v)
{
    return (int64_t) Z_DVAL_P(v);
}

void swm_zval_set_double_bits(zval *z, uint64_t bits)
{
    double d;
    memcpy(&d, &bits, sizeof(d));
    ZVAL_DOUBLE(z, d);
}

uint64_t swm_strtod_bits(const char *s, int *ok)
{
    double d;
    uint64_t bits;
    const char *end = NULL;
    *ok = 1;
    if (!strcmp(s, "Infinity")) d = INFINITY;
    else if (!strcmp(s, "-Infinity")) d = -INFINITY;
    else if (!strcmp(s, "NaN")) d = NAN;
    else {
        d = zend_strtod(s, &end);
        if (!end || *end || end == s) *ok = 0;
    }
    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

/* Extended JSON double: relaxed -> plain number when finite, else {"$numberDouble": "..."} */
void swm_append_double(smart_str *s, uint64_t bits, zend_bool relaxed)
{
    double d;
    memcpy(&d, &bits, sizeof(d));
    if (zend_isinf(d) || zend_isnan(d)) {
        smart_str_appends(s, "{ \"$numberDouble\" : \"");
        smart_str_appends(s, zend_isnan(d) ? "NaN" : (d > 0 ? "Infinity" : "-Infinity"));
        smart_str_appends(s, "\" }");
        return;
    }
    if (relaxed) {
        smart_str_append_double(s, d, (int) PG(serialize_precision), true);
        return;
    }
    smart_str_appends(s, "{ \"$numberDouble\" : \"");
    smart_str_append_double(s, d, (int) PG(serialize_precision), true);
    smart_str_appends(s, "\" }");
}
