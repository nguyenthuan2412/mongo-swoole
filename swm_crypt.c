/*
 * Client-Side Field Level Encryption and Queryable Encryption on top of libmongocrypt.
 *   - automatic encryption: driverOptions['autoEncryption'] of SwooleMongo\Driver\Manager
 *   - explicit encryption:  SwooleMongo\Driver\ClientEncryption
 *
 * libmongocrypt only does cryptography and BSON transformations; every round
 * trip it needs (listCollections, key vault queries, mongocryptd markings,
 * KMS requests) is executed here through swm_execute() / php streams, so it
 * is coroutine friendly like the rest of the driver.
 *
 * Compiled with -mgeneral-regs-only on arm64 (no floating point).
 */
#include "php_swmongo.h"

zend_class_entry *swm_ce_ClientEncryption;

#ifdef SWM_HAVE_MONGOCRYPT
#include <mongocrypt.h>
#include <spawn.h>
#include <fcntl.h>
#include <sys/wait.h>

extern char **environ;

struct _swm_crypt {
    mongocrypt_t *mc;
    swm_client *owner;       /* client whose commands are encrypted (auto) / key vault client (explicit) */
    swm_client *keyvault;    /* client used for key vault operations */
    swm_client *cryptd;      /* mongocryptd client (auto mode without crypt_shared) */
    zend_string *kv_db, *kv_coll;
    zend_bool auto_mode, bypass_auto, bypass_qa, cryptd_spawned, cryptd_bypass_spawn;
    zend_string *cryptd_uri, *cryptd_path;
    zval cryptd_args;
    zval kms_providers;      /* as configured (array) */
    zval tls_options;
    zval efc_map;            /* encryptedFieldsMap */
};

static zval *hfind(zval *arr, const char *key)
{
    if (!arr || Z_TYPE_P(arr) != IS_ARRAY) return NULL;
    return zend_hash_str_find_deref(Z_ARRVAL_P(arr), key, strlen(key));
}

/* ------------------------------------------------------------ bson glue */
static zend_string *to_bson(zval *v, swm_error *err)
{
    smart_str s = {0};
    if (swm_bson_encode(&s, v, err) == FAILURE) { smart_str_free(&s); return NULL; }
    smart_str_0(&s);
    return s.s;
}

/* user supplied filters: an empty PHP array means an empty document */
static zend_string *filter_bson(zval *v, swm_error *err)
{
    if (Z_TYPE_P(v) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(v)) == 0) {
        zval o;
        zend_string *r;
        object_init(&o);
        r = to_bson(&o, err);
        zval_ptr_dtor(&o);
        return r;
    }
    return to_bson(v, err);
}

/* Decodes a command keeping every nested value as raw BSON (Document / PackedArray) so it
 * re-encodes byte for byte; the few fields the executor inspects become internal arrays. */
static int decode_cmd(const char *data, size_t len, zval *out, swm_error *err)
{
    swm_typemap tm;
    static const char *inspect[] = {"writeConcern", "readConcern", "$readPreference", NULL};
    swm_typemap_init(&tm);
    tm.root.kind = SWM_TM_ARRAY;
    tm.document.kind = SWM_TM_BSON;
    tm.array.kind = SWM_TM_BSON;
    if (swm_bson_decode_tm(data, len, &tm, out, err) == FAILURE) return FAILURE;
    for (int i = 0; inspect[i]; i++) {
        const char *v;
        size_t vl;
        zval z;
        if (swm_bson_find(data, len, inspect[i], &v, &vl) == 0x03 && swm_bson_decode(v, vl, &z, err) == SUCCESS) {
            zend_hash_str_update(Z_ARRVAL_P(out), inspect[i], strlen(inspect[i]), &z);
        }
    }
    return SUCCESS;
}

static mongocrypt_binary_t *bin_of(zend_string *s)
{
    return mongocrypt_binary_new_from_data((uint8_t *) ZSTR_VAL(s), (uint32_t) ZSTR_LEN(s));
}

static zend_string *bin_copy(mongocrypt_binary_t *b)
{
    return zend_string_init((const char *) mongocrypt_binary_data(b), mongocrypt_binary_len(b), 0);
}

static int status_error(mongocrypt_status_t *st, swm_error *err, const char *what)
{
    uint32_t len = 0;
    const char *msg = mongocrypt_status_message(st, &len);
    swm_error_set(err, SWM_ERR_RUNTIME_CRYPT, mongocrypt_status_code(st), "%s: %.*s", what, (int) len, msg ? msg : "unknown error");
    return FAILURE;
}

static int crypt_err(mongocrypt_t *mc, swm_error *err, const char *what)
{
    mongocrypt_status_t *st = mongocrypt_status_new();
    mongocrypt_status(mc, st);
    status_error(st, err, what);
    mongocrypt_status_destroy(st);
    return FAILURE;
}

static int ctx_err(mongocrypt_ctx_t *ctx, swm_error *err, const char *what)
{
    mongocrypt_status_t *st = mongocrypt_status_new();
    mongocrypt_ctx_status(ctx, st);
    status_error(st, err, what);
    mongocrypt_status_destroy(st);
    return FAILURE;
}

/* --------------------------------------------- running commands for the state machine */
typedef int (*feed_cb)(void *arg, const char *doc, size_t len);

static int feed_ctx(void *arg, const char *doc, size_t len)
{
    mongocrypt_binary_t *b = mongocrypt_binary_new_from_data((uint8_t *) doc, (uint32_t) len);
    zend_bool ok = mongocrypt_ctx_mongo_feed((mongocrypt_ctx_t *) arg, b);
    mongocrypt_binary_destroy(b);
    return ok ? SUCCESS : FAILURE;
}

/* runs a cursor returning command and feeds every raw document */
static int run_cursor(swm_client *c, zend_string *db, zval *cmd, zend_bool write, feed_cb cb, void *arg, swm_error *err)
{
    zval reply;
    zend_string *raw = NULL, *server = NULL;
    swm_exec_opts o;
    const char *cur, *batch;
    size_t clen, blen;
    zval docs, *d, *cz, *idv;
    int64_t id;
    zend_bool ok;
    memset(&o, 0, sizeof(o));
    o.no_crypt = 1;
    o.write = write;
    if (swm_execute(c, db, cmd, &o, &reply, &raw, &server, err) == FAILURE) return FAILURE;
    for (;;) {
        if (swm_bson_find(ZSTR_VAL(raw), ZSTR_LEN(raw), "cursor", &cur, &clen) == 0x03
            && (swm_bson_find(cur, clen, "firstBatch", &batch, &blen) == 0x04 || swm_bson_find(cur, clen, "nextBatch", &batch, &blen) == 0x04)) {
            swm_bson_split(batch, blen, &docs);
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL(docs), d) {
                if (cb(arg, Z_STRVAL_P(d), Z_STRLEN_P(d)) == FAILURE) {
                    zval_ptr_dtor(&docs);
                    zval_ptr_dtor(&reply);
                    zend_string_release(raw);
                    if (server) zend_string_release(server);
                    swm_error_set(err, SWM_ERR_RUNTIME_CRYPT, 0, "libmongocrypt rejected a document");
                    return FAILURE;
                }
            } ZEND_HASH_FOREACH_END();
            zval_ptr_dtor(&docs);
        }
        cz = zend_hash_str_find(Z_ARRVAL(reply), ZEND_STRL("cursor"));
        idv = cz && Z_TYPE_P(cz) == IS_ARRAY ? zend_hash_str_find(Z_ARRVAL_P(cz), ZEND_STRL("id")) : NULL;
        id = idv ? swm_zval_to_int64(idv, &ok) : 0;
        zval_ptr_dtor(&reply);
        zend_string_release(raw);
        raw = NULL;
        if (id == 0) break;
        {
            zval gm, idz, *ns = cz ? zend_hash_str_find(Z_ARRVAL_P(cz), ZEND_STRL("ns")) : NULL;
            const char *dot = ns && Z_TYPE_P(ns) == IS_STRING ? memchr(Z_STRVAL_P(ns), '.', Z_STRLEN_P(ns)) : NULL;
            array_init(&gm);
            swm_new_int64(&idz, id);
            add_assoc_zval(&gm, "getMore", &idz);
            add_assoc_string(&gm, "collection", dot ? dot + 1 : "");
            memset(&o, 0, sizeof(o));
            o.no_crypt = 1;
            o.server = server;
            if (swm_execute(c, db, &gm, &o, &reply, &raw, NULL, err) == FAILURE) { zval_ptr_dtor(&gm); if (server) zend_string_release(server); return FAILURE; }
            zval_ptr_dtor(&gm);
        }
    }
    if (server) zend_string_release(server);
    return SUCCESS;
}

static void spawn_mongocryptd(swm_crypt *cr)
{
    pid_t pid;
    char *argv[32];
    int n = 0;
    zval *a;
    posix_spawn_file_actions_t fa;
    if (cr->cryptd_spawned || cr->cryptd_bypass_spawn) return;
    cr->cryptd_spawned = 1;
    argv[n++] = cr->cryptd_path ? ZSTR_VAL(cr->cryptd_path) : "mongocryptd";
    if (Z_TYPE(cr->cryptd_args) == IS_ARRAY) {
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(cr->cryptd_args), a) {
            if (Z_TYPE_P(a) == IS_STRING && n < 30) argv[n++] = Z_STRVAL_P(a);
        } ZEND_HASH_FOREACH_END();
    } else {
        argv[n++] = "--idleShutdownTimeoutSecs=60";
    }
    argv[n] = NULL;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ) == 0) {
        /* reap it later without blocking */
        (void) waitpid(pid, NULL, WNOHANG);
    }
    posix_spawn_file_actions_destroy(&fa);
}

static swm_client *cryptd_client(swm_crypt *cr, swm_error *err)
{
    if (!cr->cryptd) {
        zend_string *uri = cr->cryptd_uri ? zend_string_copy(cr->cryptd_uri) : zend_string_init(ZEND_STRL("mongodb://localhost:27020/?serverSelectionTimeoutMS=10000"), 0);
        spawn_mongocryptd(cr);
        cr->cryptd = swm_client_create(uri, NULL, NULL, err);
        zend_string_release(uri);
    }
    return cr->cryptd;
}

/* KMS round trips (TLS); blocking unless Swoole hooks SSL */
static int kms_request(swm_crypt *cr, mongocrypt_kms_ctx_t *kms, swm_error *err)
{
    const char *endpoint = NULL;
    mongocrypt_binary_t *msg = mongocrypt_binary_new();
    zend_string *target, *errstr = NULL;
    php_stream *stream;
    php_stream_context *ctx = NULL;
    int errcode = 0, rc = FAILURE;
    struct timeval tv = {10, 0};
    uint32_t plen = 0;
    const char *provider = mongocrypt_kms_ctx_get_kms_provider(kms, &plen);
    if (!mongocrypt_kms_ctx_endpoint(kms, &endpoint) || !mongocrypt_kms_ctx_message(kms, msg)) {
        mongocrypt_status_t *st = mongocrypt_status_new();
        mongocrypt_kms_ctx_status(kms, st);
        status_error(st, err, "KMS request");
        mongocrypt_status_destroy(st);
        goto out;
    }
    target = strchr(endpoint, ':') ? zend_strpprintf(0, "tls://%s", endpoint) : zend_strpprintf(0, "tls://%s:443", endpoint);
    {
        zval *tls = provider ? hfind(&cr->tls_options, provider) : NULL, *v;
        if (tls) {
            zval z;
            ctx = php_stream_context_alloc();
            if ((v = hfind(tls, "tlsCAFile")) && Z_TYPE_P(v) == IS_STRING) { ZVAL_COPY(&z, v); php_stream_context_set_option(ctx, "ssl", "cafile", &z); zval_ptr_dtor(&z); }
            if ((v = hfind(tls, "tlsCertificateKeyFile")) && Z_TYPE_P(v) == IS_STRING) { ZVAL_COPY(&z, v); php_stream_context_set_option(ctx, "ssl", "local_cert", &z); zval_ptr_dtor(&z); }
            if ((v = hfind(tls, "tlsCertificateKeyFilePassword")) && Z_TYPE_P(v) == IS_STRING) { ZVAL_COPY(&z, v); php_stream_context_set_option(ctx, "ssl", "passphrase", &z); zval_ptr_dtor(&z); }
        }
    }
    stream = php_stream_xport_create(ZSTR_VAL(target), ZSTR_LEN(target), 0, STREAM_XPORT_CLIENT | STREAM_XPORT_CONNECT, NULL, &tv, ctx, &errstr, &errcode);
    zend_string_release(target);
    if (!stream) {
        swm_error_set(err, SWM_ERR_NETWORK, errcode, "Failed to connect to KMS endpoint %s: %s", endpoint, errstr ? ZSTR_VAL(errstr) : "unknown");
        if (errstr) zend_string_release(errstr);
        goto out;
    }
    if (errstr) zend_string_release(errstr);
    {
        const char *p = (const char *) mongocrypt_binary_data(msg);
        size_t left = mongocrypt_binary_len(msg);
        while (left) {
            ssize_t w = php_stream_write(stream, p, left);
            if (w <= 0) { swm_error_set(err, SWM_ERR_NETWORK, 0, "KMS write failed"); php_stream_close(stream); goto out; }
            p += w;
            left -= (size_t) w;
        }
    }
    for (;;) {
        uint32_t need = mongocrypt_kms_ctx_bytes_needed(kms);
        char buf[4096];
        ssize_t r;
        mongocrypt_binary_t *in;
        if (need == 0) break;
        r = php_stream_read(stream, buf, need < sizeof(buf) ? need : sizeof(buf));
        if (r <= 0) { swm_error_set(err, SWM_ERR_NETWORK, 0, "KMS read failed"); php_stream_close(stream); goto out; }
        in = mongocrypt_binary_new_from_data((uint8_t *) buf, (uint32_t) r);
        if (!mongocrypt_kms_ctx_feed(kms, in)) {
            mongocrypt_status_t *st = mongocrypt_status_new();
            mongocrypt_kms_ctx_status(kms, st);
            status_error(st, err, "KMS response");
            mongocrypt_status_destroy(st);
            mongocrypt_binary_destroy(in);
            php_stream_close(stream);
            goto out;
        }
        mongocrypt_binary_destroy(in);
    }
    php_stream_close(stream);
    rc = SUCCESS;
out:
    mongocrypt_binary_destroy(msg);
    return rc;
}

/* credentials for providers configured as empty documents (on-demand) */
static int provide_kms_credentials(swm_crypt *cr, mongocrypt_ctx_t *ctx, swm_error *err)
{
    zval prov, *v;
    zend_string *b;
    mongocrypt_binary_t *bin;
    zend_bool ok;
    ZVAL_ARR(&prov, zend_array_dup(Z_ARRVAL(cr->kms_providers)));
    if ((v = hfind(&prov, "aws")) && Z_TYPE_P(v) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(v)) == 0) {
        const char *k = getenv("AWS_ACCESS_KEY_ID"), *s = getenv("AWS_SECRET_ACCESS_KEY"), *t = getenv("AWS_SESSION_TOKEN");
        if (k && s) {
            zval aws;
            array_init(&aws);
            add_assoc_string(&aws, "accessKeyId", (char *) k);
            add_assoc_string(&aws, "secretAccessKey", (char *) s);
            if (t && *t) add_assoc_string(&aws, "sessionToken", (char *) t);
            zend_hash_str_update(Z_ARRVAL(prov), ZEND_STRL("aws"), &aws);
        }
    }
    b = to_bson(&prov, err);
    zval_ptr_dtor(&prov);
    if (!b) return FAILURE;
    bin = bin_of(b);
    ok = mongocrypt_ctx_provide_kms_providers(ctx, bin);
    mongocrypt_binary_destroy(bin);
    zend_string_release(b);
    return ok ? SUCCESS : ctx_err(ctx, err, "provide_kms_providers");
}

/* Drives a mongocrypt context to completion; *out receives the finalized BSON */
static int run_ctx(swm_crypt *cr, mongocrypt_ctx_t *ctx, zend_string *db, zend_string **out, swm_error *err)
{
    for (;;) {
        mongocrypt_binary_t *op;
        zval cmd, opdoc;
        int rc;
        switch (mongocrypt_ctx_state(ctx)) {
            case MONGOCRYPT_CTX_ERROR:
                return ctx_err(ctx, err, "Encryption error");
            case MONGOCRYPT_CTX_NEED_MONGO_COLLINFO:
            case MONGOCRYPT_CTX_NEED_MONGO_KEYS:
            case MONGOCRYPT_CTX_NEED_MONGO_MARKINGS: {
                mongocrypt_ctx_state_t state = mongocrypt_ctx_state(ctx);
                op = mongocrypt_binary_new();
                if (!mongocrypt_ctx_mongo_op(ctx, op)) { mongocrypt_binary_destroy(op); return ctx_err(ctx, err, "mongo_op"); }
                if (state == MONGOCRYPT_CTX_NEED_MONGO_MARKINGS) {
                    if (decode_cmd((const char *) mongocrypt_binary_data(op), mongocrypt_binary_len(op), &opdoc, err) == FAILURE) {
                        mongocrypt_binary_destroy(op);
                        return FAILURE;
                    }
                } else {
                    swm_new_document_raw(&opdoc, swm_ce_Document, (const char *) mongocrypt_binary_data(op), mongocrypt_binary_len(op));
                }
                if (state == MONGOCRYPT_CTX_NEED_MONGO_MARKINGS) {
                    swm_client *cd = cryptd_client(cr, err);
                    zval reply;
                    zend_string *raw = NULL;
                    swm_exec_opts o;
                    mongocrypt_binary_t *rb;
                    mongocrypt_binary_destroy(op);
                    if (!cd) { zval_ptr_dtor(&opdoc); return FAILURE; }
                    memset(&o, 0, sizeof(o));
                    o.no_crypt = 1;
                    rc = swm_execute(cd, db, &opdoc, &o, &reply, &raw, NULL, err);
                    if (rc == FAILURE && !cr->cryptd_bypass_spawn && cr->cryptd_spawned) {
                        /* mongocryptd may still be starting: retry once */
                        swm_error_clear(err);
                        swm_sleep_us(1000000);
                        rc = swm_execute(cd, db, &opdoc, &o, &reply, &raw, NULL, err);
                    }
                    zval_ptr_dtor(&opdoc);
                    if (rc == FAILURE) return FAILURE;
                    rb = bin_of(raw);
                    if (!mongocrypt_ctx_mongo_feed(ctx, rb)) { mongocrypt_binary_destroy(rb); zend_string_release(raw); zval_ptr_dtor(&reply); return ctx_err(ctx, err, "markings"); }
                    mongocrypt_binary_destroy(rb);
                    zend_string_release(raw);
                    zval_ptr_dtor(&reply);
                } else if (state == MONGOCRYPT_CTX_NEED_MONGO_COLLINFO) {
                    mongocrypt_binary_destroy(op);
                    array_init(&cmd);
                    add_assoc_long(&cmd, "listCollections", 1);
                    add_assoc_zval(&cmd, "filter", &opdoc);
                    rc = run_cursor(cr->owner, db, &cmd, 0, feed_ctx, ctx, err);
                    zval_ptr_dtor(&cmd);
                    if (rc == FAILURE) return FAILURE;
                } else {
                    zval rc_doc;
                    mongocrypt_binary_destroy(op);
                    array_init(&cmd);
                    add_assoc_str(&cmd, "find", zend_string_copy(cr->kv_coll));
                    add_assoc_zval(&cmd, "filter", &opdoc);
                    array_init(&rc_doc);
                    add_assoc_string(&rc_doc, "level", "majority");
                    add_assoc_zval(&cmd, "readConcern", &rc_doc);
                    rc = run_cursor(cr->keyvault, cr->kv_db, &cmd, 0, feed_ctx, ctx, err);
                    zval_ptr_dtor(&cmd);
                    if (rc == FAILURE) return FAILURE;
                }
                if (!mongocrypt_ctx_mongo_done(ctx)) return ctx_err(ctx, err, "mongo_done");
                break;
            }
            case MONGOCRYPT_CTX_NEED_KMS_CREDENTIALS:
                if (provide_kms_credentials(cr, ctx, err) == FAILURE) return FAILURE;
                break;
            case MONGOCRYPT_CTX_NEED_KMS: {
                mongocrypt_kms_ctx_t *kms;
                while ((kms = mongocrypt_ctx_next_kms_ctx(ctx))) {
                    if (kms_request(cr, kms, err) == FAILURE) return FAILURE;
                }
                if (!mongocrypt_ctx_kms_done(ctx)) return ctx_err(ctx, err, "kms_done");
                break;
            }
            case MONGOCRYPT_CTX_READY: {
                mongocrypt_binary_t *res = mongocrypt_binary_new();
                if (!mongocrypt_ctx_finalize(ctx, res)) { mongocrypt_binary_destroy(res); return ctx_err(ctx, err, "finalize"); }
                *out = bin_copy(res);
                mongocrypt_binary_destroy(res);
                break;
            }
            case MONGOCRYPT_CTX_DONE:
                if (!*out) *out = ZSTR_EMPTY_ALLOC();
                return SUCCESS;
            default:
                swm_error_set(err, SWM_ERR_RUNTIME_CRYPT, 0, "Unexpected libmongocrypt state");
                return FAILURE;
        }
    }
}

/* ------------------------------------------------------------- lifecycle */
static int parse_ns(zend_string *ns, zend_string **db, zend_string **coll, swm_error *err)
{
    const char *dot = memchr(ZSTR_VAL(ns), '.', ZSTR_LEN(ns));
    if (!dot || dot == ZSTR_VAL(ns) || dot == ZSTR_VAL(ns) + ZSTR_LEN(ns) - 1) {
        swm_error_set(err, SWM_ERR_ARG, 0, "Expected keyVaultNamespace to be \"db.collection\", \"%s\" given", ZSTR_VAL(ns));
        return FAILURE;
    }
    *db = zend_string_init(ZSTR_VAL(ns), dot - ZSTR_VAL(ns), 0);
    *coll = zend_string_init(dot + 1, ZSTR_VAL(ns) + ZSTR_LEN(ns) - dot - 1, 0);
    return SUCCESS;
}

static int set_bson_opt(swm_crypt *cr, zval *v, bool (*fn)(mongocrypt_t *, mongocrypt_binary_t *), const char *what, swm_error *err)
{
    zend_string *b = to_bson(v, err);
    mongocrypt_binary_t *bin;
    zend_bool ok;
    if (!b) return FAILURE;
    bin = bin_of(b);
    ok = fn(cr->mc, bin);
    mongocrypt_binary_destroy(bin);
    zend_string_release(b);
    return ok ? SUCCESS : crypt_err(cr->mc, err, what);
}

void swm_crypt_free(swm_crypt *cr)
{
    if (!cr) return;
    if (cr->mc) mongocrypt_destroy(cr->mc);
    if (cr->keyvault && cr->keyvault != cr->owner) swm_client_release(cr->keyvault);
    if (cr->cryptd) swm_client_release(cr->cryptd);
    if (cr->kv_db) zend_string_release(cr->kv_db);
    if (cr->kv_coll) zend_string_release(cr->kv_coll);
    if (cr->cryptd_uri) zend_string_release(cr->cryptd_uri);
    if (cr->cryptd_path) zend_string_release(cr->cryptd_path);
    zval_ptr_dtor(&cr->cryptd_args);
    zval_ptr_dtor(&cr->kms_providers);
    zval_ptr_dtor(&cr->tls_options);
    zval_ptr_dtor(&cr->efc_map);
    efree(cr);
}

/* keyvault: Manager object zval or NULL (= owner) */
swm_crypt *swm_crypt_new(swm_client *owner, zval *opts, zend_bool auto_mode, swm_client *keyvault, swm_error *err)
{
    swm_crypt *cr = ecalloc(1, sizeof(*cr));
    zval *v, *extra;
    ZVAL_UNDEF(&cr->cryptd_args);
    ZVAL_UNDEF(&cr->kms_providers);
    ZVAL_UNDEF(&cr->tls_options);
    ZVAL_UNDEF(&cr->efc_map);
    cr->owner = owner;
    cr->auto_mode = auto_mode;
    cr->keyvault = keyvault ? keyvault : owner;
    if (keyvault && keyvault != owner) swm_client_addref(keyvault);
    v = hfind(opts, "keyVaultNamespace");
    if (!v || Z_TYPE_P(v) != IS_STRING) { swm_error_set(err, SWM_ERR_ARG, 0, "The \"keyVaultNamespace\" option is required"); goto fail; }
    if (parse_ns(Z_STR_P(v), &cr->kv_db, &cr->kv_coll, err) == FAILURE) goto fail;
    v = hfind(opts, "kmsProviders");
    if (!v || (Z_TYPE_P(v) != IS_ARRAY && Z_TYPE_P(v) != IS_OBJECT)) { swm_error_set(err, SWM_ERR_ARG, 0, "The \"kmsProviders\" option is required"); goto fail; }
    if (Z_TYPE_P(v) == IS_ARRAY) ZVAL_COPY(&cr->kms_providers, v);
    else { HashTable *ht = zend_get_properties_for(v, ZEND_PROP_PURPOSE_ARRAY_CAST); ZVAL_ARR(&cr->kms_providers, zend_array_dup(ht)); zend_release_properties(ht); }
    if ((v = hfind(opts, "tlsOptions")) && Z_TYPE_P(v) == IS_ARRAY) ZVAL_COPY(&cr->tls_options, v);

    cr->mc = mongocrypt_new();
    mongocrypt_setopt_use_need_kms_credentials_state(cr->mc);
    if (set_bson_opt(cr, &cr->kms_providers, mongocrypt_setopt_kms_providers, "kmsProviders", err) == FAILURE) goto fail;
    if (auto_mode) {
        if ((v = hfind(opts, "schemaMap")) && Z_TYPE_P(v) != IS_NULL && set_bson_opt(cr, v, mongocrypt_setopt_schema_map, "schemaMap", err) == FAILURE) goto fail;
        if ((v = hfind(opts, "encryptedFieldsMap")) && Z_TYPE_P(v) != IS_NULL) {
            if (set_bson_opt(cr, v, mongocrypt_setopt_encrypted_field_config_map, "encryptedFieldsMap", err) == FAILURE) goto fail;
            ZVAL_COPY(&cr->efc_map, v);
        }
        cr->bypass_auto = (v = hfind(opts, "bypassAutoEncryption")) && zend_is_true(v);
        cr->bypass_qa = (v = hfind(opts, "bypassQueryAnalysis")) && zend_is_true(v);
        if (cr->bypass_qa) mongocrypt_setopt_bypass_query_analysis(cr->mc);
        extra = hfind(opts, "extraOptions");
        if ((v = hfind(extra, "mongocryptdURI")) && Z_TYPE_P(v) == IS_STRING) cr->cryptd_uri = zend_string_copy(Z_STR_P(v));
        if ((v = hfind(extra, "mongocryptdSpawnPath")) && Z_TYPE_P(v) == IS_STRING) cr->cryptd_path = zend_string_copy(Z_STR_P(v));
        if ((v = hfind(extra, "mongocryptdSpawnArgs")) && Z_TYPE_P(v) == IS_ARRAY) ZVAL_COPY(&cr->cryptd_args, v);
        cr->cryptd_bypass_spawn = (v = hfind(extra, "mongocryptdBypassSpawn")) && zend_is_true(v);
        if ((v = hfind(extra, "cryptSharedLibPath")) && Z_TYPE_P(v) == IS_STRING) {
            mongocrypt_setopt_set_crypt_shared_lib_path_override(cr->mc, Z_STRVAL_P(v));
        } else if (!cr->bypass_auto) {
            mongocrypt_setopt_append_crypt_shared_lib_search_path(cr->mc, "$SYSTEM");
        }
    }
    if (!mongocrypt_init(cr->mc)) { crypt_err(cr->mc, err, "Unable to initialize libmongocrypt"); goto fail; }
    if (auto_mode && (v = hfind(hfind(opts, "extraOptions"), "cryptSharedLibRequired")) && zend_is_true(v)
        && mongocrypt_crypt_shared_lib_version(cr->mc) == 0) {
        swm_error_set(err, SWM_ERR_ARG, 0, "Option \"cryptSharedLibRequired\" is true, but the crypt_shared library could not be loaded");
        goto fail;
    }
    if (auto_mode && !cr->bypass_auto && !cr->bypass_qa && mongocrypt_crypt_shared_lib_version(cr->mc) == 0) {
        spawn_mongocryptd(cr);
    }
    return cr;
fail:
    swm_crypt_free(cr);
    return NULL;
}

zend_bool swm_crypt_bypass_auto(swm_crypt *cr)
{
    return cr->bypass_auto;
}

zval *swm_crypt_encrypted_fields_map(swm_crypt *cr)
{
    return Z_TYPE(cr->efc_map) == IS_UNDEF ? NULL : &cr->efc_map;
}

/* ------------------------------------------------------------- auto mode */
int swm_crypt_auto_encrypt(swm_crypt *cr, zend_string *db, zval *cmd, swm_error *err)
{
    zend_string *b, *out = NULL;
    mongocrypt_ctx_t *ctx;
    mongocrypt_binary_t *bin;
    zval res;
    int rc;
    if (cr->bypass_auto) return SUCCESS;
    if (!(b = to_bson(cmd, err))) return FAILURE;
    ctx = mongocrypt_ctx_new(cr->mc);
    bin = bin_of(b);
    if (!mongocrypt_ctx_encrypt_init(ctx, ZSTR_VAL(db), (int32_t) ZSTR_LEN(db), bin)) {
        mongocrypt_binary_destroy(bin);
        zend_string_release(b);
        ctx_err(ctx, err, "Auto encryption");
        mongocrypt_ctx_destroy(ctx);
        return FAILURE;
    }
    mongocrypt_binary_destroy(bin);
    rc = run_ctx(cr, ctx, db, &out, err);
    mongocrypt_ctx_destroy(ctx);
    zend_string_release(b);
    if (rc == FAILURE) { if (out) zend_string_release(out); return FAILURE; }
    if (ZSTR_LEN(out)) {
        if (decode_cmd(ZSTR_VAL(out), ZSTR_LEN(out), &res, err) == FAILURE) { zend_string_release(out); return FAILURE; }
        zval_ptr_dtor(cmd);
        ZVAL_COPY_VALUE(cmd, &res);
    }
    zend_string_release(out);
    return SUCCESS;
}

int swm_crypt_auto_decrypt(swm_crypt *cr, zval *reply, zend_string **raw, swm_error *err)
{
    zend_string *b, *out = NULL;
    mongocrypt_ctx_t *ctx;
    mongocrypt_binary_t *bin;
    zval res;
    int rc;
    b = raw && *raw ? zend_string_copy(*raw) : to_bson(reply, err);
    if (!b) return FAILURE;
    ctx = mongocrypt_ctx_new(cr->mc);
    bin = bin_of(b);
    if (!mongocrypt_ctx_decrypt_init(ctx, bin)) {
        mongocrypt_binary_destroy(bin);
        zend_string_release(b);
        ctx_err(ctx, err, "Auto decryption");
        mongocrypt_ctx_destroy(ctx);
        return FAILURE;
    }
    mongocrypt_binary_destroy(bin);
    rc = run_ctx(cr, ctx, NULL, &out, err);
    mongocrypt_ctx_destroy(ctx);
    zend_string_release(b);
    if (rc == FAILURE) { if (out) zend_string_release(out); return FAILURE; }
    if (ZSTR_LEN(out)) {
        if (swm_bson_decode(ZSTR_VAL(out), ZSTR_LEN(out), &res, err) == FAILURE) { zend_string_release(out); return FAILURE; }
        zval_ptr_dtor(reply);
        ZVAL_COPY_VALUE(reply, &res);
        if (raw) {
            if (*raw) zend_string_release(*raw);
            *raw = zend_string_copy(out);
        }
    }
    zend_string_release(out);
    return SUCCESS;
}

/* ===================================================== ClientEncryption class */
typedef struct {
    swm_crypt *cr;
    swm_client *keyvault;
    zend_object std;
} swm_ce_obj;

static zend_object_handlers ce_handlers;

static inline swm_ce_obj *ce_from(zend_object *o) { return (swm_ce_obj *) ((char *) o - XtOffsetOf(swm_ce_obj, std)); }

static zend_object *ce_create(zend_class_entry *ce)
{
    swm_ce_obj *o = zend_object_alloc(sizeof(swm_ce_obj), ce);
    zend_object_std_init(&o->std, ce);
    object_properties_init(&o->std, ce);
    o->std.handlers = &ce_handlers;
    return &o->std;
}

static void ce_free(zend_object *zo)
{
    swm_ce_obj *o = ce_from(zo);
    if (o->cr) swm_crypt_free(o->cr);
    if (o->keyvault) swm_client_release(o->keyvault);
    zend_object_std_dtor(zo);
}

#define THIS_CE() ce_from(Z_OBJ_P(ZEND_THIS))
#define CE_THROW(err) do { if (EG(exception)) swm_error_clear(&(err)); else swm_error_throw(&(err)); RETURN_THROWS(); } while (0)
#define ENSURE_CE(o) do { if (!(o)->cr) { swm_throw(swm_ce_LogicException, "ClientEncryption is not initialized"); RETURN_THROWS(); } } while (0)

void swm_client_encryption_init(zval *obj, swm_client *keyvault, zval *options, swm_error *err)
{
    swm_ce_obj *o = ce_from(Z_OBJ_P(obj));
    o->keyvault = keyvault;
    swm_client_addref(keyvault);
    o->cr = swm_crypt_new(keyvault, options, 0, keyvault, err);
}

PHP_METHOD(SwooleMongo_Driver_ClientEncryption, __construct)
{
    zval *options, *kv;
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(options)
    ZEND_PARSE_PARAMETERS_END();
    swm_error_init(&err);
    kv = hfind(options, "keyVaultClient");
    if (!kv || Z_TYPE_P(kv) != IS_OBJECT || Z_OBJCE_P(kv) != swm_ce_Manager) {
        swm_throw(swm_ce_InvalidArgumentException, "The \"keyVaultClient\" option is required and must be a SwooleMongo\\Driver\\Manager");
        RETURN_THROWS();
    }
    swm_client_encryption_init(ZEND_THIS, swm_manager_from(Z_OBJ_P(kv))->client, options, &err);
    if (!THIS_CE()->cr) CE_THROW(err);
}

static int kv_command(swm_ce_obj *o, zval *cmd, zend_bool write, zval *reply, swm_error *err)
{
    swm_exec_opts eo;
    memset(&eo, 0, sizeof(eo));
    eo.no_crypt = 1;
    eo.write = write;
    eo.retryable = write;
    return swm_execute(o->keyvault, o->cr->kv_db, cmd, &eo, reply, NULL, NULL, err);
}

static void majority(zval *cmd)
{
    zval wc;
    array_init(&wc);
    add_assoc_string(&wc, "w", "majority");
    add_assoc_zval(cmd, "writeConcern", &wc);
}

static zend_string *key_id_bytes(zval *key_id, swm_error *err)
{
    zend_string *d = swm_binary_data(key_id);
    if (!d || Z_LVAL_P(OBJ_PROP_NUM(Z_OBJ_P(key_id), 1)) != 4 || ZSTR_LEN(d) != 16) {
        swm_error_set(err, SWM_ERR_ARG, 0, "Expected keyId to be a UUID Binary (subtype 4)");
        return NULL;
    }
    return d;
}

PHP_METHOD(SwooleMongo_Driver_ClientEncryption, createDataKey)
{
    zend_string *provider, *b;
    zval *options = NULL, kek, *v, kd, cmd, docs, reply;
    swm_ce_obj *o = THIS_CE();
    mongocrypt_ctx_t *ctx;
    mongocrypt_binary_t *bin;
    zend_string *out = NULL;
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(provider)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_CE(o);
    swm_error_init(&err);
    ctx = mongocrypt_ctx_new(o->cr->mc);
    array_init(&kek);
    add_assoc_str(&kek, "provider", zend_string_copy(provider));
    if ((v = hfind(options, "masterKey")) && (Z_TYPE_P(v) == IS_ARRAY || Z_TYPE_P(v) == IS_OBJECT)) {
        HashTable *ht = Z_TYPE_P(v) == IS_ARRAY ? Z_ARRVAL_P(v) : Z_OBJPROP_P(v);
        zend_string *k;
        zval *mv;
        ZEND_HASH_FOREACH_STR_KEY_VAL(ht, k, mv) { if (k) { Z_TRY_ADDREF_P(mv); zend_hash_update(Z_ARRVAL(kek), k, mv); } } ZEND_HASH_FOREACH_END();
    }
    b = to_bson(&kek, &err);
    zval_ptr_dtor(&kek);
    if (!b) { mongocrypt_ctx_destroy(ctx); CE_THROW(err); }
    bin = bin_of(b);
    if (!mongocrypt_ctx_setopt_key_encryption_key(ctx, bin)) { mongocrypt_binary_destroy(bin); zend_string_release(b); ctx_err(ctx, &err, "masterKey"); mongocrypt_ctx_destroy(ctx); CE_THROW(err); }
    mongocrypt_binary_destroy(bin);
    zend_string_release(b);
    if ((v = hfind(options, "keyAltNames")) && Z_TYPE_P(v) == IS_ARRAY) {
        zval *n;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(v), n) {
            zval an;
            if (Z_TYPE_P(n) != IS_STRING) continue;
            array_init(&an);
            add_assoc_str(&an, "keyAltName", zend_string_copy(Z_STR_P(n)));
            b = to_bson(&an, &err);
            zval_ptr_dtor(&an);
            bin = bin_of(b);
            if (!mongocrypt_ctx_setopt_key_alt_name(ctx, bin)) { mongocrypt_binary_destroy(bin); zend_string_release(b); ctx_err(ctx, &err, "keyAltNames"); mongocrypt_ctx_destroy(ctx); CE_THROW(err); }
            mongocrypt_binary_destroy(bin);
            zend_string_release(b);
        } ZEND_HASH_FOREACH_END();
    }
    if ((v = hfind(options, "keyMaterial")) && Z_TYPE_P(v) == IS_OBJECT) {
        zval km;
        array_init(&km);
        Z_ADDREF_P(v);
        add_assoc_zval(&km, "keyMaterial", v);
        b = to_bson(&km, &err);
        zval_ptr_dtor(&km);
        bin = bin_of(b);
        if (!mongocrypt_ctx_setopt_key_material(ctx, bin)) { mongocrypt_binary_destroy(bin); zend_string_release(b); ctx_err(ctx, &err, "keyMaterial"); mongocrypt_ctx_destroy(ctx); CE_THROW(err); }
        mongocrypt_binary_destroy(bin);
        zend_string_release(b);
    }
    if (!mongocrypt_ctx_datakey_init(ctx) || run_ctx(o->cr, ctx, NULL, &out, &err) == FAILURE) {
        if (err.kind == SWM_ERR_NONE) ctx_err(ctx, &err, "createDataKey");
        mongocrypt_ctx_destroy(ctx);
        if (out) zend_string_release(out);
        CE_THROW(err);
    }
    mongocrypt_ctx_destroy(ctx);
    if (swm_bson_decode(ZSTR_VAL(out), ZSTR_LEN(out), &kd, &err) == FAILURE) { zend_string_release(out); CE_THROW(err); }
    array_init(&cmd);
    add_assoc_str(&cmd, "insert", zend_string_copy(o->cr->kv_coll));
    array_init(&docs);
    swm_new_document_raw(&reply, swm_ce_Document, ZSTR_VAL(out), ZSTR_LEN(out));
    add_next_index_zval(&docs, &reply);
    add_assoc_zval(&cmd, "documents", &docs);
    majority(&cmd);
    zend_string_release(out);
    if (kv_command(o, &cmd, 1, &reply, &err) == FAILURE) { zval_ptr_dtor(&cmd); zval_ptr_dtor(&kd); CE_THROW(err); }
    zval_ptr_dtor(&cmd);
    if ((v = hfind(&reply, "writeErrors")) && zend_hash_num_elements(Z_ARRVAL_P(v))) {
        zval *e = zend_hash_index_find(Z_ARRVAL_P(v), 0), *m = hfind(e, "errmsg");
        swm_error_set(&err, SWM_ERR_SERVER, zval_get_long(hfind(e, "code") ? hfind(e, "code") : &EG(uninitialized_zval)), "%s",
                      m && Z_TYPE_P(m) == IS_STRING ? Z_STRVAL_P(m) : "Inserting the data key failed");
        zval_ptr_dtor(&reply);
        zval_ptr_dtor(&kd);
        CE_THROW(err);
    }
    zval_ptr_dtor(&reply);
    RETVAL_COPY(hfind(&kd, "_id"));
    zval_ptr_dtor(&kd);
}

static int explicit_opts(mongocrypt_ctx_t *ctx, zval *options, swm_error *err)
{
    zval *v;
    if ((v = hfind(options, "keyId")) && Z_TYPE_P(v) != IS_NULL) {
        zend_string *id = key_id_bytes(v, err);
        mongocrypt_binary_t *bin;
        if (!id) return FAILURE;
        bin = bin_of(id);
        if (!mongocrypt_ctx_setopt_key_id(ctx, bin)) { mongocrypt_binary_destroy(bin); return ctx_err(ctx, err, "keyId"); }
        mongocrypt_binary_destroy(bin);
    }
    if ((v = hfind(options, "keyAltName")) && Z_TYPE_P(v) == IS_STRING) {
        zval an;
        zend_string *b;
        mongocrypt_binary_t *bin;
        array_init(&an);
        add_assoc_str(&an, "keyAltName", zend_string_copy(Z_STR_P(v)));
        b = to_bson(&an, err);
        zval_ptr_dtor(&an);
        bin = bin_of(b);
        if (!mongocrypt_ctx_setopt_key_alt_name(ctx, bin)) { mongocrypt_binary_destroy(bin); zend_string_release(b); return ctx_err(ctx, err, "keyAltName"); }
        mongocrypt_binary_destroy(bin);
        zend_string_release(b);
    }
    if ((v = hfind(options, "algorithm")) && Z_TYPE_P(v) == IS_STRING
        && !mongocrypt_ctx_setopt_algorithm(ctx, Z_STRVAL_P(v), (int) Z_STRLEN_P(v))) return ctx_err(ctx, err, "algorithm");
    if ((v = hfind(options, "contentionFactor")) && Z_TYPE_P(v) == IS_LONG
        && !mongocrypt_ctx_setopt_contention_factor(ctx, Z_LVAL_P(v))) return ctx_err(ctx, err, "contentionFactor");
    if ((v = hfind(options, "queryType")) && Z_TYPE_P(v) == IS_STRING
        && !mongocrypt_ctx_setopt_query_type(ctx, Z_STRVAL_P(v), (int) Z_STRLEN_P(v))) return ctx_err(ctx, err, "queryType");
    if ((v = hfind(options, "rangeOpts")) && Z_TYPE_P(v) == IS_ARRAY) {
        zend_string *b = to_bson(v, err);
        mongocrypt_binary_t *bin;
        if (!b) return FAILURE;
        bin = bin_of(b);
        if (!mongocrypt_ctx_setopt_algorithm_range(ctx, bin)) { mongocrypt_binary_destroy(bin); zend_string_release(b); return ctx_err(ctx, err, "rangeOpts"); }
        mongocrypt_binary_destroy(bin);
        zend_string_release(b);
    }
    return SUCCESS;
}

/* mode 0 encrypt, 1 encryptExpression, 2 decrypt */
static void explicit_op(INTERNAL_FUNCTION_PARAMETERS, int mode)
{
    zval *value, *options = NULL, wrap, res, *v;
    swm_ce_obj *o = THIS_CE();
    zend_string *b, *out = NULL;
    mongocrypt_ctx_t *ctx;
    mongocrypt_binary_t *bin;
    swm_error err;
    zend_bool ok;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ZVAL(value)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_CE(o);
    swm_error_init(&err);
    if (mode == 2 && swm_binary_data(value) == NULL) {
        swm_throw(swm_ce_InvalidArgumentException, "Expected value to decrypt to be a SwooleMongo\\BSON\\Binary");
        RETURN_THROWS();
    }
    ctx = mongocrypt_ctx_new(o->cr->mc);
    if (mode != 2 && explicit_opts(ctx, options, &err) == FAILURE) { mongocrypt_ctx_destroy(ctx); CE_THROW(err); }
    array_init(&wrap);
    Z_TRY_ADDREF_P(value);
    add_assoc_zval(&wrap, "v", value);
    b = to_bson(&wrap, &err);
    zval_ptr_dtor(&wrap);
    if (!b) { mongocrypt_ctx_destroy(ctx); CE_THROW(err); }
    bin = bin_of(b);
    ok = mode == 0 ? mongocrypt_ctx_explicit_encrypt_init(ctx, bin)
       : mode == 1 ? mongocrypt_ctx_explicit_encrypt_expression_init(ctx, bin)
       : mongocrypt_ctx_explicit_decrypt_init(ctx, bin);
    mongocrypt_binary_destroy(bin);
    if (!ok || run_ctx(o->cr, ctx, NULL, &out, &err) == FAILURE) {
        if (err.kind == SWM_ERR_NONE) ctx_err(ctx, &err, mode == 2 ? "Decryption" : "Encryption");
        mongocrypt_ctx_destroy(ctx);
        zend_string_release(b);
        if (out) zend_string_release(out);
        CE_THROW(err);
    }
    mongocrypt_ctx_destroy(ctx);
    zend_string_release(b);
    if (mode == 1) {
        swm_typemap tm;
        swm_typemap_init(&tm);
        if (swm_bson_decode_tm(ZSTR_VAL(out), ZSTR_LEN(out), &tm, &res, &err) == FAILURE) { zend_string_release(out); CE_THROW(err); }
        zend_string_release(out);
        v = zend_hash_str_find(Z_OBJPROP(res), ZEND_STRL("v"));
        if (v) RETVAL_COPY(v);
        zval_ptr_dtor(&res);
        return;
    }
    if (swm_bson_decode_tm(ZSTR_VAL(out), ZSTR_LEN(out), NULL, &res, &err) == FAILURE) { zend_string_release(out); CE_THROW(err); }
    zend_string_release(out);
    v = zend_hash_str_find(Z_OBJPROP(res), ZEND_STRL("v"));
    if (v) RETVAL_COPY(v);
    zval_ptr_dtor(&res);
}

PHP_METHOD(SwooleMongo_Driver_ClientEncryption, encrypt) { explicit_op(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0); }
PHP_METHOD(SwooleMongo_Driver_ClientEncryption, encryptExpression) { explicit_op(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1); }
PHP_METHOD(SwooleMongo_Driver_ClientEncryption, decrypt) { explicit_op(INTERNAL_FUNCTION_PARAM_PASSTHRU, 2); }

/* key vault helpers returning documents as stdClass */
static void reply_value(zval *reply, const char *field, zval *return_value)
{
    zval *v = hfind(reply, field);
    if (!v || Z_TYPE_P(v) == IS_NULL) { RETVAL_NULL(); return; }
    swm_array_to_object_doc(v, return_value);
}

static void find_and_modify(swm_ce_obj *o, zval *query, zval *update, zend_bool remove, zval *return_value)
{
    zval cmd, reply;
    swm_error err;
    swm_error_init(&err);
    array_init(&cmd);
    add_assoc_str(&cmd, "findAndModify", zend_string_copy(o->cr->kv_coll));
    add_assoc_zval(&cmd, "query", query);
    if (remove) add_assoc_bool(&cmd, "remove", 1); else add_assoc_zval(&cmd, "update", update);
    majority(&cmd);
    if (kv_command(o, &cmd, 1, &reply, &err) == FAILURE) { zval_ptr_dtor(&cmd); if (EG(exception)) swm_error_clear(&err); else swm_error_throw(&err); return; }
    zval_ptr_dtor(&cmd);
    reply_value(&reply, "value", return_value);
    zval_ptr_dtor(&reply);
}

static void id_filter(zval *key_id, zval *out)
{
    array_init(out);
    Z_ADDREF_P(key_id);
    add_assoc_zval(out, "_id", key_id);
}

PHP_METHOD(SwooleMongo_Driver_ClientEncryption, addKeyAltName)
{
    zval *key_id, q, u, set;
    zend_string *name;
    swm_ce_obj *o = THIS_CE();
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(key_id, swm_ce_Binary)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_CE(o);
    id_filter(key_id, &q);
    array_init(&u);
    array_init(&set);
    add_assoc_str(&set, "keyAltNames", zend_string_copy(name));
    add_assoc_zval(&u, "$addToSet", &set);
    find_and_modify(o, &q, &u, 0, return_value);
}

PHP_METHOD(SwooleMongo_Driver_ClientEncryption, removeKeyAltName)
{
    zval *key_id, q, u, stage, set, cond, eq, eqargs, lst, filt, fargs, ne, neargs, ifc;
    zend_string *name;
    swm_ce_obj *o = THIS_CE();
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(key_id, swm_ce_Binary)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_CE(o);
    id_filter(key_id, &q);
    /* [{$set: {keyAltNames: {$cond: [{$eq: ["$keyAltNames", [name]]}, "$$REMOVE", {$filter: {input: "$keyAltNames", cond: {$ne: ["$$this", name]}}}]}}}] */
    array_init(&eqargs); add_next_index_string(&eqargs, "$keyAltNames");
    array_init(&lst); add_next_index_str(&lst, zend_string_copy(name)); add_next_index_zval(&eqargs, &lst);
    array_init(&eq); add_assoc_zval(&eq, "$eq", &eqargs);
    array_init(&neargs); add_next_index_string(&neargs, "$$this"); add_next_index_str(&neargs, zend_string_copy(name));
    array_init(&ne); add_assoc_zval(&ne, "$ne", &neargs);
    array_init(&fargs); add_assoc_string(&fargs, "input", "$keyAltNames"); add_assoc_zval(&fargs, "cond", &ne);
    array_init(&filt); add_assoc_zval(&filt, "$filter", &fargs);
    array_init(&ifc); add_next_index_zval(&ifc, &eq); add_next_index_string(&ifc, "$$REMOVE"); add_next_index_zval(&ifc, &filt);
    array_init(&cond); add_assoc_zval(&cond, "$cond", &ifc);
    array_init(&set); add_assoc_zval(&set, "keyAltNames", &cond);
    array_init(&stage); add_assoc_zval(&stage, "$set", &set);
    array_init(&u); add_next_index_zval(&u, &stage);
    find_and_modify(o, &q, &u, 0, return_value);
}

PHP_METHOD(SwooleMongo_Driver_ClientEncryption, deleteKey)
{
    zval *key_id, cmd, dels, del, q, reply;
    swm_ce_obj *o = THIS_CE();
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(key_id, swm_ce_Binary)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_CE(o);
    swm_error_init(&err);
    array_init(&cmd);
    add_assoc_str(&cmd, "delete", zend_string_copy(o->cr->kv_coll));
    array_init(&dels);
    array_init(&del);
    id_filter(key_id, &q);
    add_assoc_zval(&del, "q", &q);
    add_assoc_long(&del, "limit", 1);
    add_next_index_zval(&dels, &del);
    add_assoc_zval(&cmd, "deletes", &dels);
    majority(&cmd);
    if (kv_command(o, &cmd, 1, &reply, &err) == FAILURE) { zval_ptr_dtor(&cmd); CE_THROW(err); }
    zval_ptr_dtor(&cmd);
    object_init(return_value);
    add_property_long(return_value, "deletedCount", zval_get_long(hfind(&reply, "n") ? hfind(&reply, "n") : &EG(uninitialized_zval)));
    zval_ptr_dtor(&reply);
}

static void kv_find(swm_ce_obj *o, zval *filter, zend_bool one, zval *return_value)
{
    zval cmd, f, *v;
    zend_string *raw = NULL;
    swm_exec_opts eo;
    swm_error err;
    zval reply;
    swm_error_init(&err);
    array_init(&cmd);
    add_assoc_str(&cmd, "find", zend_string_copy(o->cr->kv_coll));
    if (filter) { Z_ADDREF_P(filter); add_assoc_zval(&cmd, "filter", filter); }
    else { object_init(&f); add_assoc_zval(&cmd, "filter", &f); }
    if (one) { add_assoc_long(&cmd, "limit", 1); add_assoc_bool(&cmd, "singleBatch", 1); }
    {
        zval rcd;
        array_init(&rcd);
        add_assoc_string(&rcd, "level", "majority");
        add_assoc_zval(&cmd, "readConcern", &rcd);
    }
    memset(&eo, 0, sizeof(eo));
    eo.no_crypt = 1;
    eo.write = 1; /* primary */
    if (swm_execute(o->keyvault, o->cr->kv_db, &cmd, &eo, &reply, &raw, NULL, &err) == FAILURE) {
        zval_ptr_dtor(&cmd);
        if (EG(exception)) swm_error_clear(&err); else swm_error_throw(&err);
        return;
    }
    zval_ptr_dtor(&cmd);
    v = hfind(hfind(&reply, "cursor"), "firstBatch");
    if (one) {
        zval *d = v ? zend_hash_index_find(Z_ARRVAL_P(v), 0) : NULL;
        if (d) swm_array_to_object_doc(d, return_value); else RETVAL_NULL();
    } else {
        zval *d;
        array_init(return_value);
        if (v) ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(v), d) { zval obj; swm_array_to_object_doc(d, &obj); add_next_index_zval(return_value, &obj); } ZEND_HASH_FOREACH_END();
    }
    if (raw) zend_string_release(raw);
    zval_ptr_dtor(&reply);
}

PHP_METHOD(SwooleMongo_Driver_ClientEncryption, getKey)
{
    zval *key_id, q;
    swm_ce_obj *o = THIS_CE();
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(key_id, swm_ce_Binary)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_CE(o);
    id_filter(key_id, &q);
    kv_find(o, &q, 1, return_value);
    zval_ptr_dtor(&q);
}

PHP_METHOD(SwooleMongo_Driver_ClientEncryption, getKeyByAltName)
{
    zend_string *name;
    zval q;
    swm_ce_obj *o = THIS_CE();
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_CE(o);
    array_init(&q);
    add_assoc_str(&q, "keyAltNames", zend_string_copy(name));
    kv_find(o, &q, 1, return_value);
    zval_ptr_dtor(&q);
}

/* getKeys(): returns a Cursor like ext-mongodb (issued through the key vault Manager) */
PHP_METHOD(SwooleMongo_Driver_ClientEncryption, getKeys)
{
    swm_ce_obj *o = THIS_CE();
    zval mgr, ns, query, filter, qopts, rc, args[2], fn;
    ZEND_PARSE_PARAMETERS_NONE();
    ENSURE_CE(o);
    /* Manager wrapper around the key vault client */
    object_init_ex(&mgr, swm_ce_Manager);
    swm_manager_from(Z_OBJ(mgr))->client = o->keyvault;
    swm_client_addref(o->keyvault);
    object_init(&filter);
    array_init(&qopts);
    object_init_ex(&rc, swm_ce_ReadConcern);
    {
        zval lvl;
        ZVAL_STRING(&lvl, "majority");
        zend_call_method_with_1_params(Z_OBJ(rc), swm_ce_ReadConcern, NULL, "__construct", NULL, &lvl);
        zval_ptr_dtor(&lvl);
    }
    add_assoc_zval(&qopts, "readConcern", &rc);
    object_init_ex(&query, swm_ce_Query);
    zend_call_method_with_2_params(Z_OBJ(query), swm_ce_Query, NULL, "__construct", NULL, &filter, &qopts);
    ZVAL_STR(&ns, zend_strpprintf(0, "%s.%s", ZSTR_VAL(o->cr->kv_db), ZSTR_VAL(o->cr->kv_coll)));
    ZVAL_COPY_VALUE(&args[0], &ns);
    ZVAL_COPY_VALUE(&args[1], &query);
    ZVAL_STRING(&fn, "executeQuery");
    call_user_function(NULL, &mgr, &fn, return_value, 2, args);
    zval_ptr_dtor(&fn);
    zval_ptr_dtor(&ns);
    zval_ptr_dtor(&query);
    zval_ptr_dtor(&qopts);
    zval_ptr_dtor(&filter);
    zval_ptr_dtor(&mgr);
}

PHP_METHOD(SwooleMongo_Driver_ClientEncryption, rewrapManyDataKey)
{
    zval *filter, *options = NULL, *v, res, *docs, *d;
    swm_ce_obj *o = THIS_CE();
    zend_string *b, *out = NULL;
    mongocrypt_ctx_t *ctx;
    mongocrypt_binary_t *bin;
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ZVAL(filter)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_CE(o);
    swm_error_init(&err);
    ctx = mongocrypt_ctx_new(o->cr->mc);
    if ((v = hfind(options, "provider")) && Z_TYPE_P(v) == IS_STRING) {
        zval kek, *mk;
        array_init(&kek);
        add_assoc_str(&kek, "provider", zend_string_copy(Z_STR_P(v)));
        if ((mk = hfind(options, "masterKey")) && Z_TYPE_P(mk) == IS_ARRAY) {
            zend_string *k;
            zval *mv;
            ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(mk), k, mv) { if (k) { Z_TRY_ADDREF_P(mv); zend_hash_update(Z_ARRVAL(kek), k, mv); } } ZEND_HASH_FOREACH_END();
        }
        b = to_bson(&kek, &err);
        zval_ptr_dtor(&kek);
        bin = bin_of(b);
        if (!mongocrypt_ctx_setopt_key_encryption_key(ctx, bin)) { mongocrypt_binary_destroy(bin); zend_string_release(b); ctx_err(ctx, &err, "masterKey"); mongocrypt_ctx_destroy(ctx); CE_THROW(err); }
        mongocrypt_binary_destroy(bin);
        zend_string_release(b);
    }
    if (!(b = filter_bson(filter, &err))) { mongocrypt_ctx_destroy(ctx); CE_THROW(err); }
    bin = bin_of(b);
    if (!mongocrypt_ctx_rewrap_many_datakey_init(ctx, bin) || run_ctx(o->cr, ctx, NULL, &out, &err) == FAILURE) {
        if (err.kind == SWM_ERR_NONE) ctx_err(ctx, &err, "rewrapManyDataKey");
        mongocrypt_binary_destroy(bin);
        zend_string_release(b);
        mongocrypt_ctx_destroy(ctx);
        if (out) zend_string_release(out);
        CE_THROW(err);
    }
    mongocrypt_binary_destroy(bin);
    zend_string_release(b);
    mongocrypt_ctx_destroy(ctx);
    if (swm_bson_decode(ZSTR_VAL(out), ZSTR_LEN(out), &res, &err) == FAILURE) { zend_string_release(out); CE_THROW(err); }
    zend_string_release(out);
    object_init(return_value);
    docs = hfind(&res, "v");
    if (!docs || Z_TYPE_P(docs) != IS_ARRAY || zend_hash_num_elements(Z_ARRVAL_P(docs)) == 0) {
        add_property_null(return_value, "bulkWriteResult");
        zval_ptr_dtor(&res);
        return;
    }
    {
        zval cmd, ups, reply, summary;
        array_init(&cmd);
        add_assoc_str(&cmd, "update", zend_string_copy(o->cr->kv_coll));
        array_init(&ups);
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(docs), d) {
            zval up, q, u, set, cd, *idv = hfind(d, "_id");
            if (!idv) continue;
            array_init(&up);
            id_filter(idv, &q);
            add_assoc_zval(&up, "q", &q);
            array_init(&u);
            array_init(&set);
            if ((v = hfind(d, "masterKey"))) { Z_TRY_ADDREF_P(v); add_assoc_zval(&set, "masterKey", v); }
            if ((v = hfind(d, "keyMaterial"))) { Z_TRY_ADDREF_P(v); add_assoc_zval(&set, "keyMaterial", v); }
            add_assoc_zval(&u, "$set", &set);
            array_init(&cd);
            add_assoc_bool(&cd, "updateDate", 1);
            add_assoc_zval(&u, "$currentDate", &cd);
            add_assoc_zval(&up, "u", &u);
            add_next_index_zval(&ups, &up);
        } ZEND_HASH_FOREACH_END();
        add_assoc_zval(&cmd, "updates", &ups);
        majority(&cmd);
        if (kv_command(o, &cmd, 1, &reply, &err) == FAILURE) { zval_ptr_dtor(&cmd); zval_ptr_dtor(&res); zval_ptr_dtor(return_value); ZVAL_UNDEF(return_value); CE_THROW(err); }
        zval_ptr_dtor(&cmd);
        object_init(&summary);
        add_property_long(&summary, "matchedCount", zval_get_long(hfind(&reply, "n") ? hfind(&reply, "n") : &EG(uninitialized_zval)));
        add_property_long(&summary, "modifiedCount", zval_get_long(hfind(&reply, "nModified") ? hfind(&reply, "nModified") : &EG(uninitialized_zval)));
        add_property_zval(return_value, "bulkWriteResult", &summary);
        zval_ptr_dtor(&summary);
        zval_ptr_dtor(&reply);
    }
    zval_ptr_dtor(&res);
}

/* ---------------------------------------------------------- registration */
ZEND_BEGIN_ARG_INFO_EX(ai_ce_ctor, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, options, IS_ARRAY, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_ce_create, 0, 1, SwooleMongo\\BSON\\Binary, 0)
    ZEND_ARG_TYPE_INFO(0, kmsProvider, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_ce_encrypt, 0, 1, SwooleMongo\\BSON\\Binary, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_ce_expr, 0, 1, IS_OBJECT, 0)
    ZEND_ARG_TYPE_MASK(0, expr, MAY_BE_ARRAY | MAY_BE_OBJECT, NULL)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_ce_decrypt, 0, 1, IS_MIXED, 0)
    ZEND_ARG_OBJ_INFO(0, value, SwooleMongo\\BSON\\Binary, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_ce_altname, 0, 2, IS_OBJECT, 1)
    ZEND_ARG_OBJ_INFO(0, keyId, SwooleMongo\\BSON\\Binary, 0)
    ZEND_ARG_TYPE_INFO(0, keyAltName, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_ce_delete, 0, 1, IS_OBJECT, 0)
    ZEND_ARG_OBJ_INFO(0, keyId, SwooleMongo\\BSON\\Binary, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_ce_getkey, 0, 1, IS_OBJECT, 1)
    ZEND_ARG_OBJ_INFO(0, keyId, SwooleMongo\\BSON\\Binary, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_ce_getkeyalt, 0, 1, IS_OBJECT, 1)
    ZEND_ARG_TYPE_INFO(0, keyAltName, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_ce_getkeys, 0, 0, SwooleMongo\\Driver\\Cursor, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_ce_rewrap, 0, 1, IS_OBJECT, 0)
    ZEND_ARG_TYPE_MASK(0, filter, MAY_BE_ARRAY | MAY_BE_OBJECT, NULL)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

#define CME(name, ai) PHP_ME(SwooleMongo_Driver_ClientEncryption, name, ai, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
static const zend_function_entry ce_methods[] = {
    CME(__construct, ai_ce_ctor)
    CME(createDataKey, ai_ce_create)
    CME(encrypt, ai_ce_encrypt)
    CME(encryptExpression, ai_ce_expr)
    CME(decrypt, ai_ce_decrypt)
    CME(addKeyAltName, ai_ce_altname)
    CME(removeKeyAltName, ai_ce_altname)
    CME(deleteKey, ai_ce_delete)
    CME(getKey, ai_ce_getkey)
    CME(getKeyByAltName, ai_ce_getkeyalt)
    CME(getKeys, ai_ce_getkeys)
    CME(rewrapManyDataKey, ai_ce_rewrap)
    PHP_FE_END
};
#endif /* SWM_HAVE_MONGOCRYPT */

void swm_crypt_minit(void)
{
#ifdef SWM_HAVE_MONGOCRYPT
    zend_class_entry ce;
    INIT_CLASS_ENTRY(ce, "SwooleMongo\\Driver\\ClientEncryption", ce_methods);
    swm_ce_ClientEncryption = zend_register_internal_class(&ce);
    swm_ce_ClientEncryption->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;
    swm_ce_ClientEncryption->create_object = ce_create;
    memcpy(&ce_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    ce_handlers.offset = XtOffsetOf(swm_ce_obj, std);
    ce_handlers.free_obj = ce_free;
    ce_handlers.clone_obj = NULL;
#define CC(n, v) zend_declare_class_constant_string(swm_ce_ClientEncryption, ZEND_STRL(n), v)
    CC("AEAD_AES_256_CBC_HMAC_SHA_512_DETERMINISTIC", "AEAD_AES_256_CBC_HMAC_SHA_512-Deterministic");
    CC("AEAD_AES_256_CBC_HMAC_SHA_512_RANDOM", "AEAD_AES_256_CBC_HMAC_SHA_512-Random");
    CC("ALGORITHM_INDEXED", "Indexed");
    CC("ALGORITHM_UNINDEXED", "Unindexed");
    CC("ALGORITHM_RANGE", "Range");
    CC("QUERY_TYPE_EQUALITY", "equality");
    CC("QUERY_TYPE_RANGE", "range");
#undef CC
#endif
}
