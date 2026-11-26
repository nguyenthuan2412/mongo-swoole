/*
 * SwooleMongo\BSON: value classes, Document / PackedArray, encoder,
 * typeMap aware decoder and Extended JSON.
 *
 * Compiled with -mgeneral-regs-only on arm64 (see config.m4): floating point
 * values are only ever moved as raw bits here; formatting/parsing of doubles
 * lives in swm_util.c.
 */
#include "php_swoole_mongo.h"
#include "ext/standard/base64.h"
#include <sys/time.h>
#include <unistd.h>

#if PHP_VERSION_ID >= 80200
#include "ext/random/php_random.h"
#else
#include "ext/standard/php_random.h"
#endif

zend_class_entry *swm_ce_Type;
zend_class_entry *swm_ce_Serializable;
zend_class_entry *swm_ce_Unserializable;
zend_class_entry *swm_ce_Persistable;
zend_class_entry *swm_ce_ObjectId;
zend_class_entry *swm_ce_UTCDateTime;
zend_class_entry *swm_ce_Binary;
zend_class_entry *swm_ce_Regex;
zend_class_entry *swm_ce_Timestamp;
zend_class_entry *swm_ce_Int64;
zend_class_entry *swm_ce_Decimal128;
zend_class_entry *swm_ce_MinKey;
zend_class_entry *swm_ce_MaxKey;
zend_class_entry *swm_ce_Javascript;
zend_class_entry *swm_ce_Document;
zend_class_entry *swm_ce_PackedArray;

#define SWM_BSON_MAX_DEPTH 100
#define PROP(o, n) OBJ_PROP_NUM((o), (n))
#define PCLASS "__pclass"

typedef unsigned __int128 swm_u128;

static int encode_document(smart_str *s, zval *doc, zend_bool as_array, int depth, swm_error *err);
static int encode_element(smart_str *s, const char *key, size_t klen, zval *v, int depth, swm_error *err);

/* ------------------------------------------------------------ prop helpers */
static inline void prop_set_str(zend_object *o, int n, zend_string *s)
{
    zval *p = PROP(o, n);
    zval_ptr_dtor(p);
    ZVAL_STR(p, s);
}
static inline void prop_set_long(zend_object *o, int n, zend_long v)
{
    zval *p = PROP(o, n);
    zval_ptr_dtor(p);
    ZVAL_LONG(p, v);
}
static inline void prop_set_zval(zend_object *o, int n, zval *v)
{
    zval *p = PROP(o, n);
    zval_ptr_dtor(p);
    ZVAL_COPY(p, v);
}

/* ------------------------------------------------------------ little endian */
static inline int32_t rd_i32(const char *p)
{
    const uint8_t *b = (const uint8_t *) p;
    return (int32_t) ((uint32_t) b[0] | ((uint32_t) b[1] << 8) | ((uint32_t) b[2] << 16) | ((uint32_t) b[3] << 24));
}
static inline uint32_t rd_u32(const char *p) { return (uint32_t) rd_i32(p); }
static inline int64_t rd_i64(const char *p)
{
    return (int64_t) ((uint64_t) rd_u32(p) | ((uint64_t) rd_u32(p + 4) << 32));
}
static inline void put_i32(smart_str *s, int32_t v)
{
    uint32_t u = (uint32_t) v;
    char b[4] = {(char) u, (char) (u >> 8), (char) (u >> 16), (char) (u >> 24)};
    smart_str_appendl(s, b, 4);
}
static inline void put_i64(smart_str *s, int64_t v)
{
    put_i32(s, (int32_t) (uint32_t) ((uint64_t) v & 0xffffffffu));
    put_i32(s, (int32_t) (uint32_t) ((uint64_t) v >> 32));
}
static inline size_t sstr_len(smart_str *s) { return s->s ? ZSTR_LEN(s->s) : 0; }
static inline void patch_i32(smart_str *s, size_t at, int32_t v)
{
    uint32_t u = (uint32_t) v;
    char *b = ZSTR_VAL(s->s) + at;
    b[0] = (char) u; b[1] = (char) (u >> 8); b[2] = (char) (u >> 16); b[3] = (char) (u >> 24);
}

static const char hexchars[] = "0123456789abcdef";

static void bin2hex(const uint8_t *in, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hexchars[in[i] >> 4];
        out[i * 2 + 1] = hexchars[in[i] & 15];
    }
    out[len * 2] = '\0';
}
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int hex2bin(const char *in, size_t len, uint8_t *out)
{
    if (len % 2) return FAILURE;
    for (size_t i = 0; i < len / 2; i++) {
        int h = hexval(in[i * 2]), l = hexval(in[i * 2 + 1]);
        if (h < 0 || l < 0) return FAILURE;
        out[i] = (uint8_t) (h << 4 | l);
    }
    return SUCCESS;
}

/* ------------------------------------------------------------------ errors */
void swm_error_init(swm_error *e)
{
    e->kind = SWM_ERR_NONE;
    e->code = 0;
    e->msg = NULL;
    ZVAL_UNDEF(&e->reply);
    ZVAL_UNDEF(&e->labels);
}

void swm_error_clear(swm_error *e)
{
    if (e->msg) zend_string_release(e->msg);
    zval_ptr_dtor(&e->reply);
    zval_ptr_dtor(&e->labels);
    swm_error_init(e);
}

void swm_error_set(swm_error *e, swm_err_kind kind, zend_long code, const char *fmt, ...)
{
    va_list ap;
    if (!e) return;
    swm_error_clear(e);
    e->kind = kind;
    e->code = code;
    va_start(ap, fmt);
    e->msg = zend_vstrpprintf(0, fmt, ap);
    va_end(ap);
}

zend_bool swm_error_has_label(swm_error *e, const char *label)
{
    zval *v;
    if (Z_TYPE(e->labels) != IS_ARRAY) return 0;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(e->labels), v) {
        if (Z_TYPE_P(v) == IS_STRING && strcmp(Z_STRVAL_P(v), label) == 0) return 1;
    } ZEND_HASH_FOREACH_END();
    return 0;
}

void swm_error_add_label(swm_error *e, const char *label)
{
    if (swm_error_has_label(e, label)) return;
    if (Z_TYPE(e->labels) != IS_ARRAY) {
        zval_ptr_dtor(&e->labels);
        array_init(&e->labels);
    }
    add_next_index_string(&e->labels, label);
}

void swm_error_to_exception(swm_error *e, zval *out)
{
    zend_class_entry *ce;
    switch (e->kind) {
        case SWM_ERR_ARG: ce = swm_ce_InvalidArgumentException; break;
        case SWM_ERR_LOGIC: ce = swm_ce_LogicException; break;
        case SWM_ERR_BSON: ce = swm_ce_UnexpectedValueException; break;
        case SWM_ERR_NETWORK: ce = swm_ce_ConnectionException; break;
        case SWM_ERR_TIMEOUT:
        case SWM_ERR_SELECTION: ce = swm_ce_ConnectionTimeoutException; break;
        case SWM_ERR_AUTH: ce = swm_ce_AuthenticationException; break;
        case SWM_ERR_RUNTIME_CRYPT: ce = swm_ce_EncryptionException; break;
        case SWM_ERR_SERVER: ce = e->code == 50 ? swm_ce_ExecutionTimeoutException : swm_ce_CommandException; break;
        default: ce = swm_ce_RuntimeException; break;
    }
    object_init_ex(out, ce);
    zend_update_property_string(zend_ce_exception, Z_OBJ_P(out), ZEND_STRL("message"), e->msg ? ZSTR_VAL(e->msg) : "unknown error");
    zend_update_property_long(zend_ce_exception, Z_OBJ_P(out), ZEND_STRL("code"), e->code);
    if (Z_TYPE(e->labels) == IS_ARRAY && instanceof_function(ce, swm_ce_RuntimeException)) {
        zend_update_property(swm_ce_RuntimeException, Z_OBJ_P(out), ZEND_STRL("errorLabels"), &e->labels);
    }
    if (ce == swm_ce_CommandException && Z_TYPE(e->reply) == IS_ARRAY) {
        zval doc;
        swm_array_to_object_doc(&e->reply, &doc);
        zend_update_property(swm_ce_CommandException, Z_OBJ_P(out), ZEND_STRL("resultDocument"), &doc);
        zval_ptr_dtor(&doc);
    }
}

void swm_error_throw(swm_error *e)
{
    zval ex;
    swm_error_to_exception(e, &ex);
    zend_throw_exception_object(&ex);
    swm_error_clear(e);
}

void swm_throw(zend_class_entry *ce, const char *fmt, ...)
{
    va_list ap;
    zend_string *msg;
    va_start(ap, fmt);
    msg = zend_vstrpprintf(0, fmt, ap);
    va_end(ap);
    zend_throw_exception(ce, ZSTR_VAL(msg), 0);
    zend_string_release(msg);
}

/* ================================================================ ObjectId */
static void oid_generate(uint8_t b[12])
{
    static uint8_t rnd[5];
    static uint32_t counter;
    static pid_t pid = 0;
    pid_t cur = getpid();
    uint32_t t, c;
    if (pid != cur) {
        php_random_bytes_silent(rnd, sizeof(rnd));
        php_random_bytes_silent(&counter, sizeof(counter));
        pid = cur;
    }
    t = (uint32_t) time(NULL);
    c = (++counter) & 0xffffff;
    b[0] = t >> 24; b[1] = t >> 16; b[2] = t >> 8; b[3] = t;
    memcpy(b + 4, rnd, 5);
    b[9] = c >> 16; b[10] = c >> 8; b[11] = c;
}

static void new_oid(zval *out, const uint8_t bin[12])
{
    char hex[25];
    bin2hex(bin, 12, hex);
    object_init_ex(out, swm_ce_ObjectId);
    prop_set_str(Z_OBJ_P(out), 0, zend_string_init(hex, 24, 0));
}

void swm_new_objectid(zval *out)
{
    uint8_t b[12];
    oid_generate(b);
    new_oid(out, b);
}

static zend_bool oid_valid(const char *s, size_t len)
{
    if (len != 24) return 0;
    for (size_t i = 0; i < 24; i++) if (hexval(s[i]) < 0) return 0;
    return 1;
}

PHP_METHOD(SwooleMongo_BSON_ObjectId, __construct)
{
    zend_string *id = NULL;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR_OR_NULL(id)
    ZEND_PARSE_PARAMETERS_END();
    if (id) {
        if (!oid_valid(ZSTR_VAL(id), ZSTR_LEN(id))) {
            swm_throw(swm_ce_InvalidArgumentException, "Error parsing ObjectId string: %s", ZSTR_VAL(id));
            RETURN_THROWS();
        }
        prop_set_str(Z_OBJ_P(ZEND_THIS), 0, zend_string_tolower(id));
    } else {
        uint8_t b[12];
        char hex[25];
        oid_generate(b);
        bin2hex(b, 12, hex);
        prop_set_str(Z_OBJ_P(ZEND_THIS), 0, zend_string_init(hex, 24, 0));
    }
}

PHP_METHOD(SwooleMongo_BSON_ObjectId, __toString)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(Z_OBJ_P(ZEND_THIS), 0));
}

PHP_METHOD(SwooleMongo_BSON_ObjectId, getTimestamp)
{
    uint8_t b[4];
    zval *hex = PROP(Z_OBJ_P(ZEND_THIS), 0);
    ZEND_PARSE_PARAMETERS_NONE();
    hex2bin(Z_STRVAL_P(hex), 8, b);
    RETURN_LONG(((zend_long) b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
}

PHP_METHOD(SwooleMongo_BSON_ObjectId, jsonSerialize)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    Z_TRY_ADDREF_P(PROP(Z_OBJ_P(ZEND_THIS), 0));
    add_assoc_zval(return_value, "$oid", PROP(Z_OBJ_P(ZEND_THIS), 0));
}

PHP_METHOD(SwooleMongo_BSON_ObjectId, isValid)
{
    zend_string *s;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(s)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_BOOL(oid_valid(ZSTR_VAL(s), ZSTR_LEN(s)));
}

/* ============================================================= UTCDateTime */
static int64_t now_ms(void)
{
    return swm_now_us() / 1000;
}

static int datetime_to_ms(zval *dt, int64_t *ms)
{
    zval ts, v, fmt;
    zend_call_method_with_0_params(Z_OBJ_P(dt), Z_OBJCE_P(dt), NULL, "gettimestamp", &ts);
    if (EG(exception) || Z_TYPE(ts) != IS_LONG) { zval_ptr_dtor(&ts); return FAILURE; }
    ZVAL_STRING(&fmt, "v");
    zend_call_method_with_1_params(Z_OBJ_P(dt), Z_OBJCE_P(dt), NULL, "format", &v, &fmt);
    zval_ptr_dtor(&fmt);
    if (EG(exception) || Z_TYPE(v) != IS_STRING) { zval_ptr_dtor(&v); return FAILURE; }
    *ms = (int64_t) Z_LVAL(ts) * 1000 + ZEND_STRTOL(Z_STRVAL(v), NULL, 10);
    zval_ptr_dtor(&v);
    return SUCCESS;
}

static void new_utcdatetime(zval *out, int64_t ms)
{
    object_init_ex(out, swm_ce_UTCDateTime);
    prop_set_long(Z_OBJ_P(out), 0, (zend_long) ms);
}

PHP_METHOD(SwooleMongo_BSON_UTCDateTime, __construct)
{
    zval *arg = NULL;
    int64_t ms;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL_OR_NULL(arg)
    ZEND_PARSE_PARAMETERS_END();
    if (!arg) {
        ms = now_ms();
    } else if (Z_TYPE_P(arg) == IS_LONG) {
        ms = Z_LVAL_P(arg);
    } else if (Z_TYPE_P(arg) == IS_DOUBLE) {
        ms = swm_dval_to_int64(arg);
    } else if (Z_TYPE_P(arg) == IS_STRING && is_numeric_string(Z_STRVAL_P(arg), Z_STRLEN_P(arg), NULL, NULL, 0) == IS_LONG) {
        ms = ZEND_STRTOL(Z_STRVAL_P(arg), NULL, 10);
    } else if (Z_TYPE_P(arg) == IS_OBJECT && instanceof_function(Z_OBJCE_P(arg), swm_ce_Int64)) {
        ms = Z_LVAL_P(PROP(Z_OBJ_P(arg), 0));
    } else if (Z_TYPE_P(arg) == IS_OBJECT && instanceof_function(Z_OBJCE_P(arg), php_date_get_interface_ce())) {
        if (datetime_to_ms(arg, &ms) == FAILURE) RETURN_THROWS();
    } else {
        swm_throw(swm_ce_InvalidArgumentException, "Expected integer, Int64 or DateTimeInterface, %s given", zend_zval_type_name(arg));
        RETURN_THROWS();
    }
    prop_set_long(Z_OBJ_P(ZEND_THIS), 0, (zend_long) ms);
}

static void utc_to_datetime(zval *self, const char *klass, zval *return_value)
{
    zend_long ms = Z_LVAL_P(PROP(Z_OBJ_P(self), 0));
    zend_long sec = ms / 1000, rem = ms % 1000;
    zval args[2], fn;
    if (rem < 0) { rem += 1000; sec -= 1; }
    ZVAL_STRING(&args[0], "U.u");
    ZVAL_STR(&args[1], zend_strpprintf(0, ZEND_LONG_FMT ".%06d", sec, (int) rem * 1000));
    ZVAL_STR(&fn, zend_strpprintf(0, "%s::createFromFormat", klass));
    call_user_function(NULL, NULL, &fn, return_value, 2, args);
    zval_ptr_dtor(&fn);
    zval_ptr_dtor(&args[0]);
    zval_ptr_dtor(&args[1]);
}

PHP_METHOD(SwooleMongo_BSON_UTCDateTime, toDateTime)
{
    ZEND_PARSE_PARAMETERS_NONE();
    utc_to_datetime(ZEND_THIS, "DateTime", return_value);
}

PHP_METHOD(SwooleMongo_BSON_UTCDateTime, toDateTimeImmutable)
{
    ZEND_PARSE_PARAMETERS_NONE();
    utc_to_datetime(ZEND_THIS, "DateTimeImmutable", return_value);
}

PHP_METHOD(SwooleMongo_BSON_UTCDateTime, __toString)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STR(zend_long_to_str(Z_LVAL_P(PROP(Z_OBJ_P(ZEND_THIS), 0))));
}

PHP_METHOD(SwooleMongo_BSON_UTCDateTime, jsonSerialize)
{
    zval inner;
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    array_init(&inner);
    add_assoc_str(&inner, "$numberLong", zend_long_to_str(Z_LVAL_P(PROP(Z_OBJ_P(ZEND_THIS), 0))));
    add_assoc_zval(return_value, "$date", &inner);
}

/* ================================================================== Binary */
void swm_new_binary(zval *out, const char *data, size_t len, zend_long subtype)
{
    object_init_ex(out, swm_ce_Binary);
    prop_set_str(Z_OBJ_P(out), 0, zend_string_init(data, len, 0));
    prop_set_long(Z_OBJ_P(out), 1, subtype);
}

zend_string *swm_binary_data(zval *bin)
{
    if (Z_TYPE_P(bin) != IS_OBJECT || Z_OBJCE_P(bin) != swm_ce_Binary) return NULL;
    return Z_STR_P(PROP(Z_OBJ_P(bin), 0));
}

void swm_uuid4(uint8_t out[16])
{
    php_random_bytes_silent(out, 16);
    out[6] = (out[6] & 0x0f) | 0x40;
    out[8] = (out[8] & 0x3f) | 0x80;
}

static int parse_uuid(const char *s, size_t len, uint8_t b[16])
{
    char hex[33];
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '-' || c == '{' || c == '}') continue;
        if (n >= 32) return FAILURE;
        hex[n++] = c;
    }
    if (n != 32) return FAILURE;
    return hex2bin(hex, 32, b);
}

PHP_METHOD(SwooleMongo_BSON_Binary, __construct)
{
    zend_string *data;
    zend_long type = 0;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(data)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(type)
    ZEND_PARSE_PARAMETERS_END();
    if (type < 0 || type > 255) {
        swm_throw(swm_ce_InvalidArgumentException, "Expected type to be an unsigned 8-bit integer, " ZEND_LONG_FMT " given", type);
        RETURN_THROWS();
    }
    if ((type == 3 || type == 4) && ZSTR_LEN(data) != 16) {
        swm_throw(swm_ce_InvalidArgumentException, "Expected UUID length to be 16 bytes, %zu given", ZSTR_LEN(data));
        RETURN_THROWS();
    }
    prop_set_str(Z_OBJ_P(ZEND_THIS), 0, zend_string_copy(data));
    prop_set_long(Z_OBJ_P(ZEND_THIS), 1, type);
}

PHP_METHOD(SwooleMongo_BSON_Binary, getData)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(Z_OBJ_P(ZEND_THIS), 0));
}

PHP_METHOD(SwooleMongo_BSON_Binary, getType)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(Z_OBJ_P(ZEND_THIS), 1));
}

PHP_METHOD(SwooleMongo_BSON_Binary, fromUuid)
{
    zend_string *s = NULL;
    uint8_t b[16];
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR_OR_NULL(s)
    ZEND_PARSE_PARAMETERS_END();
    if (!s) {
        swm_uuid4(b);
    } else if (parse_uuid(ZSTR_VAL(s), ZSTR_LEN(s), b) == FAILURE) {
        swm_throw(swm_ce_InvalidArgumentException, "Invalid UUID string \"%s\"", ZSTR_VAL(s));
        RETURN_THROWS();
    }
    swm_new_binary(return_value, (char *) b, 16, 4);
}

PHP_METHOD(SwooleMongo_BSON_Binary, toUuid)
{
    zend_string *d = Z_STR_P(PROP(Z_OBJ_P(ZEND_THIS), 0));
    char hex[33];
    ZEND_PARSE_PARAMETERS_NONE();
    if (ZSTR_LEN(d) != 16) {
        swm_throw(swm_ce_LogicException, "Binary data is not a 16 byte UUID");
        RETURN_THROWS();
    }
    bin2hex((uint8_t *) ZSTR_VAL(d), 16, hex);
    RETURN_STR(zend_strpprintf(0, "%.8s-%.4s-%.4s-%.4s-%.12s", hex, hex + 8, hex + 12, hex + 16, hex + 20));
}

PHP_METHOD(SwooleMongo_BSON_Binary, jsonSerialize)
{
    zval inner;
    zend_string *d = Z_STR_P(PROP(Z_OBJ_P(ZEND_THIS), 0));
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    array_init(&inner);
    add_assoc_str(&inner, "base64", php_base64_encode((unsigned char *) ZSTR_VAL(d), ZSTR_LEN(d)));
    add_assoc_str(&inner, "subType", zend_strpprintf(0, "%02x", (int) Z_LVAL_P(PROP(Z_OBJ_P(ZEND_THIS), 1))));
    add_assoc_zval(return_value, "$binary", &inner);
}

/* =================================================================== Regex */
static zend_string *sort_flags(const char *f, size_t len)
{
    zend_string *s = zend_string_init(f, len, 0);
    char *p = ZSTR_VAL(s);
    for (size_t i = 1; i < len; i++) {
        char c = p[i];
        size_t j = i;
        while (j > 0 && p[j - 1] > c) { p[j] = p[j - 1]; j--; }
        p[j] = c;
    }
    return s;
}

static void new_regex(zval *out, const char *pat, size_t plen, const char *flags, size_t flen)
{
    object_init_ex(out, swm_ce_Regex);
    prop_set_str(Z_OBJ_P(out), 0, zend_string_init(pat, plen, 0));
    prop_set_str(Z_OBJ_P(out), 1, sort_flags(flags, flen));
}

PHP_METHOD(SwooleMongo_BSON_Regex, __construct)
{
    zend_string *pat, *flags = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(pat)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR(flags)
    ZEND_PARSE_PARAMETERS_END();
    if (memchr(ZSTR_VAL(pat), 0, ZSTR_LEN(pat)) || (flags && memchr(ZSTR_VAL(flags), 0, ZSTR_LEN(flags)))) {
        swm_throw(swm_ce_InvalidArgumentException, "Pattern and flags cannot contain null bytes");
        RETURN_THROWS();
    }
    prop_set_str(Z_OBJ_P(ZEND_THIS), 0, zend_string_copy(pat));
    prop_set_str(Z_OBJ_P(ZEND_THIS), 1, flags ? sort_flags(ZSTR_VAL(flags), ZSTR_LEN(flags)) : ZSTR_EMPTY_ALLOC());
}

PHP_METHOD(SwooleMongo_BSON_Regex, getPattern)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(Z_OBJ_P(ZEND_THIS), 0));
}
PHP_METHOD(SwooleMongo_BSON_Regex, getFlags)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(Z_OBJ_P(ZEND_THIS), 1));
}
PHP_METHOD(SwooleMongo_BSON_Regex, __toString)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STR(zend_strpprintf(0, "/%s/%s", Z_STRVAL_P(PROP(Z_OBJ_P(ZEND_THIS), 0)), Z_STRVAL_P(PROP(Z_OBJ_P(ZEND_THIS), 1))));
}
PHP_METHOD(SwooleMongo_BSON_Regex, jsonSerialize)
{
    zval inner;
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    array_init(&inner);
    Z_TRY_ADDREF_P(PROP(Z_OBJ_P(ZEND_THIS), 0));
    add_assoc_zval(&inner, "pattern", PROP(Z_OBJ_P(ZEND_THIS), 0));
    Z_TRY_ADDREF_P(PROP(Z_OBJ_P(ZEND_THIS), 1));
    add_assoc_zval(&inner, "options", PROP(Z_OBJ_P(ZEND_THIS), 1));
    add_assoc_zval(return_value, "$regularExpression", &inner);
}

/* =============================================================== Timestamp */
void swm_new_timestamp(zval *out, uint32_t inc, uint32_t ts)
{
    object_init_ex(out, swm_ce_Timestamp);
    prop_set_long(Z_OBJ_P(out), 0, (zend_long) inc);
    prop_set_long(Z_OBJ_P(out), 1, (zend_long) ts);
}

int swm_timestamp_compare(zval *a, zval *b)
{
    zend_long ta = Z_LVAL_P(PROP(Z_OBJ_P(a), 1)), tb = Z_LVAL_P(PROP(Z_OBJ_P(b), 1));
    zend_long ia = Z_LVAL_P(PROP(Z_OBJ_P(a), 0)), ib = Z_LVAL_P(PROP(Z_OBJ_P(b), 0));
    if (ta != tb) return ta < tb ? -1 : 1;
    if (ia != ib) return ia < ib ? -1 : 1;
    return 0;
}

PHP_METHOD(SwooleMongo_BSON_Timestamp, __construct)
{
    zend_long inc, ts;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(inc)
        Z_PARAM_LONG(ts)
    ZEND_PARSE_PARAMETERS_END();
    if (inc < 0 || inc > 0xffffffffLL || ts < 0 || ts > 0xffffffffLL) {
        swm_throw(swm_ce_InvalidArgumentException, "Expected increment and timestamp to be unsigned 32-bit integers");
        RETURN_THROWS();
    }
    prop_set_long(Z_OBJ_P(ZEND_THIS), 0, inc);
    prop_set_long(Z_OBJ_P(ZEND_THIS), 1, ts);
}
PHP_METHOD(SwooleMongo_BSON_Timestamp, getIncrement)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(Z_OBJ_P(ZEND_THIS), 0));
}
PHP_METHOD(SwooleMongo_BSON_Timestamp, getTimestamp)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(Z_OBJ_P(ZEND_THIS), 1));
}
PHP_METHOD(SwooleMongo_BSON_Timestamp, __toString)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STR(zend_strpprintf(0, "[" ZEND_LONG_FMT ":" ZEND_LONG_FMT "]",
        Z_LVAL_P(PROP(Z_OBJ_P(ZEND_THIS), 0)), Z_LVAL_P(PROP(Z_OBJ_P(ZEND_THIS), 1))));
}
PHP_METHOD(SwooleMongo_BSON_Timestamp, jsonSerialize)
{
    zval inner;
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    array_init(&inner);
    add_assoc_long(&inner, "t", Z_LVAL_P(PROP(Z_OBJ_P(ZEND_THIS), 1)));
    add_assoc_long(&inner, "i", Z_LVAL_P(PROP(Z_OBJ_P(ZEND_THIS), 0)));
    add_assoc_zval(return_value, "$timestamp", &inner);
}

/* =================================================================== Int64 */
void swm_new_int64(zval *out, int64_t v)
{
    object_init_ex(out, swm_ce_Int64);
    prop_set_long(Z_OBJ_P(out), 0, (zend_long) v);
}

int64_t swm_zval_to_int64(zval *v, zend_bool *ok)
{
    *ok = 1;
    ZVAL_DEREF(v);
    if (Z_TYPE_P(v) == IS_LONG) return Z_LVAL_P(v);
    if (Z_TYPE_P(v) == IS_DOUBLE) return swm_dval_to_int64(v);
    if (Z_TYPE_P(v) == IS_OBJECT && Z_OBJCE_P(v) == swm_ce_Int64) return Z_LVAL_P(PROP(Z_OBJ_P(v), 0));
    *ok = 0;
    return 0;
}

PHP_METHOD(SwooleMongo_BSON_Int64, __construct)
{
    zval *v;
    zend_long lv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(v)
    ZEND_PARSE_PARAMETERS_END();
    if (Z_TYPE_P(v) == IS_LONG) {
        lv = Z_LVAL_P(v);
    } else if (Z_TYPE_P(v) == IS_STRING && is_numeric_string(Z_STRVAL_P(v), Z_STRLEN_P(v), &lv, NULL, 0) == IS_LONG) {
        /* lv set */
    } else {
        swm_throw(swm_ce_InvalidArgumentException, "Expected value to be an integer or a numeric string");
        RETURN_THROWS();
    }
    prop_set_long(Z_OBJ_P(ZEND_THIS), 0, lv);
}
PHP_METHOD(SwooleMongo_BSON_Int64, __toString)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STR(zend_long_to_str(Z_LVAL_P(PROP(Z_OBJ_P(ZEND_THIS), 0))));
}
PHP_METHOD(SwooleMongo_BSON_Int64, jsonSerialize)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    add_assoc_str(return_value, "$numberLong", zend_long_to_str(Z_LVAL_P(PROP(Z_OBJ_P(ZEND_THIS), 0))));
}

/* ============================================================== Decimal128 */
#define DEC_EXP_MAX 6111
#define DEC_EXP_MIN (-6176)
#define DEC_BIAS 6176

static zend_string *dec128_to_string(uint64_t lo, uint64_t hi)
{
    int neg = (int) (hi >> 63);
    uint64_t comb = (hi >> 58) & 0x1f;
    int32_t biased;
    swm_u128 coef;
    char digits[64], rev[64];
    int nd = 0, exponent, adjusted;
    smart_str s = {0};

    if ((comb >> 3) == 3) {
        if (comb == 0x1e) return zend_string_init(neg ? "-Infinity" : "Infinity", neg ? 9 : 8, 0);
        if (comb == 0x1f) return zend_string_init("NaN", 3, 0);
        biased = (int32_t) ((hi >> 47) & 0x3fff);
        coef = 0;
    } else {
        swm_u128 max = 1;
        biased = (int32_t) ((hi >> 49) & 0x3fff);
        coef = ((swm_u128) (hi & 0x1ffffffffffffULL) << 64) | lo;
        for (int i = 0; i < 34; i++) max *= 10;
        if (coef >= max) coef = 0;
    }
    exponent = biased - DEC_BIAS;

    if (coef == 0) {
        digits[nd++] = '0';
    } else {
        int r = 0;
        while (coef > 0) { rev[r++] = (char) ('0' + (int) (coef % 10)); coef /= 10; }
        while (r > 0) digits[nd++] = rev[--r];
    }
    digits[nd] = '\0';
    adjusted = exponent + nd - 1;

    if (neg) smart_str_appendc(&s, '-');
    if (exponent <= 0 && adjusted >= -6) {
        if (exponent == 0) {
            smart_str_appendl(&s, digits, nd);
        } else {
            int point = nd + exponent;
            if (point > 0) {
                smart_str_appendl(&s, digits, point);
                smart_str_appendc(&s, '.');
                smart_str_appendl(&s, digits + point, nd - point);
            } else {
                smart_str_appendl(&s, "0.", 2);
                for (int i = 0; i < -point; i++) smart_str_appendc(&s, '0');
                smart_str_appendl(&s, digits, nd);
            }
        }
    } else {
        smart_str_appendc(&s, digits[0]);
        if (nd > 1) {
            smart_str_appendc(&s, '.');
            smart_str_appendl(&s, digits + 1, nd - 1);
        }
        smart_str_appendc(&s, 'E');
        if (adjusted >= 0) smart_str_appendc(&s, '+');
        smart_str_append_long(&s, adjusted);
    }
    smart_str_0(&s);
    return s.s;
}

static int dec128_from_string(const char *str, size_t len, uint64_t *lo, uint64_t *hi)
{
    const char *p = str, *end = str + len;
    int neg = 0, seen_dot = 0, seen_digit = 0, nfrac = 0;
    char *buf;
    size_t n = 0;
    int64_t exp = 0, e;
    char coef_digits[48];
    int nd, start, zero;
    swm_u128 coef = 0;

    if (p < end && (*p == '+' || *p == '-')) { neg = (*p == '-'); p++; }
    if ((end - p == 3 && strncasecmp(p, "inf", 3) == 0) || (end - p == 8 && strncasecmp(p, "infinity", 8) == 0)) {
        *hi = ((uint64_t) neg << 63) | 0x7800000000000000ULL;
        *lo = 0;
        return SUCCESS;
    }
    if (end - p == 3 && strncasecmp(p, "nan", 3) == 0) {
        *hi = 0x7c00000000000000ULL;
        *lo = 0;
        return SUCCESS;
    }
    buf = emalloc(len + 1);
    for (; p < end; p++) {
        if (*p >= '0' && *p <= '9') {
            if (seen_dot) nfrac++;
            buf[n++] = *p;
            seen_digit = 1;
        } else if (*p == '.' && !seen_dot) {
            seen_dot = 1;
        } else {
            break;
        }
    }
    if (!seen_digit) { efree(buf); return FAILURE; }
    if (p < end && (*p == 'e' || *p == 'E')) {
        int eneg = 0, edig = 0;
        p++;
        if (p < end && (*p == '+' || *p == '-')) { eneg = (*p == '-'); p++; }
        for (; p < end && *p >= '0' && *p <= '9'; p++) {
            if (exp < 100000000) exp = exp * 10 + (*p - '0');
            edig++;
        }
        if (!edig) { efree(buf); return FAILURE; }
        if (eneg) exp = -exp;
    }
    if (p != end) { efree(buf); return FAILURE; }

    start = 0;
    while ((size_t) start < n - 1 && buf[start] == '0') start++;
    nd = (int) (n - start);
    e = exp - nfrac;
    zero = 1;
    for (int i = start; i < (int) n; i++) if (buf[i] != '0') { zero = 0; break; }

    if (zero) {
        nd = 1;
        coef_digits[0] = '0';
        if (e > DEC_EXP_MAX) e = DEC_EXP_MAX;
        if (e < DEC_EXP_MIN) e = DEC_EXP_MIN;
    } else {
        while (nd > 34) {
            if (buf[start + nd - 1] != '0') { efree(buf); return FAILURE; }
            nd--; e++;
        }
        memcpy(coef_digits, buf + start, nd);
        while (e > DEC_EXP_MAX) {
            if (nd >= 34) { efree(buf); return FAILURE; }
            coef_digits[nd++] = '0';
            e--;
        }
        while (e < DEC_EXP_MIN) {
            if (coef_digits[nd - 1] != '0' || nd == 1) { efree(buf); return FAILURE; }
            nd--; e++;
        }
    }
    efree(buf);
    for (int i = 0; i < nd; i++) coef = coef * 10 + (swm_u128) (coef_digits[i] - '0');
    *hi = ((uint64_t) neg << 63) | ((uint64_t) (e + DEC_BIAS) << 49) | ((uint64_t) (coef >> 64) & 0x1ffffffffffffULL);
    *lo = (uint64_t) coef;
    return SUCCESS;
}

static void new_decimal(zval *out, uint64_t lo, uint64_t hi)
{
    object_init_ex(out, swm_ce_Decimal128);
    prop_set_str(Z_OBJ_P(out), 0, dec128_to_string(lo, hi));
}

PHP_METHOD(SwooleMongo_BSON_Decimal128, __construct)
{
    zend_string *s;
    uint64_t lo, hi;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(s)
    ZEND_PARSE_PARAMETERS_END();
    if (dec128_from_string(ZSTR_VAL(s), ZSTR_LEN(s), &lo, &hi) == FAILURE) {
        swm_throw(swm_ce_InvalidArgumentException, "Error parsing Decimal128 string: %s", ZSTR_VAL(s));
        RETURN_THROWS();
    }
    prop_set_str(Z_OBJ_P(ZEND_THIS), 0, dec128_to_string(lo, hi));
}
PHP_METHOD(SwooleMongo_BSON_Decimal128, __toString)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(Z_OBJ_P(ZEND_THIS), 0));
}
PHP_METHOD(SwooleMongo_BSON_Decimal128, jsonSerialize)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    Z_TRY_ADDREF_P(PROP(Z_OBJ_P(ZEND_THIS), 0));
    add_assoc_zval(return_value, "$numberDecimal", PROP(Z_OBJ_P(ZEND_THIS), 0));
}

/* ========================================================= MinKey/MaxKey */
PHP_METHOD(SwooleMongo_BSON_MinKey, jsonSerialize)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    add_assoc_long(return_value, "$minKey", 1);
}
PHP_METHOD(SwooleMongo_BSON_MaxKey, jsonSerialize)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    add_assoc_long(return_value, "$maxKey", 1);
}

/* ============================================================== Javascript */
PHP_METHOD(SwooleMongo_BSON_Javascript, __construct)
{
    zend_string *code;
    zval *scope = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(code)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL_OR_NULL(scope)
    ZEND_PARSE_PARAMETERS_END();
    if (scope && Z_TYPE_P(scope) != IS_ARRAY && Z_TYPE_P(scope) != IS_OBJECT) {
        swm_throw(swm_ce_InvalidArgumentException, "Expected scope to be array or object, %s given", zend_zval_type_name(scope));
        RETURN_THROWS();
    }
    prop_set_str(Z_OBJ_P(ZEND_THIS), 0, zend_string_copy(code));
    if (scope) prop_set_zval(Z_OBJ_P(ZEND_THIS), 1, scope);
}
PHP_METHOD(SwooleMongo_BSON_Javascript, getCode)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(Z_OBJ_P(ZEND_THIS), 0));
}
PHP_METHOD(SwooleMongo_BSON_Javascript, getScope)
{
    zval *scope = PROP(Z_OBJ_P(ZEND_THIS), 1);
    ZEND_PARSE_PARAMETERS_NONE();
    if (Z_TYPE_P(scope) == IS_ARRAY) {
        swm_array_to_object_doc(scope, return_value);
        return;
    }
    RETURN_COPY(scope);
}
PHP_METHOD(SwooleMongo_BSON_Javascript, __toString)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(Z_OBJ_P(ZEND_THIS), 0));
}
PHP_METHOD(SwooleMongo_BSON_Javascript, jsonSerialize)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    Z_TRY_ADDREF_P(PROP(Z_OBJ_P(ZEND_THIS), 0));
    add_assoc_zval(return_value, "$code", PROP(Z_OBJ_P(ZEND_THIS), 0));
    if (Z_TYPE_P(PROP(Z_OBJ_P(ZEND_THIS), 1)) != IS_NULL) {
        Z_TRY_ADDREF_P(PROP(Z_OBJ_P(ZEND_THIS), 1));
        add_assoc_zval(return_value, "$scope", PROP(Z_OBJ_P(ZEND_THIS), 1));
    }
}

/* ================================================================ ENCODER */
static int put_cstring(smart_str *s, const char *str, size_t len, const char *what, swm_error *err)
{
    if (memchr(str, 0, len)) {
        swm_error_set(err, SWM_ERR_BSON, 0, "%s cannot contain null bytes", what);
        return FAILURE;
    }
    smart_str_appendl(s, str, len);
    smart_str_appendc(s, '\0');
    return SUCCESS;
}

static void put_string(smart_str *s, const char *str, size_t len)
{
    put_i32(s, (int32_t) (len + 1));
    smart_str_appendl(s, str, len);
    smart_str_appendc(s, '\0');
}

/* Public properties of an object as a new array */
static void object_public_props(zval *obj, zval *out)
{
    HashTable *props = zend_get_properties_for(obj, ZEND_PROP_PURPOSE_JSON);
    zend_string *k;
    zend_ulong idx;
    zval *v;
    array_init(out);
    if (!props) return;
    ZEND_HASH_FOREACH_KEY_VAL_IND(props, idx, k, v) {
        if (k && ZSTR_LEN(k) && ZSTR_VAL(k)[0] == '\0') continue;
        Z_TRY_ADDREF_P(v);
        if (k) zend_hash_update(Z_ARRVAL_P(out), k, v); else zend_hash_index_update(Z_ARRVAL_P(out), idx, v);
    } ZEND_HASH_FOREACH_END();
    zend_release_properties(props);
}

/* Resolves Persistable / Serializable / DateTimeInterface / enums.
 * Returns 0 (untouched), 1 (replaced by *tmp), 2 (replaced, must be a document), -1 error. */
static int normalize_value(zval *v, zval *tmp, swm_error *err)
{
    zend_class_entry *ce;
    if (Z_TYPE_P(v) != IS_OBJECT) return 0;
    ce = Z_OBJCE_P(v);
    if (ce == swm_ce_Document || ce == swm_ce_PackedArray) return 0;
    if (instanceof_function(ce, swm_ce_Serializable)) {
        zval r;
        zend_call_method_with_0_params(Z_OBJ_P(v), ce, NULL, "bsonserialize", &r);
        if (EG(exception)) { zval_ptr_dtor(&r); return -1; }
        if (Z_TYPE(r) != IS_ARRAY && Z_TYPE(r) != IS_OBJECT) {
            zval_ptr_dtor(&r);
            swm_error_set(err, SWM_ERR_BSON, 0, "Expected %s::bsonSerialize() to return an array or stdClass, %s given",
                          ZSTR_VAL(ce->name), zend_zval_type_name(&r));
            return -1;
        }
        if (instanceof_function(ce, swm_ce_Persistable)) {
            zval props, pc;
            zend_string *k;
            zend_ulong idx;
            zval *pv;
            if (Z_TYPE(r) == IS_OBJECT) object_public_props(&r, &props); else ZVAL_COPY(&props, &r);
            zval_ptr_dtor(&r);
            array_init(tmp);
            swm_new_binary(&pc, ZSTR_VAL(ce->name), ZSTR_LEN(ce->name), 0x80);
            add_assoc_zval(tmp, PCLASS, &pc);
            ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL(props), idx, k, pv) {
                if (k && zend_string_equals_literal(k, PCLASS)) continue;
                Z_TRY_ADDREF_P(pv);
                if (k) zend_hash_update(Z_ARRVAL_P(tmp), k, pv); else zend_hash_index_update(Z_ARRVAL_P(tmp), idx, pv);
            } ZEND_HASH_FOREACH_END();
            zval_ptr_dtor(&props);
            return 2;
        }
        ZVAL_COPY_VALUE(tmp, &r);
        return Z_TYPE_P(tmp) == IS_OBJECT ? 2 : 1;
    }
    if (instanceof_function(ce, swm_ce_Type)) return 0;
    if (instanceof_function(ce, php_date_get_interface_ce())) {
        int64_t ms;
        if (datetime_to_ms(v, &ms) == FAILURE) return -1;
        new_utcdatetime(tmp, ms);
        return 1;
    }
    if (ce->ce_flags & ZEND_ACC_ENUM) {
        if (ce->enum_backing_type == IS_UNDEF) {
            swm_error_set(err, SWM_ERR_BSON, 0, "Non-backed enum %s cannot be serialized", ZSTR_VAL(ce->name));
            return -1;
        }
        ZVAL_COPY(tmp, zend_enum_fetch_case_value(Z_OBJ_P(v)));
        return 1;
    }
    return 0;
}

static int encode_element(smart_str *s, const char *key, size_t klen, zval *v, int depth, swm_error *err)
{
    size_t type_pos;
    uint8_t type = 0;
    zval tmp;
    int norm, rc = SUCCESS;

    ZVAL_DEREF(v);
    norm = normalize_value(v, &tmp, err);
    if (norm < 0) return FAILURE;
    if (norm) v = &tmp;

    type_pos = sstr_len(s);
    smart_str_appendc(s, 0);
    if (put_cstring(s, key, klen, "BSON keys", err) == FAILURE) { rc = FAILURE; goto done; }

    switch (Z_TYPE_P(v)) {
        case IS_NULL:
            type = 0x0A;
            break;
        case IS_FALSE:
        case IS_TRUE:
            type = 0x08;
            smart_str_appendc(s, Z_TYPE_P(v) == IS_TRUE ? 1 : 0);
            break;
        case IS_LONG:
            if (Z_LVAL_P(v) >= INT32_MIN && Z_LVAL_P(v) <= INT32_MAX) {
                type = 0x10;
                put_i32(s, (int32_t) Z_LVAL_P(v));
            } else {
                type = 0x12;
                put_i64(s, (int64_t) Z_LVAL_P(v));
            }
            break;
        case IS_DOUBLE: {
            uint64_t u;
            type = 0x01;
            memcpy(&u, &Z_DVAL_P(v), 8); /* raw bits, no FPU */
            put_i64(s, (int64_t) u);
            break;
        }
        case IS_STRING:
            type = 0x02;
            put_string(s, Z_STRVAL_P(v), Z_STRLEN_P(v));
            break;
        case IS_ARRAY: {
            zend_bool list = norm != 2 && zend_array_is_list(Z_ARRVAL_P(v));
            type = list ? 0x04 : 0x03;
            rc = encode_document(s, v, list, depth + 1, err);
            break;
        }
        case IS_OBJECT: {
            zend_class_entry *ce = Z_OBJCE_P(v);
            zend_object *o = Z_OBJ_P(v);
            if (ce == swm_ce_ObjectId) {
                uint8_t b[12];
                zval *hex = PROP(o, 0);
                type = 0x07;
                if (Z_TYPE_P(hex) != IS_STRING || Z_STRLEN_P(hex) != 24 || hex2bin(Z_STRVAL_P(hex), 24, b) == FAILURE) {
                    swm_error_set(err, SWM_ERR_BSON, 0, "Invalid ObjectId instance");
                    rc = FAILURE;
                    break;
                }
                smart_str_appendl(s, (char *) b, 12);
            } else if (ce == swm_ce_UTCDateTime) {
                type = 0x09;
                put_i64(s, Z_LVAL_P(PROP(o, 0)));
            } else if (ce == swm_ce_Binary) {
                zend_string *d = Z_STR_P(PROP(o, 0));
                zend_long sub = Z_LVAL_P(PROP(o, 1));
                type = 0x05;
                if (sub == 2) {
                    put_i32(s, (int32_t) ZSTR_LEN(d) + 4);
                    smart_str_appendc(s, (char) sub);
                    put_i32(s, (int32_t) ZSTR_LEN(d));
                } else {
                    put_i32(s, (int32_t) ZSTR_LEN(d));
                    smart_str_appendc(s, (char) sub);
                }
                smart_str_append(s, d);
            } else if (ce == swm_ce_Regex) {
                type = 0x0B;
                if (put_cstring(s, Z_STRVAL_P(PROP(o, 0)), Z_STRLEN_P(PROP(o, 0)), "Regex pattern", err) == FAILURE
                    || put_cstring(s, Z_STRVAL_P(PROP(o, 1)), Z_STRLEN_P(PROP(o, 1)), "Regex flags", err) == FAILURE) {
                    rc = FAILURE;
                }
            } else if (ce == swm_ce_Timestamp) {
                type = 0x11;
                put_i32(s, (int32_t) (uint32_t) Z_LVAL_P(PROP(o, 0)));
                put_i32(s, (int32_t) (uint32_t) Z_LVAL_P(PROP(o, 1)));
            } else if (ce == swm_ce_Int64) {
                type = 0x12;
                put_i64(s, Z_LVAL_P(PROP(o, 0)));
            } else if (ce == swm_ce_Decimal128) {
                uint64_t lo, hi;
                type = 0x13;
                if (dec128_from_string(Z_STRVAL_P(PROP(o, 0)), Z_STRLEN_P(PROP(o, 0)), &lo, &hi) == FAILURE) {
                    swm_error_set(err, SWM_ERR_BSON, 0, "Invalid Decimal128 instance");
                    rc = FAILURE;
                    break;
                }
                put_i64(s, (int64_t) lo);
                put_i64(s, (int64_t) hi);
            } else if (ce == swm_ce_MinKey) {
                type = 0xFF;
            } else if (ce == swm_ce_MaxKey) {
                type = 0x7F;
            } else if (ce == swm_ce_Javascript) {
                zval *code = PROP(o, 0), *scope = PROP(o, 1);
                if (Z_TYPE_P(scope) == IS_NULL) {
                    type = 0x0D;
                    put_string(s, Z_STRVAL_P(code), Z_STRLEN_P(code));
                } else {
                    size_t start = sstr_len(s);
                    type = 0x0F;
                    put_i32(s, 0);
                    put_string(s, Z_STRVAL_P(code), Z_STRLEN_P(code));
                    if (Z_TYPE_P(scope) == IS_OBJECT && Z_OBJCE_P(scope) == swm_ce_Document) {
                        smart_str_append(s, Z_STR_P(PROP(Z_OBJ_P(scope), 0)));
                    } else {
                        rc = encode_document(s, scope, 0, depth + 1, err);
                    }
                    if (rc == SUCCESS) patch_i32(s, start, (int32_t) (sstr_len(s) - start));
                }
            } else if (ce == swm_ce_Document) {
                type = 0x03;
                smart_str_append(s, Z_STR_P(PROP(o, 0)));
            } else if (ce == swm_ce_PackedArray) {
                type = 0x04;
                smart_str_append(s, Z_STR_P(PROP(o, 0)));
            } else if (instanceof_function(ce, swm_ce_Type)) {
                swm_error_set(err, SWM_ERR_BSON, 0, "Unexpected %s instance", ZSTR_VAL(ce->name));
                rc = FAILURE;
            } else {
                type = 0x03;
                rc = encode_document(s, v, 0, depth + 1, err);
            }
            break;
        }
        default:
            swm_error_set(err, SWM_ERR_BSON, 0, "Unsupported type %s for key \"%s\"", zend_zval_type_name(v), key);
            rc = FAILURE;
    }
    if (rc == SUCCESS) ZSTR_VAL(s->s)[type_pos] = (char) type;
done:
    if (norm) zval_ptr_dtor(&tmp);
    return rc;
}

static int encode_document(smart_str *s, zval *doc, zend_bool as_array, int depth, swm_error *err)
{
    HashTable *ht;
    zend_bool is_obj = Z_TYPE_P(doc) == IS_OBJECT;
    size_t start;
    zend_ulong idx;
    zend_string *key;
    zval *val;
    uint32_t i = 0;
    int rc = SUCCESS;

    if (depth > SWM_BSON_MAX_DEPTH) {
        swm_error_set(err, SWM_ERR_BSON, 0, "BSON document exceeds the maximum nesting depth of %d", SWM_BSON_MAX_DEPTH);
        return FAILURE;
    }
    ht = is_obj ? zend_get_properties_for(doc, ZEND_PROP_PURPOSE_JSON) : Z_ARRVAL_P(doc);
    start = sstr_len(s);
    put_i32(s, 0);
    if (ht) {
        if (GC_IS_RECURSIVE(ht)) {
            swm_error_set(err, SWM_ERR_BSON, 0, "Detected recursion for fieldname");
            if (is_obj) zend_release_properties(ht);
            return FAILURE;
        }
        GC_TRY_PROTECT_RECURSION(ht);
        ZEND_HASH_FOREACH_KEY_VAL_IND(ht, idx, key, val) {
            char buf[32];
            const char *k;
            size_t kl;
            if (as_array) {
                kl = (size_t) snprintf(buf, sizeof(buf), "%u", i++);
                k = buf;
            } else if (key) {
                if (is_obj && ZSTR_LEN(key) > 0 && ZSTR_VAL(key)[0] == '\0') continue;
                k = ZSTR_VAL(key);
                kl = ZSTR_LEN(key);
            } else {
                kl = (size_t) snprintf(buf, sizeof(buf), ZEND_ULONG_FMT, idx);
                k = buf;
            }
            if (encode_element(s, k, kl, val, depth, err) == FAILURE) { rc = FAILURE; break; }
        } ZEND_HASH_FOREACH_END();
        GC_TRY_UNPROTECT_RECURSION(ht);
        if (is_obj) zend_release_properties(ht);
    }
    if (rc == FAILURE) return FAILURE;
    smart_str_appendc(s, '\0');
    patch_i32(s, start, (int32_t) (sstr_len(s) - start));
    return SUCCESS;
}

int swm_bson_encode(smart_str *out, zval *doc, swm_error *err)
{
    zval tmp;
    int norm, rc;
    ZVAL_DEREF(doc);
    if (Z_TYPE_P(doc) == IS_OBJECT && Z_OBJCE_P(doc) == swm_ce_Document) {
        smart_str_append(out, Z_STR_P(PROP(Z_OBJ_P(doc), 0)));
        return SUCCESS;
    }
    if (Z_TYPE_P(doc) == IS_OBJECT && Z_OBJCE_P(doc) == swm_ce_PackedArray) {
        swm_error_set(err, SWM_ERR_BSON, 0, "SwooleMongo\\BSON\\PackedArray cannot be serialized as a root element");
        return FAILURE;
    }
    norm = normalize_value(doc, &tmp, err);
    if (norm < 0) return FAILURE;
    if (norm) doc = &tmp;
    if (Z_TYPE_P(doc) == IS_OBJECT && instanceof_function(Z_OBJCE_P(doc), swm_ce_Type)) {
        if (norm) zval_ptr_dtor(&tmp);
        swm_error_set(err, SWM_ERR_BSON, 0, "%s instance cannot be serialized as a root element", ZSTR_VAL(Z_OBJCE_P(doc)->name));
        return FAILURE;
    }
    if (Z_TYPE_P(doc) != IS_ARRAY && Z_TYPE_P(doc) != IS_OBJECT) {
        if (norm) zval_ptr_dtor(&tmp);
        swm_error_set(err, SWM_ERR_BSON, 0, "Expected value to be an array or object, %s given", zend_zval_type_name(doc));
        return FAILURE;
    }
    rc = encode_document(out, doc, 0, 0, err);
    if (norm) zval_ptr_dtor(&tmp);
    return rc;
}

/* ================================================================= typeMap */
static swm_typemap default_tm; /* all SWM_TM_DEFAULT */

void swm_typemap_init(swm_typemap *tm)
{
    memset(tm, 0, sizeof(*tm));
}

void swm_typemap_dtor(swm_typemap *tm)
{
    for (int i = 0; i < tm->npaths; i++) {
        for (int j = 0; j < tm->paths[i].nsegs; j++) zend_string_release(tm->paths[i].segs[j]);
        if (tm->paths[i].segs) efree(tm->paths[i].segs);
    }
    if (tm->paths) efree(tm->paths);
    swm_typemap_init(tm);
}

static int tm_entry_parse(zval *v, swm_tm_entry *e, const char *what, swm_error *err)
{
    zend_class_entry *ce;
    if (Z_TYPE_P(v) != IS_STRING) {
        swm_error_set(err, SWM_ERR_ARG, 0, "Expected typeMap[\"%s\"] to be string, %s given", what, zend_zval_type_name(v));
        return FAILURE;
    }
    e->ce = NULL;
    if (!strcasecmp(Z_STRVAL_P(v), "array")) { e->kind = SWM_TM_ARRAY; return SUCCESS; }
    if (!strcasecmp(Z_STRVAL_P(v), "object") || !strcasecmp(Z_STRVAL_P(v), "stdclass")) { e->kind = SWM_TM_OBJECT; return SUCCESS; }
    if (!strcasecmp(Z_STRVAL_P(v), "bson")) { e->kind = SWM_TM_BSON; return SUCCESS; }
    ce = zend_lookup_class(Z_STR_P(v));
    if (!ce) {
        swm_error_set(err, SWM_ERR_ARG, 0, "Class %s does not exist", Z_STRVAL_P(v));
        return FAILURE;
    }
    if (ce->ce_flags & (ZEND_ACC_INTERFACE | ZEND_ACC_TRAIT | ZEND_ACC_EXPLICIT_ABSTRACT_CLASS | ZEND_ACC_ENUM)) {
        swm_error_set(err, SWM_ERR_ARG, 0, "Class %s is not instantiatable", ZSTR_VAL(ce->name));
        return FAILURE;
    }
    if (!instanceof_function(ce, swm_ce_Unserializable)) {
        swm_error_set(err, SWM_ERR_ARG, 0, "Class %s does not implement SwooleMongo\\BSON\\Unserializable", ZSTR_VAL(ce->name));
        return FAILURE;
    }
    e->kind = SWM_TM_CLASS;
    e->ce = ce;
    return SUCCESS;
}

int swm_typemap_parse(zval *arr, swm_typemap *tm, swm_error *err)
{
    zval *v;
    swm_typemap_init(tm);
    if (!arr || Z_TYPE_P(arr) != IS_ARRAY) return SUCCESS;
    if ((v = zend_hash_str_find_deref(Z_ARRVAL_P(arr), ZEND_STRL("root"))) && tm_entry_parse(v, &tm->root, "root", err) == FAILURE) return FAILURE;
    if ((v = zend_hash_str_find_deref(Z_ARRVAL_P(arr), ZEND_STRL("document"))) && tm_entry_parse(v, &tm->document, "document", err) == FAILURE) return FAILURE;
    if ((v = zend_hash_str_find_deref(Z_ARRVAL_P(arr), ZEND_STRL("array"))) && tm_entry_parse(v, &tm->array, "array", err) == FAILURE) return FAILURE;
    if ((v = zend_hash_str_find_deref(Z_ARRVAL_P(arr), ZEND_STRL("fieldPaths")))) {
        zend_string *path;
        zval *t;
        if (Z_TYPE_P(v) != IS_ARRAY) {
            swm_error_set(err, SWM_ERR_ARG, 0, "Expected fieldPaths to be an array");
            return FAILURE;
        }
        tm->paths = ecalloc(zend_hash_num_elements(Z_ARRVAL_P(v)) + 1, sizeof(swm_tm_path));
        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(v), path, t) {
            swm_tm_path *p;
            const char *c, *e;
            if (!path || ZSTR_LEN(path) == 0 || ZSTR_VAL(path)[0] == '.' || ZSTR_VAL(path)[ZSTR_LEN(path) - 1] == '.') {
                swm_typemap_dtor(tm);
                swm_error_set(err, SWM_ERR_ARG, 0, "A 'fieldPaths' key may not start or end with a '.'");
                return FAILURE;
            }
            p = &tm->paths[tm->npaths];
            if (tm_entry_parse(t, &p->entry, ZSTR_VAL(path), err) == FAILURE) { swm_typemap_dtor(tm); return FAILURE; }
            p->segs = emalloc(sizeof(zend_string *) * (ZSTR_LEN(path) + 1));
            tm->npaths++;
            c = ZSTR_VAL(path);
            e = c + ZSTR_LEN(path);
            for (;;) {
                const char *dot = memchr(c, '.', e - c);
                const char *se = dot ? dot : e;
                if (se == c) {
                    swm_typemap_dtor(tm);
                    swm_error_set(err, SWM_ERR_ARG, 0, "A 'fieldPaths' key may not have an empty segment");
                    return FAILURE;
                }
                p->segs[p->nsegs++] = zend_string_init(c, se - c, 0);
                if (!dot) break;
                c = dot + 1;
            }
        } ZEND_HASH_FOREACH_END();
    }
    return SUCCESS;
}

/* ================================================================ DECODER */
typedef struct {
    swm_typemap *tm; /* NULL: internal decoding (arrays everywhere) */
    const char *seg[SWM_BSON_MAX_DEPTH + 2];
    size_t seglen[SWM_BSON_MAX_DEPTH + 2];
    int depth;
} dctx;

static swm_tm_entry *child_entry(dctx *c, zend_bool is_array)
{
    if (!c->tm) return NULL;
    for (int i = 0; i < c->tm->npaths; i++) {
        swm_tm_path *p = &c->tm->paths[i];
        int ok = 1;
        if (p->nsegs != c->depth) continue;
        for (int j = 0; j < p->nsegs; j++) {
            zend_string *sg = p->segs[j];
            if (ZSTR_LEN(sg) == 1 && ZSTR_VAL(sg)[0] == '$') continue;
            if (ZSTR_LEN(sg) != c->seglen[j] || memcmp(ZSTR_VAL(sg), c->seg[j], c->seglen[j]) != 0) { ok = 0; break; }
        }
        if (ok) return &p->entry;
    }
    return is_array ? &c->tm->array : &c->tm->document;
}

void swm_new_document_raw(zval *out, zend_class_entry *ce, const char *data, size_t len)
{
    object_init_ex(out, ce);
    prop_set_str(Z_OBJ_P(out), 0, zend_string_init(data, len, 0));
}

static int unserialize_into(zend_class_entry *ce, zval *arr)
{
    zval obj, ret;
    if (object_init_ex(&obj, ce) == FAILURE) return FAILURE;
    zend_call_method_with_1_params(Z_OBJ(obj), ce, NULL, "bsonunserialize", &ret, arr);
    zval_ptr_dtor(&ret);
    if (EG(exception)) {
        zval_ptr_dtor(&obj);
        return FAILURE;
    }
    zval_ptr_dtor(arr);
    ZVAL_COPY_VALUE(arr, &obj);
    return SUCCESS;
}

/* Converts the freshly decoded PHP array according to the typeMap entry */
static int finalize(zval *arr, swm_tm_entry *e, zend_bool is_array)
{
    if (!e) return SUCCESS;
    switch (e->kind) {
        case SWM_TM_ARRAY:
            return SUCCESS;
        case SWM_TM_DEFAULT:
            if (is_array) return SUCCESS;
            {
                zval *pc = zend_hash_str_find(Z_ARRVAL_P(arr), ZEND_STRL(PCLASS));
                zend_string *name;
                if (pc && (name = swm_binary_data(pc)) && Z_LVAL_P(PROP(Z_OBJ_P(pc), 1)) == 0x80) {
                    zend_class_entry *ce = zend_lookup_class(name);
                    if (ce && !(ce->ce_flags & (ZEND_ACC_INTERFACE | ZEND_ACC_TRAIT | ZEND_ACC_EXPLICIT_ABSTRACT_CLASS | ZEND_ACC_ENUM))
                        && instanceof_function(ce, swm_ce_Persistable)) {
                        return unserialize_into(ce, arr);
                    }
                    if (EG(exception)) return FAILURE;
                }
            }
            convert_to_object(arr);
            return SUCCESS;
        case SWM_TM_OBJECT:
            convert_to_object(arr);
            return SUCCESS;
        case SWM_TM_CLASS:
            return unserialize_into(e->ce, arr);
        default:
            return SUCCESS;
    }
}

#define CORRUPT(msg) do { swm_error_set(err, SWM_ERR_BSON, 0, "Could not read document from BSON reader: %s", msg); goto fail; } while (0)

static int decode_doc(dctx *ctx, const char *data, size_t avail, size_t *used, zval *out, zend_bool as_array,
                      swm_tm_entry *entry, swm_error *err)
{
    int32_t dl;
    size_t pos, end;

    if (avail < 5) { swm_error_set(err, SWM_ERR_BSON, 0, "Could not read document from BSON reader"); return FAILURE; }
    dl = rd_i32(data);
    if (dl < 5 || (size_t) dl > avail || data[dl - 1] != '\0') {
        swm_error_set(err, SWM_ERR_BSON, 0, "Could not read document from BSON reader");
        return FAILURE;
    }
    array_init(out);
    pos = 4;
    end = (size_t) dl - 1;
    while (pos < end) {
        uint8_t t = (uint8_t) data[pos++];
        const char *key = data + pos, *nul;
        size_t kl;
        zval v;
        nul = memchr(key, 0, end - pos);
        if (!nul) CORRUPT("unterminated key");
        kl = (size_t) (nul - key);
        pos += kl + 1;
#define NEED(n) if (end - pos < (size_t) (n)) CORRUPT("truncated value")
        switch (t) {
            case 0x01:
                NEED(8);
                swm_zval_set_double_bits(&v, (uint64_t) rd_i64(data + pos));
                pos += 8;
                break;
            case 0x02: case 0x0D: case 0x0E: {
                int32_t sl;
                NEED(4);
                sl = rd_i32(data + pos);
                if (sl < 1 || (size_t) sl > end - pos - 4 || data[pos + 4 + sl - 1] != '\0') CORRUPT("invalid string length");
                if (t == 0x0D) {
                    object_init_ex(&v, swm_ce_Javascript);
                    prop_set_str(Z_OBJ(v), 0, zend_string_init(data + pos + 4, sl - 1, 0));
                } else {
                    ZVAL_STRINGL(&v, data + pos + 4, sl - 1);
                }
                pos += 4 + (size_t) sl;
                break;
            }
            case 0x03: case 0x04: {
                size_t u;
                swm_tm_entry *ce;
                int32_t sub;
                NEED(5);
                sub = rd_i32(data + pos);
                if (sub < 5 || (size_t) sub > end - pos || data[pos + sub - 1] != '\0') CORRUPT("invalid embedded document");
                if (ctx->depth >= SWM_BSON_MAX_DEPTH) CORRUPT("nesting too deep");
                ctx->seg[ctx->depth] = key;
                ctx->seglen[ctx->depth] = kl;
                ctx->depth++;
                ce = child_entry(ctx, t == 0x04);
                if (ce && ce->kind == SWM_TM_BSON) {
                    swm_new_document_raw(&v, t == 0x04 ? swm_ce_PackedArray : swm_ce_Document, data + pos, (size_t) sub);
                    u = (size_t) sub;
                } else if (decode_doc(ctx, data + pos, end - pos, &u, &v, t == 0x04, ce, err) == FAILURE) {
                    ctx->depth--;
                    goto fail;
                }
                ctx->depth--;
                pos += u;
                break;
            }
            case 0x05: {
                int32_t bl;
                uint8_t sub;
                NEED(5);
                bl = rd_i32(data + pos);
                sub = (uint8_t) data[pos + 4];
                if (bl < 0 || (size_t) bl > end - pos - 5) CORRUPT("invalid binary length");
                if (sub == 2) {
                    int32_t il;
                    if (bl < 4) CORRUPT("invalid old binary length");
                    il = rd_i32(data + pos + 5);
                    if (il != bl - 4) CORRUPT("invalid old binary length");
                    swm_new_binary(&v, data + pos + 9, (size_t) il, sub);
                } else {
                    swm_new_binary(&v, data + pos + 5, (size_t) bl, sub);
                }
                pos += 5 + (size_t) bl;
                break;
            }
            case 0x06: case 0x0A:
                ZVAL_NULL(&v);
                break;
            case 0x07:
                NEED(12);
                new_oid(&v, (const uint8_t *) data + pos);
                pos += 12;
                break;
            case 0x08:
                NEED(1);
                ZVAL_BOOL(&v, data[pos] != 0);
                pos += 1;
                break;
            case 0x09:
                NEED(8);
                new_utcdatetime(&v, rd_i64(data + pos));
                pos += 8;
                break;
            case 0x0B: {
                const char *p1 = data + pos, *e1, *p2, *e2;
                e1 = memchr(p1, 0, end - pos);
                if (!e1) CORRUPT("unterminated regex");
                p2 = e1 + 1;
                e2 = memchr(p2, 0, end - (size_t) (p2 - data));
                if (!e2) CORRUPT("unterminated regex flags");
                new_regex(&v, p1, (size_t) (e1 - p1), p2, (size_t) (e2 - p2));
                pos = (size_t) (e2 - data) + 1;
                break;
            }
            case 0x0C: {
                int32_t sl;
                zval id;
                NEED(4);
                sl = rd_i32(data + pos);
                if (sl < 1 || (size_t) sl + 12 > end - pos - 4) CORRUPT("invalid DBPointer");
                array_init(&v);
                add_assoc_stringl(&v, "$ref", data + pos + 4, sl - 1);
                new_oid(&id, (const uint8_t *) data + pos + 4 + sl);
                add_assoc_zval(&v, "$id", &id);
                pos += 4 + (size_t) sl + 12;
                break;
            }
            case 0x0F: {
                int32_t total, sl;
                size_t u;
                zval scope;
                dctx sub;
                NEED(4);
                memset(&sub, 0, sizeof(sub));
                total = rd_i32(data + pos);
                if (total < 14 || (size_t) total > end - pos) CORRUPT("invalid code with scope");
                sl = rd_i32(data + pos + 4);
                if (sl < 1 || (size_t) sl > (size_t) total - 8 || data[pos + 8 + sl - 1] != '\0') CORRUPT("invalid code string");
                if (decode_doc(&sub, data + pos + 8 + sl, (size_t) total - 8 - (size_t) sl, &u, &scope, 0, NULL, err) == FAILURE) goto fail;
                object_init_ex(&v, swm_ce_Javascript);
                prop_set_str(Z_OBJ(v), 0, zend_string_init(data + pos + 8, sl - 1, 0));
                prop_set_zval(Z_OBJ(v), 1, &scope);
                zval_ptr_dtor(&scope);
                pos += (size_t) total;
                break;
            }
            case 0x10:
                NEED(4);
                ZVAL_LONG(&v, rd_i32(data + pos));
                pos += 4;
                break;
            case 0x11:
                NEED(8);
                swm_new_timestamp(&v, rd_u32(data + pos), rd_u32(data + pos + 4));
                pos += 8;
                break;
            case 0x12:
                NEED(8);
                ZVAL_LONG(&v, (zend_long) rd_i64(data + pos));
                pos += 8;
                break;
            case 0x13:
                NEED(16);
                new_decimal(&v, (uint64_t) rd_i64(data + pos), (uint64_t) rd_i64(data + pos + 8));
                pos += 16;
                break;
            case 0xFF:
                object_init_ex(&v, swm_ce_MinKey);
                break;
            case 0x7F:
                object_init_ex(&v, swm_ce_MaxKey);
                break;
            default:
                swm_error_set(err, SWM_ERR_BSON, 0, "Detected unknown BSON type 0x%02x for field path \"%.*s\"", (unsigned) t, (int) kl, key);
                goto fail;
        }
#undef NEED
        if (as_array) {
            add_next_index_zval(out, &v);
        } else {
            zend_symtable_str_update(Z_ARRVAL_P(out), key, kl, &v);
        }
    }
    if (pos != end) {
        swm_error_set(err, SWM_ERR_BSON, 0, "Could not read document from BSON reader: trailing data");
        goto fail;
    }
    *used = (size_t) dl;
    if (finalize(out, entry, as_array) == FAILURE) goto fail;
    return SUCCESS;
fail:
    zval_ptr_dtor(out);
    ZVAL_UNDEF(out);
    return FAILURE;
}

int swm_bson_decode_one(const char *data, size_t len, size_t *used, zval *out, swm_error *err)
{
    dctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    return decode_doc(&ctx, data, len, used, out, 0, NULL, err);
}

int swm_bson_decode(const char *data, size_t len, zval *out, swm_error *err)
{
    size_t used;
    if (swm_bson_decode_one(data, len, &used, out, err) == FAILURE) return FAILURE;
    if (used != len) {
        zval_ptr_dtor(out);
        ZVAL_UNDEF(out);
        swm_error_set(err, SWM_ERR_BSON, 0, "Reading document did not exhaust input buffer");
        return FAILURE;
    }
    return SUCCESS;
}

static int decode_tm_root(const char *data, size_t len, swm_typemap *tm, swm_tm_entry *root, zend_bool as_array, zval *out, swm_error *err)
{
    dctx ctx;
    size_t used;
    memset(&ctx, 0, sizeof(ctx));
    ctx.tm = tm ? tm : &default_tm;
    if (root && root->kind == SWM_TM_BSON) {
        swm_new_document_raw(out, as_array ? swm_ce_PackedArray : swm_ce_Document, data, len);
        return SUCCESS;
    }
    if (decode_doc(&ctx, data, len, &used, out, as_array, root, err) == FAILURE) return FAILURE;
    if (used != len) {
        zval_ptr_dtor(out);
        ZVAL_UNDEF(out);
        swm_error_set(err, SWM_ERR_BSON, 0, "Reading document did not exhaust input buffer");
        return FAILURE;
    }
    return SUCCESS;
}

int swm_bson_decode_tm(const char *data, size_t len, swm_typemap *tm, zval *out, swm_error *err)
{
    swm_typemap *t = tm ? tm : &default_tm;
    return decode_tm_root(data, len, t, &t->root, 0, out, err);
}

static size_t element_size(uint8_t t, const char *v, size_t avail)
{
    switch (t) {
        case 0x01: case 0x09: case 0x11: case 0x12: return 8;
        case 0x02: case 0x0D: case 0x0E: return avail < 4 ? (size_t) -1 : 4 + (size_t) rd_i32(v);
        case 0x03: case 0x04: case 0x0F: return avail < 4 ? (size_t) -1 : (size_t) rd_i32(v);
        case 0x05: return avail < 4 ? (size_t) -1 : 5 + (size_t) rd_i32(v);
        case 0x06: case 0x0A: case 0xFF: case 0x7F: return 0;
        case 0x07: return 12;
        case 0x08: return 1;
        case 0x0B: {
            const char *e1 = memchr(v, 0, avail), *e2;
            if (!e1) return (size_t) -1;
            e2 = memchr(e1 + 1, 0, avail - (size_t) (e1 + 1 - v));
            if (!e2) return (size_t) -1;
            return (size_t) (e2 + 1 - v);
        }
        case 0x0C: return avail < 4 ? (size_t) -1 : 4 + (size_t) rd_i32(v) + 12;
        case 0x10: return 4;
        case 0x13: return 16;
        default: return (size_t) -1;
    }
}

uint8_t swm_bson_find(const char *data, size_t len, const char *key, const char **val, size_t *vlen)
{
    size_t klen = strlen(key), pos = 4, end;
    if (len < 5) return 0;
    end = len - 1;
    while (pos < end) {
        uint8_t t = (uint8_t) data[pos++];
        const char *k = data + pos, *nul = memchr(k, 0, end - pos);
        size_t kl, sz;
        if (!nul) return 0;
        kl = (size_t) (nul - k);
        pos += kl + 1;
        sz = element_size(t, data + pos, end - pos);
        if (sz == (size_t) -1 || sz > end - pos) return 0;
        if (kl == klen && memcmp(k, key, kl) == 0) {
            *val = data + pos;
            *vlen = sz;
            return t;
        }
        pos += sz;
    }
    return 0;
}

void swm_bson_split(const char *data, size_t len, zval *out)
{
    size_t pos = 4, end = len ? len - 1 : 0;
    array_init(out);
    while (len >= 5 && pos < end) {
        uint8_t t = (uint8_t) data[pos++];
        const char *nul = memchr(data + pos, 0, end - pos);
        int32_t sz;
        if (!nul) return;
        pos = (size_t) (nul - data) + 1;
        if (t != 0x03 || end - pos < 5) return;
        sz = rd_i32(data + pos);
        if (sz < 5 || (size_t) sz > end - pos) return;
        add_next_index_stringl(out, data + pos, (size_t) sz);
        pos += (size_t) sz;
    }
}

void swm_array_to_object_doc(zval *arr, zval *out)
{
    smart_str s = {0};
    swm_error err;
    swm_error_init(&err);
    if (swm_bson_encode(&s, arr, &err) == SUCCESS && swm_bson_decode_tm(ZSTR_VAL(s.s), ZSTR_LEN(s.s), NULL, out, &err) == SUCCESS) {
        smart_str_free(&s);
        return;
    }
    smart_str_free(&s);
    swm_error_clear(&err);
    if (EG(exception)) zend_clear_exception();
    ZVAL_COPY(out, arr);
}

/* ============================================================ Extended JSON */
static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    int64_t era, yy;
    unsigned doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned) (z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    yy = (int64_t) yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (*m <= 2);
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    int64_t era;
    unsigned yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned) (y - era * 400);
    doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t) doe - 719468;
}

static void ej_string(smart_str *s, const char *str, size_t len)
{
    zval tmp;
    ZVAL_STRINGL(&tmp, str, len);
    php_json_encode(s, &tmp, PHP_JSON_UNESCAPED_SLASHES | PHP_JSON_UNESCAPED_UNICODE | PHP_JSON_INVALID_UTF8_SUBSTITUTE);
    zval_ptr_dtor(&tmp);
}

static void ej_doc(smart_str *s, const char *data, size_t len, zend_bool canonical, zend_bool is_array);

static void ej_value(smart_str *s, uint8_t t, const char *v, zend_bool canonical)
{
    switch (t) {
        case 0x01:
            swm_append_double(s, (uint64_t) rd_i64(v), !canonical);
            break;
        case 0x02:
            ej_string(s, v + 4, (size_t) rd_i32(v) - 1);
            break;
        case 0x03: case 0x04:
            ej_doc(s, v, (size_t) rd_i32(v), canonical, t == 0x04);
            break;
        case 0x05: {
            int32_t bl = rd_i32(v);
            uint8_t sub = (uint8_t) v[4];
            const char *d = v + 5;
            zend_string *b64;
            char st[8];
            if (sub == 2 && bl >= 4) { d += 4; bl -= 4; }
            b64 = php_base64_encode((const unsigned char *) d, (size_t) bl);
            snprintf(st, sizeof(st), "%02x", sub);
            smart_str_appends(s, "{ \"$binary\" : { \"base64\" : \"");
            smart_str_append(s, b64);
            smart_str_appends(s, "\", \"subType\" : \"");
            smart_str_appends(s, st);
            smart_str_appends(s, "\" } }");
            zend_string_release(b64);
            break;
        }
        case 0x06:
            smart_str_appends(s, "{ \"$undefined\" : true }");
            break;
        case 0x07: {
            char hex[25];
            bin2hex((const uint8_t *) v, 12, hex);
            smart_str_appends(s, "{ \"$oid\" : \"");
            smart_str_appends(s, hex);
            smart_str_appends(s, "\" }");
            break;
        }
        case 0x08:
            smart_str_appends(s, v[0] ? "true" : "false");
            break;
        case 0x09: {
            int64_t ms = rd_i64(v);
            if (!canonical && ms >= 0 && ms <= 253402300799999LL) {
                int64_t secs = ms / 1000, days = secs / 86400, rem = secs % 86400, y;
                unsigned mo, d;
                char buf[64];
                civil_from_days(days, &y, &mo, &d);
                if (ms % 1000) {
                    snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", (int) y, mo, d, (int) (rem / 3600),
                             (int) (rem % 3600 / 60), (int) (rem % 60), (int) (ms % 1000));
                } else {
                    snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02d:%02d:%02dZ", (int) y, mo, d, (int) (rem / 3600),
                             (int) (rem % 3600 / 60), (int) (rem % 60));
                }
                smart_str_appends(s, "{ \"$date\" : \"");
                smart_str_appends(s, buf);
                smart_str_appends(s, "\" }");
            } else {
                smart_str_appends(s, "{ \"$date\" : { \"$numberLong\" : \"");
                smart_str_append_long(s, (zend_long) ms);
                smart_str_appends(s, "\" } }");
            }
            break;
        }
        case 0x0A:
            smart_str_appends(s, "null");
            break;
        case 0x0B: {
            size_t pl = strlen(v);
            smart_str_appends(s, "{ \"$regularExpression\" : { \"pattern\" : ");
            ej_string(s, v, pl);
            smart_str_appends(s, ", \"options\" : ");
            ej_string(s, v + pl + 1, strlen(v + pl + 1));
            smart_str_appends(s, " } }");
            break;
        }
        case 0x0C: {
            int32_t sl = rd_i32(v);
            char hex[25];
            bin2hex((const uint8_t *) v + 4 + sl, 12, hex);
            smart_str_appends(s, "{ \"$dbPointer\" : { \"$ref\" : ");
            ej_string(s, v + 4, (size_t) sl - 1);
            smart_str_appends(s, ", \"$id\" : { \"$oid\" : \"");
            smart_str_appends(s, hex);
            smart_str_appends(s, "\" } } }");
            break;
        }
        case 0x0D:
            smart_str_appends(s, "{ \"$code\" : ");
            ej_string(s, v + 4, (size_t) rd_i32(v) - 1);
            smart_str_appends(s, " }");
            break;
        case 0x0E:
            smart_str_appends(s, "{ \"$symbol\" : ");
            ej_string(s, v + 4, (size_t) rd_i32(v) - 1);
            smart_str_appends(s, " }");
            break;
        case 0x0F: {
            int32_t sl = rd_i32(v + 4);
            smart_str_appends(s, "{ \"$code\" : ");
            ej_string(s, v + 8, (size_t) sl - 1);
            smart_str_appends(s, ", \"$scope\" : ");
            ej_doc(s, v + 8 + sl, (size_t) rd_i32(v + 8 + sl), canonical, 0);
            smart_str_appends(s, " }");
            break;
        }
        case 0x10:
            if (canonical) {
                smart_str_appends(s, "{ \"$numberInt\" : \"");
                smart_str_append_long(s, rd_i32(v));
                smart_str_appends(s, "\" }");
            } else {
                smart_str_append_long(s, rd_i32(v));
            }
            break;
        case 0x11:
            smart_str_appends(s, "{ \"$timestamp\" : { \"t\" : ");
            smart_str_append_unsigned(s, rd_u32(v + 4));
            smart_str_appends(s, ", \"i\" : ");
            smart_str_append_unsigned(s, rd_u32(v));
            smart_str_appends(s, " } }");
            break;
        case 0x12:
            if (canonical) {
                smart_str_appends(s, "{ \"$numberLong\" : \"");
                smart_str_append_long(s, (zend_long) rd_i64(v));
                smart_str_appends(s, "\" }");
            } else {
                smart_str_append_long(s, (zend_long) rd_i64(v));
            }
            break;
        case 0x13: {
            zend_string *d = dec128_to_string((uint64_t) rd_i64(v), (uint64_t) rd_i64(v + 8));
            smart_str_appends(s, "{ \"$numberDecimal\" : \"");
            smart_str_append(s, d);
            smart_str_appends(s, "\" }");
            zend_string_release(d);
            break;
        }
        case 0xFF:
            smart_str_appends(s, "{ \"$minKey\" : 1 }");
            break;
        case 0x7F:
            smart_str_appends(s, "{ \"$maxKey\" : 1 }");
            break;
    }
}

/* data must be a validated BSON document */
static void ej_doc(smart_str *s, const char *data, size_t len, zend_bool canonical, zend_bool is_array)
{
    size_t pos = 4, end = len - 1;
    int first = 1;
    smart_str_appends(s, is_array ? "[" : "{");
    while (pos < end) {
        uint8_t t = (uint8_t) data[pos++];
        const char *k = data + pos;
        size_t kl = strlen(k);
        pos += kl + 1;
        smart_str_appends(s, first ? " " : ", ");
        first = 0;
        if (!is_array) {
            ej_string(s, k, kl);
            smart_str_appends(s, " : ");
        }
        ej_value(s, t, data + pos, canonical);
        pos += element_size(t, data + pos, end - pos);
    }
    smart_str_appends(s, is_array ? " ]" : " }");
}

/* ---- JSON -> BSON */
static int ej_write_doc(smart_str *s, zval *v, zend_bool as_array, int depth, swm_error *err);

static zval *objprop(zval *o, const char *k)
{
    return zend_hash_str_find(Z_OBJPROP_P(o), k, strlen(k));
}

static int parse_iso_date(const char *s, size_t len, int64_t *ms)
{
    int y, mo, d, h, mi, sec, frac = 0, fd = 0, tzs = 0, tzh = 0, tzm = 0;
    const char *p, *end = s + len;
    if (len < 20 || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec) != 6) return FAILURE;
    p = s + 19;
    if (p < end && *p == '.') {
        p++;
        while (p < end && *p >= '0' && *p <= '9') {
            if (fd < 3) { frac = frac * 10 + (*p - '0'); fd++; }
            p++;
        }
        while (fd < 3) { frac *= 10; fd++; }
    }
    if (p < end && *p == 'Z') {
        p++;
    } else if (p < end && (*p == '+' || *p == '-')) {
        tzs = *p == '-' ? -1 : 1;
        if (end - p < 5) return FAILURE;
        tzh = (p[1] - '0') * 10 + (p[2] - '0');
        p += 3;
        if (*p == ':') p++;
        if (end - p < 2) return FAILURE;
        tzm = (p[0] - '0') * 10 + (p[1] - '0');
        p += 2;
    } else {
        return FAILURE;
    }
    if (p != end) return FAILURE;
    *ms = ((days_from_civil(y, (unsigned) mo, (unsigned) d) * 86400 + h * 3600 + mi * 60 + sec) - tzs * (tzh * 3600 + tzm * 60)) * 1000 + frac;
    return SUCCESS;
}

/* Converts an Extended JSON wrapper object into a BSON value zval. 1 = converted, 0 = not a wrapper, -1 = error */
static int ej_wrapper(zval *o, zval *out, swm_error *err)
{
    HashTable *props = Z_OBJPROP_P(o);
    uint32_t n = zend_hash_num_elements(props);
    zend_string *k1 = NULL;
    zval *v1 = NULL, *x;
    if (n == 0 || n > 2) return 0;
    ZEND_HASH_FOREACH_STR_KEY_VAL(props, k1, v1) { break; } ZEND_HASH_FOREACH_END();
    if (!k1 || ZSTR_LEN(k1) == 0 || ZSTR_VAL(k1)[0] != '$') return 0;
#define BAD(msg) do { swm_error_set(err, SWM_ERR_BSON, 0, "Invalid Extended JSON %s", msg); return -1; } while (0)
    if (n == 2) {
        zval *code = objprop(o, "$code"), *scope = objprop(o, "$scope");
        zval *bin = objprop(o, "$binary"), *type = objprop(o, "$type");
        zval *re = objprop(o, "$regex"), *opts = objprop(o, "$options");
        if (code && scope) {
            smart_str sc = {0};
            zval d;
            if (Z_TYPE_P(code) != IS_STRING || Z_TYPE_P(scope) != IS_OBJECT) BAD("$code/$scope");
            if (ej_write_doc(&sc, scope, 0, 1, err) == FAILURE) { smart_str_free(&sc); return -1; }
            object_init_ex(out, swm_ce_Javascript);
            prop_set_str(Z_OBJ_P(out), 0, zend_string_copy(Z_STR_P(code)));
            swm_new_document_raw(&d, swm_ce_Document, ZSTR_VAL(sc.s), ZSTR_LEN(sc.s));
            prop_set_zval(Z_OBJ_P(out), 1, &d);
            zval_ptr_dtor(&d);
            smart_str_free(&sc);
            return 1;
        }
        if (bin && type && Z_TYPE_P(bin) == IS_STRING && Z_TYPE_P(type) == IS_STRING) {
            zend_string *raw = php_base64_decode((unsigned char *) Z_STRVAL_P(bin), Z_STRLEN_P(bin));
            if (!raw) BAD("$binary");
            swm_new_binary(out, ZSTR_VAL(raw), ZSTR_LEN(raw), ZEND_STRTOL(Z_STRVAL_P(type), NULL, 16));
            zend_string_release(raw);
            return 1;
        }
        if (re && opts && Z_TYPE_P(re) == IS_STRING && Z_TYPE_P(opts) == IS_STRING) {
            new_regex(out, Z_STRVAL_P(re), Z_STRLEN_P(re), Z_STRVAL_P(opts), Z_STRLEN_P(opts));
            return 1;
        }
        return 0;
    }
    if (zend_string_equals_literal(k1, "$oid")) {
        uint8_t b[12];
        if (Z_TYPE_P(v1) != IS_STRING || Z_STRLEN_P(v1) != 24 || hex2bin(Z_STRVAL_P(v1), 24, b) == FAILURE) BAD("$oid");
        new_oid(out, b);
        return 1;
    }
    if (zend_string_equals_literal(k1, "$date")) {
        int64_t ms;
        if (Z_TYPE_P(v1) == IS_STRING) {
            if (parse_iso_date(Z_STRVAL_P(v1), Z_STRLEN_P(v1), &ms) == FAILURE) BAD("$date");
        } else if (Z_TYPE_P(v1) == IS_LONG) {
            ms = Z_LVAL_P(v1);
        } else if (Z_TYPE_P(v1) == IS_OBJECT && (x = objprop(v1, "$numberLong")) && Z_TYPE_P(x) == IS_STRING) {
            ms = ZEND_STRTOL(Z_STRVAL_P(x), NULL, 10);
        } else {
            BAD("$date");
        }
        new_utcdatetime(out, ms);
        return 1;
    }
    if (zend_string_equals_literal(k1, "$numberLong")) {
        if (Z_TYPE_P(v1) != IS_STRING) BAD("$numberLong");
        swm_new_int64(out, ZEND_STRTOL(Z_STRVAL_P(v1), NULL, 10));
        return 1;
    }
    if (zend_string_equals_literal(k1, "$numberInt")) {
        if (Z_TYPE_P(v1) != IS_STRING) BAD("$numberInt");
        ZVAL_LONG(out, ZEND_STRTOL(Z_STRVAL_P(v1), NULL, 10));
        return 1;
    }
    if (zend_string_equals_literal(k1, "$numberDouble")) {
        int ok;
        uint64_t bits;
        if (Z_TYPE_P(v1) != IS_STRING) BAD("$numberDouble");
        bits = swm_strtod_bits(Z_STRVAL_P(v1), &ok);
        if (!ok) BAD("$numberDouble");
        swm_zval_set_double_bits(out, bits);
        return 1;
    }
    if (zend_string_equals_literal(k1, "$numberDecimal")) {
        uint64_t lo, hi;
        if (Z_TYPE_P(v1) != IS_STRING || dec128_from_string(Z_STRVAL_P(v1), Z_STRLEN_P(v1), &lo, &hi) == FAILURE) BAD("$numberDecimal");
        new_decimal(out, lo, hi);
        return 1;
    }
    if (zend_string_equals_literal(k1, "$binary")) {
        zval *b64, *st;
        zend_string *raw;
        if (Z_TYPE_P(v1) != IS_OBJECT || !(b64 = objprop(v1, "base64")) || !(st = objprop(v1, "subType"))
            || Z_TYPE_P(b64) != IS_STRING || Z_TYPE_P(st) != IS_STRING) BAD("$binary");
        raw = php_base64_decode((unsigned char *) Z_STRVAL_P(b64), Z_STRLEN_P(b64));
        if (!raw) BAD("$binary");
        swm_new_binary(out, ZSTR_VAL(raw), ZSTR_LEN(raw), ZEND_STRTOL(Z_STRVAL_P(st), NULL, 16));
        zend_string_release(raw);
        return 1;
    }
    if (zend_string_equals_literal(k1, "$uuid")) {
        uint8_t b[16];
        if (Z_TYPE_P(v1) != IS_STRING || parse_uuid(Z_STRVAL_P(v1), Z_STRLEN_P(v1), b) == FAILURE) BAD("$uuid");
        swm_new_binary(out, (char *) b, 16, 4);
        return 1;
    }
    if (zend_string_equals_literal(k1, "$timestamp")) {
        zval *t, *i;
        if (Z_TYPE_P(v1) != IS_OBJECT || !(t = objprop(v1, "t")) || !(i = objprop(v1, "i")) || Z_TYPE_P(t) != IS_LONG || Z_TYPE_P(i) != IS_LONG) BAD("$timestamp");
        swm_new_timestamp(out, (uint32_t) Z_LVAL_P(i), (uint32_t) Z_LVAL_P(t));
        return 1;
    }
    if (zend_string_equals_literal(k1, "$regularExpression")) {
        zval *p, *op;
        if (Z_TYPE_P(v1) != IS_OBJECT || !(p = objprop(v1, "pattern")) || !(op = objprop(v1, "options"))
            || Z_TYPE_P(p) != IS_STRING || Z_TYPE_P(op) != IS_STRING) BAD("$regularExpression");
        new_regex(out, Z_STRVAL_P(p), Z_STRLEN_P(p), Z_STRVAL_P(op), Z_STRLEN_P(op));
        return 1;
    }
    if (zend_string_equals_literal(k1, "$code")) {
        if (Z_TYPE_P(v1) != IS_STRING) BAD("$code");
        object_init_ex(out, swm_ce_Javascript);
        prop_set_str(Z_OBJ_P(out), 0, zend_string_copy(Z_STR_P(v1)));
        return 1;
    }
    if (zend_string_equals_literal(k1, "$symbol")) {
        if (Z_TYPE_P(v1) != IS_STRING) BAD("$symbol");
        ZVAL_COPY(out, v1);
        return 1;
    }
    if (zend_string_equals_literal(k1, "$undefined")) { ZVAL_NULL(out); return 1; }
    if (zend_string_equals_literal(k1, "$minKey")) { object_init_ex(out, swm_ce_MinKey); return 1; }
    if (zend_string_equals_literal(k1, "$maxKey")) { object_init_ex(out, swm_ce_MaxKey); return 1; }
#undef BAD
    return 0;
}

static int ej_write_value(smart_str *s, const char *k, size_t kl, zval *v, int depth, swm_error *err)
{
    if (Z_TYPE_P(v) == IS_OBJECT) {
        zval w;
        int r = ej_wrapper(v, &w, err);
        if (r < 0) return FAILURE;
        if (r > 0) {
            int rc = encode_element(s, k, kl, &w, depth, err);
            zval_ptr_dtor(&w);
            return rc;
        }
        smart_str_appendc(s, 0x03);
        smart_str_appendl(s, k, kl);
        smart_str_appendc(s, 0);
        return ej_write_doc(s, v, 0, depth + 1, err);
    }
    if (Z_TYPE_P(v) == IS_ARRAY) {
        smart_str_appendc(s, 0x04);
        smart_str_appendl(s, k, kl);
        smart_str_appendc(s, 0);
        return ej_write_doc(s, v, 1, depth + 1, err);
    }
    return encode_element(s, k, kl, v, depth, err);
}

static int ej_write_doc(smart_str *s, zval *v, zend_bool as_array, int depth, swm_error *err)
{
    size_t start = sstr_len(s);
    zend_string *k;
    zval *e;
    uint32_t i = 0;
    if (depth > SWM_BSON_MAX_DEPTH) {
        swm_error_set(err, SWM_ERR_BSON, 0, "JSON exceeds the maximum nesting depth");
        return FAILURE;
    }
    put_i32(s, 0);
    if (as_array) {
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(v), e) {
            char buf[16];
            size_t kl = (size_t) snprintf(buf, sizeof(buf), "%u", i++);
            if (ej_write_value(s, buf, kl, e, depth, err) == FAILURE) return FAILURE;
        } ZEND_HASH_FOREACH_END();
    } else {
        zend_ulong idx;
        ZEND_HASH_FOREACH_KEY_VAL_IND(Z_OBJPROP_P(v), idx, k, e) {
            char buf[32];
            const char *kp;
            size_t kl;
            if (k) { kp = ZSTR_VAL(k); kl = ZSTR_LEN(k); }
            else { kl = (size_t) snprintf(buf, sizeof(buf), ZEND_ULONG_FMT, idx); kp = buf; }
            if (memchr(kp, 0, kl)) { swm_error_set(err, SWM_ERR_BSON, 0, "BSON keys cannot contain null bytes"); return FAILURE; }
            if (ej_write_value(s, kp, kl, e, depth, err) == FAILURE) return FAILURE;
        } ZEND_HASH_FOREACH_END();
    }
    smart_str_appendc(s, 0);
    patch_i32(s, start, (int32_t) (sstr_len(s) - start));
    return SUCCESS;
}

static zend_string *json_to_bson(zend_string *json, swm_error *err)
{
    zval root;
    smart_str s = {0};
    if (php_json_decode_ex(&root, ZSTR_VAL(json), ZSTR_LEN(json), 0, PHP_JSON_PARSER_DEFAULT_DEPTH) == FAILURE) {
        if (EG(exception)) zend_clear_exception();
        swm_error_set(err, SWM_ERR_BSON, 0, "Got parse error while parsing JSON");
        return NULL;
    }
    if (Z_TYPE(root) != IS_OBJECT) {
        zval_ptr_dtor(&root);
        swm_error_set(err, SWM_ERR_BSON, 0, "Expected JSON object at the root");
        return NULL;
    }
    if (ej_write_doc(&s, &root, 0, 0, err) == FAILURE) {
        zval_ptr_dtor(&root);
        smart_str_free(&s);
        return NULL;
    }
    zval_ptr_dtor(&root);
    smart_str_0(&s);
    return s.s;
}

/* ======================================================= Document & PackedArray */
#define RAW(z) Z_STR_P(PROP(Z_OBJ_P(z), 0))
#define THROW_ERR(err) do { if (EG(exception)) swm_error_clear(&err); else swm_error_throw(&err); RETURN_THROWS(); } while (0)

PHP_METHOD(SwooleMongo_BSON_Document, fromBSON)
{
    zend_string *data;
    zval tmp;
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(data)
    ZEND_PARSE_PARAMETERS_END();
    swm_error_init(&err);
    if (swm_bson_decode(ZSTR_VAL(data), ZSTR_LEN(data), &tmp, &err) == FAILURE) THROW_ERR(err);
    zval_ptr_dtor(&tmp);
    object_init_ex(return_value, swm_ce_Document);
    prop_set_str(Z_OBJ_P(return_value), 0, zend_string_copy(data));
}

PHP_METHOD(SwooleMongo_BSON_Document, fromJSON)
{
    zend_string *json, *bytes;
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(json)
    ZEND_PARSE_PARAMETERS_END();
    swm_error_init(&err);
    if (!(bytes = json_to_bson(json, &err))) THROW_ERR(err);
    object_init_ex(return_value, swm_ce_Document);
    prop_set_str(Z_OBJ_P(return_value), 0, bytes);
}

PHP_METHOD(SwooleMongo_BSON_Document, fromPHP)
{
    zval *doc;
    smart_str s = {0};
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(doc)
    ZEND_PARSE_PARAMETERS_END();
    swm_error_init(&err);
    if (Z_TYPE_P(doc) != IS_ARRAY && Z_TYPE_P(doc) != IS_OBJECT) {
        swm_throw(swm_ce_InvalidArgumentException, "Expected value to be an array or object, %s given", zend_zval_type_name(doc));
        RETURN_THROWS();
    }
    if (swm_bson_encode(&s, doc, &err) == FAILURE) { smart_str_free(&s); THROW_ERR(err); }
    smart_str_0(&s);
    object_init_ex(return_value, swm_ce_Document);
    prop_set_str(Z_OBJ_P(return_value), 0, s.s);
}

static void raw_to_php(zval *self, zval *tm_arr, zend_bool is_array, zval *return_value)
{
    swm_typemap tm;
    swm_error err;
    zend_string *raw = RAW(self);
    int rc;
    swm_error_init(&err);
    if (swm_typemap_parse(tm_arr, &tm, &err) == FAILURE) { swm_error_throw(&err); return; }
    if (is_array) {
        swm_tm_entry root = tm.root;
        if (!tm_arr || !zend_hash_str_exists(Z_ARRVAL_P(tm_arr), ZEND_STRL("root"))) root = tm.array;
        rc = decode_tm_root(ZSTR_VAL(raw), ZSTR_LEN(raw), &tm, &root, 1, return_value, &err);
    } else {
        rc = decode_tm_root(ZSTR_VAL(raw), ZSTR_LEN(raw), &tm, &tm.root, 0, return_value, &err);
    }
    swm_typemap_dtor(&tm);
    if (rc == FAILURE) {
        if (EG(exception)) swm_error_clear(&err); else swm_error_throw(&err);
    }
}

/* decodes one level with nested values as Document / PackedArray */
static int raw_to_bson_values(zval *self, zend_bool is_array, zval *out, swm_error *err)
{
    swm_typemap tm;
    swm_tm_entry root;
    zend_string *raw = RAW(self);
    swm_typemap_init(&tm);
    tm.document.kind = SWM_TM_BSON;
    tm.array.kind = SWM_TM_BSON;
    root.kind = SWM_TM_ARRAY;
    root.ce = NULL;
    return decode_tm_root(ZSTR_VAL(raw), ZSTR_LEN(raw), &tm, &root, is_array, out, err);
}

PHP_METHOD(SwooleMongo_BSON_Document, toPHP)
{
    zval *tm = NULL;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(tm)
    ZEND_PARSE_PARAMETERS_END();
    raw_to_php(ZEND_THIS, tm, 0, return_value);
}

static void raw_get(zval *self, zval *key, zend_bool is_array, zend_bool throw_missing, zval *return_value, zend_bool exists_only)
{
    zval all, *v;
    swm_error err;
    swm_error_init(&err);
    if (raw_to_bson_values(self, is_array, &all, &err) == FAILURE) { swm_error_throw(&err); return; }
    if (Z_TYPE_P(key) == IS_LONG) v = zend_hash_index_find(Z_ARRVAL(all), Z_LVAL_P(key));
    else if (Z_TYPE_P(key) == IS_STRING) v = zend_symtable_find(Z_ARRVAL(all), Z_STR_P(key));
    else v = NULL;
    if (exists_only) {
        RETVAL_BOOL(v != NULL);
    } else if (v) {
        RETVAL_COPY(v);
    } else if (throw_missing) {
        zend_string *ks = zval_get_string(key);
        swm_throw(swm_ce_RuntimeException, is_array ? "Could not find index \"%s\" in BSON array" : "Could not find key \"%s\" in BSON document", ZSTR_VAL(ks));
        zend_string_release(ks);
    }
    zval_ptr_dtor(&all);
}

PHP_METHOD(SwooleMongo_BSON_Document, get)
{
    zval *key;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(key)
    ZEND_PARSE_PARAMETERS_END();
    raw_get(ZEND_THIS, key, 0, 1, return_value, 0);
}

PHP_METHOD(SwooleMongo_BSON_Document, has)
{
    zval *key;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(key)
    ZEND_PARSE_PARAMETERS_END();
    raw_get(ZEND_THIS, key, 0, 0, return_value, 1);
}

static void raw_iterator(zval *self, zend_bool is_array, zval *return_value)
{
    zval all, ret;
    swm_error err;
    swm_error_init(&err);
    if (raw_to_bson_values(self, is_array, &all, &err) == FAILURE) { swm_error_throw(&err); return; }
    object_init_ex(return_value, spl_ce_ArrayIterator);
    zend_call_method_with_1_params(Z_OBJ_P(return_value), spl_ce_ArrayIterator, NULL, "__construct", &ret, &all);
    zval_ptr_dtor(&ret);
    zval_ptr_dtor(&all);
}

PHP_METHOD(SwooleMongo_BSON_Document, getIterator)
{
    ZEND_PARSE_PARAMETERS_NONE();
    raw_iterator(ZEND_THIS, Z_OBJCE_P(ZEND_THIS) == swm_ce_PackedArray, return_value);
}

static void raw_ejson(zval *self, zend_bool canonical, zval *return_value)
{
    smart_str s = {0};
    zend_string *raw = RAW(self);
    ej_doc(&s, ZSTR_VAL(raw), ZSTR_LEN(raw), canonical, Z_OBJCE_P(self) == swm_ce_PackedArray);
    smart_str_0(&s);
    RETURN_STR(s.s);
}

PHP_METHOD(SwooleMongo_BSON_Document, toCanonicalExtendedJSON)
{
    ZEND_PARSE_PARAMETERS_NONE();
    raw_ejson(ZEND_THIS, 1, return_value);
}

PHP_METHOD(SwooleMongo_BSON_Document, toRelaxedExtendedJSON)
{
    ZEND_PARSE_PARAMETERS_NONE();
    raw_ejson(ZEND_THIS, 0, return_value);
}

PHP_METHOD(SwooleMongo_BSON_Document, __toString)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STR_COPY(RAW(ZEND_THIS));
}

PHP_METHOD(SwooleMongo_BSON_Document, offsetExists)
{
    zval *key;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(key)
    ZEND_PARSE_PARAMETERS_END();
    raw_get(ZEND_THIS, key, Z_OBJCE_P(ZEND_THIS) == swm_ce_PackedArray, 0, return_value, 1);
}

PHP_METHOD(SwooleMongo_BSON_Document, offsetGet)
{
    zval *key;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(key)
    ZEND_PARSE_PARAMETERS_END();
    raw_get(ZEND_THIS, key, Z_OBJCE_P(ZEND_THIS) == swm_ce_PackedArray, 1, return_value, 0);
}

PHP_METHOD(SwooleMongo_BSON_Document, offsetSet)
{
    zval *k, *v;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_ZVAL(k)
        Z_PARAM_ZVAL(v)
    ZEND_PARSE_PARAMETERS_END();
    swm_throw(swm_ce_LogicException, "Cannot write to %s property", ZSTR_VAL(Z_OBJCE_P(ZEND_THIS)->name));
}

PHP_METHOD(SwooleMongo_BSON_Document, offsetUnset)
{
    zval *k;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(k)
    ZEND_PARSE_PARAMETERS_END();
    swm_throw(swm_ce_LogicException, "Cannot unset %s property", ZSTR_VAL(Z_OBJCE_P(ZEND_THIS)->name));
}

PHP_METHOD(SwooleMongo_BSON_PackedArray, fromPHP)
{
    zval *arr;
    smart_str s = {0};
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(arr)
    ZEND_PARSE_PARAMETERS_END();
    if (!zend_array_is_list(Z_ARRVAL_P(arr))) {
        swm_throw(swm_ce_InvalidArgumentException, "Expected value to be a list, but given array is not");
        RETURN_THROWS();
    }
    swm_error_init(&err);
    if (encode_document(&s, arr, 1, 0, &err) == FAILURE) { smart_str_free(&s); THROW_ERR(err); }
    smart_str_0(&s);
    object_init_ex(return_value, swm_ce_PackedArray);
    prop_set_str(Z_OBJ_P(return_value), 0, s.s);
}

PHP_METHOD(SwooleMongo_BSON_PackedArray, toPHP)
{
    zval *tm = NULL;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(tm)
    ZEND_PARSE_PARAMETERS_END();
    raw_to_php(ZEND_THIS, tm, 1, return_value);
}

PHP_METHOD(SwooleMongo_BSON_PackedArray, get)
{
    zend_long i;
    zval k;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(i)
    ZEND_PARSE_PARAMETERS_END();
    ZVAL_LONG(&k, i);
    raw_get(ZEND_THIS, &k, 1, 1, return_value, 0);
}

PHP_METHOD(SwooleMongo_BSON_PackedArray, has)
{
    zend_long i;
    zval k;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(i)
    ZEND_PARSE_PARAMETERS_END();
    ZVAL_LONG(&k, i);
    raw_get(ZEND_THIS, &k, 1, 0, return_value, 1);
}

/* ============================================================ registration */
SWM_ARGINFO_RET(arginfo_tostring, IS_STRING)
SWM_ARGINFO_RET(arginfo_json, IS_MIXED)
SWM_ARGINFO_RET(arginfo_ret_long, IS_LONG)
SWM_ARGINFO_RET(arginfo_ret_string, IS_STRING)
SWM_ARGINFO_RET(arginfo_ret_mixed, IS_MIXED)
ZEND_BEGIN_ARG_INFO_EX(arginfo_one_opt, 0, 0, 0)
    ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(arginfo_one, 0, 0, 1)
    ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(arginfo_one_two, 0, 0, 1)
    ZEND_ARG_INFO(0, a)
    ZEND_ARG_INFO(0, b)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(arginfo_two, 0, 0, 2)
    ZEND_ARG_INFO(0, a)
    ZEND_ARG_INFO(0, b)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_isvalid, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, id, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_todatetime, 0, 0, DateTime, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_todatetimeimm, 0, 0, DateTimeImmutable, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_fromuuid, 0, 0, SwooleMongo\\BSON\\Binary, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, uuid, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_bsonserialize, 0, 0, MAY_BE_ARRAY | MAY_BE_OBJECT)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bsonunserialize, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, data, IS_ARRAY, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_doc_from, 0, 1, SwooleMongo\\BSON\\Document, 0)
    ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_arr_from, 0, 1, SwooleMongo\\BSON\\PackedArray, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_ARRAY, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_tophp, 0, 0, MAY_BE_ARRAY | MAY_BE_OBJECT)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, typeMap, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_get, 0, 1, IS_MIXED, 0)
    ZEND_ARG_INFO(0, key)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_has, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_INFO(0, key)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_getiter, 0, 0, Iterator, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_offset_exists, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, offset, IS_MIXED, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_offset_get, 0, 1, IS_MIXED, 0)
    ZEND_ARG_TYPE_INFO(0, offset, IS_MIXED, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_offset_set, 0, 2, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, offset, IS_MIXED, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_offset_unset, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, offset, IS_MIXED, 0)
ZEND_END_ARG_INFO()

#define M(cls, name, ai, flags) PHP_ME(SwooleMongo_BSON_##cls, name, ai, flags)

static const zend_function_entry oid_methods[] = {
    M(ObjectId, __construct, arginfo_one_opt, ZEND_ACC_PUBLIC)
    M(ObjectId, __toString, arginfo_tostring, ZEND_ACC_PUBLIC)
    M(ObjectId, getTimestamp, arginfo_ret_long, ZEND_ACC_PUBLIC)
    M(ObjectId, jsonSerialize, arginfo_json, ZEND_ACC_PUBLIC)
    M(ObjectId, isValid, arginfo_isvalid, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};
static const zend_function_entry utc_methods[] = {
    M(UTCDateTime, __construct, arginfo_one_opt, ZEND_ACC_PUBLIC)
    M(UTCDateTime, toDateTime, arginfo_todatetime, ZEND_ACC_PUBLIC)
    M(UTCDateTime, toDateTimeImmutable, arginfo_todatetimeimm, ZEND_ACC_PUBLIC)
    M(UTCDateTime, __toString, arginfo_tostring, ZEND_ACC_PUBLIC)
    M(UTCDateTime, jsonSerialize, arginfo_json, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry bin_methods[] = {
    M(Binary, __construct, arginfo_one_two, ZEND_ACC_PUBLIC)
    M(Binary, getData, arginfo_ret_string, ZEND_ACC_PUBLIC)
    M(Binary, getType, arginfo_ret_long, ZEND_ACC_PUBLIC)
    M(Binary, fromUuid, arginfo_fromuuid, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    M(Binary, toUuid, arginfo_ret_string, ZEND_ACC_PUBLIC)
    M(Binary, jsonSerialize, arginfo_json, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry regex_methods[] = {
    M(Regex, __construct, arginfo_one_two, ZEND_ACC_PUBLIC)
    M(Regex, getPattern, arginfo_ret_string, ZEND_ACC_PUBLIC)
    M(Regex, getFlags, arginfo_ret_string, ZEND_ACC_PUBLIC)
    M(Regex, __toString, arginfo_tostring, ZEND_ACC_PUBLIC)
    M(Regex, jsonSerialize, arginfo_json, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry ts_methods[] = {
    M(Timestamp, __construct, arginfo_two, ZEND_ACC_PUBLIC)
    M(Timestamp, getIncrement, arginfo_ret_long, ZEND_ACC_PUBLIC)
    M(Timestamp, getTimestamp, arginfo_ret_long, ZEND_ACC_PUBLIC)
    M(Timestamp, __toString, arginfo_tostring, ZEND_ACC_PUBLIC)
    M(Timestamp, jsonSerialize, arginfo_json, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry int64_methods[] = {
    M(Int64, __construct, arginfo_one, ZEND_ACC_PUBLIC)
    M(Int64, __toString, arginfo_tostring, ZEND_ACC_PUBLIC)
    M(Int64, jsonSerialize, arginfo_json, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry dec_methods[] = {
    M(Decimal128, __construct, arginfo_one, ZEND_ACC_PUBLIC)
    M(Decimal128, __toString, arginfo_tostring, ZEND_ACC_PUBLIC)
    M(Decimal128, jsonSerialize, arginfo_json, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry minkey_methods[] = {
    M(MinKey, jsonSerialize, arginfo_json, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry maxkey_methods[] = {
    M(MaxKey, jsonSerialize, arginfo_json, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry js_methods[] = {
    M(Javascript, __construct, arginfo_one_two, ZEND_ACC_PUBLIC)
    M(Javascript, getCode, arginfo_ret_string, ZEND_ACC_PUBLIC)
    M(Javascript, getScope, arginfo_ret_mixed, ZEND_ACC_PUBLIC)
    M(Javascript, __toString, arginfo_tostring, ZEND_ACC_PUBLIC)
    M(Javascript, jsonSerialize, arginfo_json, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry document_methods[] = {
    M(Document, fromBSON, arginfo_doc_from, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    M(Document, fromJSON, arginfo_doc_from, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    M(Document, fromPHP, arginfo_doc_from, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    M(Document, get, arginfo_get, ZEND_ACC_PUBLIC)
    M(Document, has, arginfo_has, ZEND_ACC_PUBLIC)
    M(Document, getIterator, arginfo_getiter, ZEND_ACC_PUBLIC)
    M(Document, toPHP, arginfo_tophp, ZEND_ACC_PUBLIC)
    M(Document, toCanonicalExtendedJSON, arginfo_ret_string, ZEND_ACC_PUBLIC)
    M(Document, toRelaxedExtendedJSON, arginfo_ret_string, ZEND_ACC_PUBLIC)
    M(Document, __toString, arginfo_tostring, ZEND_ACC_PUBLIC)
    M(Document, offsetExists, arginfo_offset_exists, ZEND_ACC_PUBLIC)
    M(Document, offsetGet, arginfo_offset_get, ZEND_ACC_PUBLIC)
    M(Document, offsetSet, arginfo_offset_set, ZEND_ACC_PUBLIC)
    M(Document, offsetUnset, arginfo_offset_unset, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry packed_methods[] = {
    M(PackedArray, fromPHP, arginfo_arr_from, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    M(PackedArray, get, arginfo_get, ZEND_ACC_PUBLIC)
    M(PackedArray, has, arginfo_has, ZEND_ACC_PUBLIC)
    M(PackedArray, toPHP, arginfo_tophp, ZEND_ACC_PUBLIC)
    PHP_MALIAS(SwooleMongo_BSON_Document, getIterator, getIterator, arginfo_getiter, ZEND_ACC_PUBLIC)
    PHP_MALIAS(SwooleMongo_BSON_Document, toCanonicalExtendedJSON, toCanonicalExtendedJSON, arginfo_ret_string, ZEND_ACC_PUBLIC)
    PHP_MALIAS(SwooleMongo_BSON_Document, toRelaxedExtendedJSON, toRelaxedExtendedJSON, arginfo_ret_string, ZEND_ACC_PUBLIC)
    PHP_MALIAS(SwooleMongo_BSON_Document, __toString, __toString, arginfo_tostring, ZEND_ACC_PUBLIC)
    PHP_MALIAS(SwooleMongo_BSON_Document, offsetExists, offsetExists, arginfo_offset_exists, ZEND_ACC_PUBLIC)
    PHP_MALIAS(SwooleMongo_BSON_Document, offsetGet, offsetGet, arginfo_offset_get, ZEND_ACC_PUBLIC)
    PHP_MALIAS(SwooleMongo_BSON_Document, offsetSet, offsetSet, arginfo_offset_set, ZEND_ACC_PUBLIC)
    PHP_MALIAS(SwooleMongo_BSON_Document, offsetUnset, offsetUnset, arginfo_offset_unset, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry serializable_methods[] = {
    ZEND_ABSTRACT_ME_WITH_FLAGS(SwooleMongo_BSON_Serializable, bsonSerialize, arginfo_bsonserialize, ZEND_ACC_PUBLIC | ZEND_ACC_ABSTRACT)
    PHP_FE_END
};
static const zend_function_entry unserializable_methods[] = {
    ZEND_ABSTRACT_ME_WITH_FLAGS(SwooleMongo_BSON_Unserializable, bsonUnserialize, arginfo_bsonunserialize, ZEND_ACC_PUBLIC | ZEND_ACC_ABSTRACT)
    PHP_FE_END
};

static zend_class_entry *reg_type(const char *name, const zend_function_entry *methods, const char **props)
{
    zend_class_entry ce, *r;
    INIT_CLASS_ENTRY_EX(ce, name, strlen(name), methods);
    r = zend_register_internal_class(&ce);
    r->ce_flags |= ZEND_ACC_FINAL;
    zend_class_implements(r, 2, swm_ce_Type, php_json_serializable_ce);
    for (int i = 0; props && props[i]; i++) {
        zend_declare_property_null(r, props[i], strlen(props[i]), ZEND_ACC_PRIVATE);
    }
    return r;
}

static zend_class_entry *reg_raw(const char *name, const zend_function_entry *methods)
{
    zend_class_entry ce, *r;
    INIT_CLASS_ENTRY_EX(ce, name, strlen(name), methods);
    r = zend_register_internal_class(&ce);
    r->ce_flags |= ZEND_ACC_FINAL;
    zend_class_implements(r, 3, zend_ce_aggregate, zend_ce_arrayaccess, swm_ce_Type);
    zend_declare_property_null(r, ZEND_STRL("data"), ZEND_ACC_PRIVATE);
    return r;
}

void swm_bson_minit(void)
{
    zend_class_entry ce;
    static const char *p_oid[] = {"oid", NULL};
    static const char *p_ms[] = {"milliseconds", NULL};
    static const char *p_bin[] = {"data", "type", NULL};
    static const char *p_re[] = {"pattern", "flags", NULL};
    static const char *p_ts[] = {"increment", "timestamp", NULL};
    static const char *p_i64[] = {"integer", NULL};
    static const char *p_dec[] = {"dec", NULL};
    static const char *p_js[] = {"code", "scope", NULL};

    swm_typemap_init(&default_tm);

    INIT_CLASS_ENTRY(ce, "SwooleMongo\\BSON\\Type", NULL);
    swm_ce_Type = zend_register_internal_interface(&ce);
    INIT_CLASS_ENTRY(ce, "SwooleMongo\\BSON\\Serializable", serializable_methods);
    swm_ce_Serializable = zend_register_internal_interface(&ce);
    zend_class_implements(swm_ce_Serializable, 1, swm_ce_Type);
    INIT_CLASS_ENTRY(ce, "SwooleMongo\\BSON\\Unserializable", unserializable_methods);
    swm_ce_Unserializable = zend_register_internal_interface(&ce);
    INIT_CLASS_ENTRY(ce, "SwooleMongo\\BSON\\Persistable", NULL);
    swm_ce_Persistable = zend_register_internal_interface(&ce);
    zend_class_implements(swm_ce_Persistable, 2, swm_ce_Serializable, swm_ce_Unserializable);

    swm_ce_ObjectId = reg_type("SwooleMongo\\BSON\\ObjectId", oid_methods, p_oid);
    swm_ce_UTCDateTime = reg_type("SwooleMongo\\BSON\\UTCDateTime", utc_methods, p_ms);
    swm_ce_Binary = reg_type("SwooleMongo\\BSON\\Binary", bin_methods, p_bin);
    swm_ce_Regex = reg_type("SwooleMongo\\BSON\\Regex", regex_methods, p_re);
    swm_ce_Timestamp = reg_type("SwooleMongo\\BSON\\Timestamp", ts_methods, p_ts);
    swm_ce_Int64 = reg_type("SwooleMongo\\BSON\\Int64", int64_methods, p_i64);
    swm_ce_Decimal128 = reg_type("SwooleMongo\\BSON\\Decimal128", dec_methods, p_dec);
    swm_ce_MinKey = reg_type("SwooleMongo\\BSON\\MinKey", minkey_methods, NULL);
    swm_ce_MaxKey = reg_type("SwooleMongo\\BSON\\MaxKey", maxkey_methods, NULL);
    swm_ce_Javascript = reg_type("SwooleMongo\\BSON\\Javascript", js_methods, p_js);
    swm_ce_Document = reg_raw("SwooleMongo\\BSON\\Document", document_methods);
    swm_ce_PackedArray = reg_raw("SwooleMongo\\BSON\\PackedArray", packed_methods);

    zend_declare_class_constant_long(swm_ce_Binary, ZEND_STRL("TYPE_GENERIC"), 0);
    zend_declare_class_constant_long(swm_ce_Binary, ZEND_STRL("TYPE_FUNCTION"), 1);
    zend_declare_class_constant_long(swm_ce_Binary, ZEND_STRL("TYPE_OLD_BINARY"), 2);
    zend_declare_class_constant_long(swm_ce_Binary, ZEND_STRL("TYPE_OLD_UUID"), 3);
    zend_declare_class_constant_long(swm_ce_Binary, ZEND_STRL("TYPE_UUID"), 4);
    zend_declare_class_constant_long(swm_ce_Binary, ZEND_STRL("TYPE_MD5"), 5);
    zend_declare_class_constant_long(swm_ce_Binary, ZEND_STRL("TYPE_ENCRYPTED"), 6);
    zend_declare_class_constant_long(swm_ce_Binary, ZEND_STRL("TYPE_COLUMN"), 7);
    zend_declare_class_constant_long(swm_ce_Binary, ZEND_STRL("TYPE_SENSITIVE"), 8);
    zend_declare_class_constant_long(swm_ce_Binary, ZEND_STRL("TYPE_VECTOR"), 9);
    zend_declare_class_constant_long(swm_ce_Binary, ZEND_STRL("TYPE_USER_DEFINED"), 128);
}
