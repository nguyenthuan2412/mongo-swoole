/*
 * Connection string parsing, topology discovery, server selection,
 * connection pooling, wire protocol (OP_MSG / OP_COMPRESSED),
 * authentication (SCRAM-SHA-1/256, PLAIN, X509) and command execution.
 *
 * Coroutine model: every socket is a php_stream created through
 * php_stream_xport_create(). With Swoole's runtime hooks enabled the
 * stream is a coroutine socket, so any read/write yields. Shared state
 * (topology, pools) is only mutated between I/O calls; objects that must
 * survive a yield are reference counted.
 */
#include "php_swmongo.h"
#include "ext/standard/base64.h"
#include "ext/standard/url.h"
#include "ext/standard/php_mt_rand.h"
#include <zlib.h>
#ifdef SWM_HAVE_GSSAPI
# ifdef SWM_GSSAPI_APPLE
#  include <GSS/GSS.h>
# else
#  include <gssapi/gssapi.h>
#  include <gssapi/gssapi_krb5.h>
#  ifdef SWM_HAVE_GSS_PASSWORD
#   include <gssapi/gssapi_ext.h>
#  endif
# endif
# include <netdb.h>
#endif
#include <sys/time.h>
#include <unistd.h>

#if PHP_VERSION_ID >= 80200
#include "ext/random/php_random.h"
#else
#include "ext/standard/php_random.h"
#endif

#define OP_COMPRESSED 2012
#define OP_MSG 2013
#define SWM_MAX_MESSAGE (48 * 1024 * 1024)
#define SWM_MAX_REPLY (64 * 1024 * 1024)

typedef enum {
    SRV_UNKNOWN = 0,
    SRV_STANDALONE,
    SRV_MONGOS,
    SRV_RS_PRIMARY,
    SRV_RS_SECONDARY,
    SRV_RS_ARBITER,
    SRV_RS_OTHER,
    SRV_RS_GHOST,
    SRV_LOAD_BALANCER,
} srv_type;

static const char *srv_type_names[] = {
    "Unknown", "Standalone", "Mongos", "RSPrimary", "RSSecondary", "RSArbiter", "RSOther", "RSGhost", "LoadBalancer",
};

typedef enum {
    TOPO_UNKNOWN = 0,
    TOPO_SINGLE,
    TOPO_RS_NO_PRIMARY,
    TOPO_RS_WITH_PRIMARY,
    TOPO_SHARDED,
    TOPO_LOAD_BALANCED,
} topo_type;

static const char *topo_names[] = {"Unknown", "Single", "ReplicaSetNoPrimary", "ReplicaSetWithPrimary", "Sharded", "LoadBalanced"};

typedef enum { RP_PRIMARY = 0, RP_PRIMARY_PREFERRED, RP_SECONDARY, RP_SECONDARY_PREFERRED, RP_NEAREST } rp_mode;
static const char *rp_names[] = {"primary", "primaryPreferred", "secondary", "secondaryPreferred", "nearest"};

typedef struct {
    rp_mode mode;
    zval *tags;             /* borrowed: array of tag sets */
    zend_long max_staleness;
} readpref;

typedef struct _swm_conn {
    php_stream *stream;
    swm_server *server;
    uint64_t generation;
    int64_t last_used;
    int compressor;
    zend_bool broken;
    zend_bool legacy_hello;
    zend_bool pinned;
    zend_bool more_to_come_next; /* next message is an unacknowledged write */
    int64_t deadline;            /* CSOT deadline for the next round trip (0 = none) */
} swm_conn;

struct _swm_server {
    uint32_t refcount;
    zend_string *address;
    zend_string *host;
    int port;
    zend_bool is_unix;
    zend_bool removed;
    srv_type type;
    int64_t rtt;          /* microseconds, EWMA */
    int64_t last_update;
    zend_string *set_name;
    zend_string *error;
    zend_long max_wire;
    zend_long max_bson;
    zend_long max_msg;
    zend_long max_batch;
    zend_long session_timeout; /* minutes, -1 if sessions unsupported */
    zval tags;
    zval hosts;
    zval hello;
    int64_t last_write_ms; /* lastWrite.lastWriteDate, for maxStalenessSeconds */
    swm_conn **idle;
    int idle_count;
    int idle_cap;
    int total;
    uint64_t generation;
    swm_conn *monitor;
};

struct _swm_client {
    uint32_t refcount;
    zend_bool closed;

    swm_server **servers;
    int nservers;
    int servers_cap;
    int nseeds;
    topo_type topo;
    int64_t last_scan;
    zend_bool scanning;

    zval options;
    zend_string *set_name;
    zend_bool direct;
    zend_bool load_balanced;
    zend_string *username;
    zend_string *password;
    zend_string *auth_source;
    zend_string *auth_mechanism;
    zval auth_props;            /* authMechanismProperties */
    zval oidc_callback;         /* driverOptions['oidcCallback'] */
    zend_string *oidc_token;    /* cached OIDC access token */
    zend_string *aws_key, *aws_secret, *aws_token; /* cached temporary AWS credentials */
    int64_t aws_expire;
    zend_string *default_db;
    zend_string *appname;
    /* all durations in microseconds: no floating point may live across a coroutine
     * switch (Swoole on arm64 does not preserve d8-d15, see config.m4) */
    int64_t connect_timeout;
    int64_t socket_timeout;
    int64_t sst;
    int64_t heartbeat;
    int64_t local_threshold;
    int64_t max_idle;
    int64_t wait_queue_timeout;
    zend_long max_pool;
    zend_bool retry_writes;
    zend_bool retry_reads;
    zend_bool tls;
    php_stream_context *ctx;
    zval ctx_zv;
    int compressors[3];     /* preference order: 1 snappy, 2 zlib, 3 zstd */
    int ncompressors;
    int zlib_level;
    zval read_pref;
    zval write_concern;
    zval read_concern;

    zval cluster_time;
    swm_server_session *sess_pool;
    int sess_count;
    int sess_cap;
    zend_long session_timeout;
    zval subscribers;
    zval kill_queue;

    /* SCRAM salted password cache */
    zend_string *scram_key;
    zend_string *scram_salted;

    int64_t timeout;        /* timeoutMS in microseconds (0 = disabled) */
    zend_string *api_version;
    int api_strict;         /* -1 unset, 0 false, 1 true */
    int api_deprecation;
    zval topology_id;       /* ObjectId used by SDAM events */
    zend_bool topology_opened;
    zend_long id;           /* registry id used by background coroutines */
    int pending_checks;     /* parallel heartbeat checks in flight */
    zend_bool bg_scan;      /* a background refresh is running */
    swm_crypt *crypt;       /* driverOptions['autoEncryption'] */
};

static zend_bool has_sdam(swm_client *c);
static void emit_server_event(swm_client *c, int kind, const char *method, swm_server *s);

/* ======================================================== small helpers */
static int64_t g_operation_id = 0;

static void check_hooks(swm_client *c, zend_bool is_unix)
{
    static zend_bool warned = 0;
    zend_string *n;
    zend_class_entry *ce;
    zval r;
    zval *flag;
    zend_long need;
    if (warned || !SWMG(warn_blocking) || !swm_in_coroutine()) return;
    n = zend_string_init(ZEND_STRL("swoole\\runtime"), 0);
    ce = zend_lookup_class_ex(n, NULL, ZEND_FETCH_CLASS_NO_AUTOLOAD);
    zend_string_release(n);
    if (!ce) return;
    flag = zend_get_constant_str(is_unix ? "SWOOLE_HOOK_UNIX" : (c->tls ? "SWOOLE_HOOK_SSL" : "SWOOLE_HOOK_TCP"),
                                 is_unix ? 16 : 15);
    if (!flag || Z_TYPE_P(flag) != IS_LONG) return;
    need = Z_LVAL_P(flag);
    zend_call_method(NULL, ce, NULL, ZEND_STRL("gethookflags"), &r, 0, NULL, NULL);
    if (Z_TYPE(r) == IS_LONG && !(Z_LVAL(r) & need)) {
        warned = 1;
        php_error_docref(NULL, E_WARNING,
            "swmongo is running inside a Swoole coroutine but the runtime hook for this transport is disabled; "
            "MongoDB I/O will block the whole worker. Call Swoole\\Runtime::enableCoroutine(SWOOLE_HOOK_ALL) "
            "or set 'hook_flags' => SWOOLE_HOOK_ALL");
    }
    zval_ptr_dtor(&r);
}

static inline int32_t rd_i32(const char *p)
{
    const uint8_t *b = (const uint8_t *) p;
    return (int32_t) ((uint32_t) b[0] | ((uint32_t) b[1] << 8) | ((uint32_t) b[2] << 16) | ((uint32_t) b[3] << 24));
}
static inline void put_i32(smart_str *s, int32_t v)
{
    uint32_t u = (uint32_t) v;
    char b[4] = {(char) u, (char) (u >> 8), (char) (u >> 16), (char) (u >> 24)};
    smart_str_appendl(s, b, 4);
}
static inline void patch_i32(smart_str *s, size_t at, int32_t v)
{
    uint32_t u = (uint32_t) v;
    char *b = ZSTR_VAL(s->s) + at;
    b[0] = (char) u; b[1] = (char) (u >> 8); b[2] = (char) (u >> 16); b[3] = (char) (u >> 24);
}
static inline size_t sstr_len(smart_str *s) { return s->s ? ZSTR_LEN(s->s) : 0; }

static zval *hfind(zval *arr, const char *key)
{
    if (!arr || Z_TYPE_P(arr) != IS_ARRAY) return NULL;
    return zend_hash_str_find_deref(Z_ARRVAL_P(arr), key, strlen(key));
}

static zend_bool reply_ok(zval *reply)
{
    zval *ok = hfind(reply, "ok");
    if (!ok) return 0;
    if (Z_TYPE_P(ok) == IS_DOUBLE) {
        uint64_t bits;
        memcpy(&bits, &ok->value, sizeof(bits));
        return bits == 0x3ff0000000000000ULL; /* 1.0, compared bitwise (no FPU) */
    }
    if (Z_TYPE_P(ok) == IS_LONG) return Z_LVAL_P(ok) == 1;
    if (Z_TYPE_P(ok) == IS_TRUE) return 1;
    return 0;
}

static zend_long zv_long(zval *v, zend_long def)
{
    if (!v) return def;
    if (Z_TYPE_P(v) == IS_LONG) return Z_LVAL_P(v);
    if (Z_TYPE_P(v) == IS_DOUBLE) return zval_get_long(v);
    return def;
}

static void error_from_reply(swm_error *err, zval *reply)
{
    zval *msg = hfind(reply, "errmsg"), *labels = hfind(reply, "errorLabels");
    swm_error_set(err, SWM_ERR_SERVER, zv_long(hfind(reply, "code"), 0), "%s",
                  msg && Z_TYPE_P(msg) == IS_STRING ? Z_STRVAL_P(msg) : "Command failed");
    ZVAL_COPY(&err->reply, reply);
    if (labels && Z_TYPE_P(labels) == IS_ARRAY) ZVAL_COPY(&err->labels, labels);
}

/* =================================================================== URI */
static zend_string *url_decode(const char *s, size_t len)
{
    zend_string *r = zend_string_init(s, len, 0);
    ZSTR_LEN(r) = php_raw_url_decode(ZSTR_VAL(r), ZSTR_LEN(r));
    ZSTR_VAL(r)[ZSTR_LEN(r)] = '\0';
    return r;
}

static zval *opt(swm_client *c, const char *key)
{
    return zend_hash_str_find_deref(Z_ARRVAL(c->options), key, strlen(key));
}

static int opt_bool(swm_client *c, const char *key, int def)
{
    zval *v = opt(c, key);
    if (!v) return def;
    switch (Z_TYPE_P(v)) {
        case IS_TRUE: return 1;
        case IS_FALSE: return 0;
        case IS_LONG: return Z_LVAL_P(v) != 0;
        case IS_STRING:
            if (!strcasecmp(Z_STRVAL_P(v), "true") || !strcmp(Z_STRVAL_P(v), "1") || !strcasecmp(Z_STRVAL_P(v), "yes")) return 1;
            if (!strcasecmp(Z_STRVAL_P(v), "false") || !strcmp(Z_STRVAL_P(v), "0") || !strcasecmp(Z_STRVAL_P(v), "no")) return 0;
            return def;
        default: return def;
    }
}

static zend_long opt_long(swm_client *c, const char *key, zend_long def)
{
    zval *v = opt(c, key);
    if (!v) return def;
    if (Z_TYPE_P(v) == IS_LONG) return Z_LVAL_P(v);
    if (Z_TYPE_P(v) == IS_DOUBLE) return zval_get_long(v);
    if (Z_TYPE_P(v) == IS_STRING && Z_STRLEN_P(v)) return ZEND_STRTOL(Z_STRVAL_P(v), NULL, 10);
    return def;
}

static zend_string *opt_str(swm_client *c, const char *key)
{
    zval *v = opt(c, key);
    if (!v) return NULL;
    if (Z_TYPE_P(v) == IS_STRING) return Z_STRLEN_P(v) ? zend_string_copy(Z_STR_P(v)) : NULL;
    if (Z_TYPE_P(v) == IS_LONG || Z_TYPE_P(v) == IS_DOUBLE) return zval_get_string(v);
    return NULL;
}

static void parse_tags_string(const char *s, size_t len, zval *out)
{
    const char *p = s, *end = s + len;
    array_init(out);
    while (p < end) {
        const char *comma = memchr(p, ',', end - p), *colon;
        const char *e = comma ? comma : end;
        colon = memchr(p, ':', e - p);
        if (colon) {
            zval v;
            ZVAL_STRINGL(&v, colon + 1, e - colon - 1);
            zend_symtable_str_update(Z_ARRVAL_P(out), p, colon - p, &v);
        }
        p = comma ? comma + 1 : end;
    }
}

static void set_option(swm_client *c, zend_string *key, zval *val)
{
    zend_string *lk = zend_string_tolower(key);
    if (zend_string_equals_literal(lk, "readpreferencetags")) {
        zval *list = zend_hash_find(Z_ARRVAL(c->options), lk);
        zval nl;
        if (!list) {
            array_init(&nl);
            list = zend_hash_update(Z_ARRVAL(c->options), lk, &nl);
        }
        if (Z_TYPE_P(val) == IS_STRING) {
            zval t;
            parse_tags_string(Z_STRVAL_P(val), Z_STRLEN_P(val), &t);
            add_next_index_zval(list, &t);
        } else if (Z_TYPE_P(val) == IS_ARRAY) {
            zval *tv;
            /* either a list of tag sets or a single tag set */
            if (zend_array_is_list(Z_ARRVAL_P(val))) {
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(val), tv) {
                    Z_TRY_ADDREF_P(tv);
                    add_next_index_zval(list, tv);
                } ZEND_HASH_FOREACH_END();
            } else {
                Z_TRY_ADDREF_P(val);
                add_next_index_zval(list, val);
            }
        }
    } else {
        Z_TRY_ADDREF_P(val);
        zend_hash_update(Z_ARRVAL(c->options), lk, val);
    }
    zend_string_release(lk);
}

static void parse_query(swm_client *c, const char *q, size_t len)
{
    const char *p = q, *end = q + len;
    while (p < end) {
        const char *amp = p;
        const char *eq;
        while (amp < end && *amp != '&' && *amp != ';') amp++;
        eq = memchr(p, '=', amp - p);
        if (eq) {
            zend_string *k = url_decode(p, eq - p);
            zval v;
            ZVAL_STR(&v, url_decode(eq + 1, amp - eq - 1));
            set_option(c, k, &v);
            zval_ptr_dtor(&v);
            zend_string_release(k);
        }
        p = amp + 1;
    }
}

static swm_server *server_new(zend_string *host, int port, zend_bool is_unix)
{
    swm_server *s = ecalloc(1, sizeof(*s));
    s->refcount = 1;
    s->host = zend_string_copy(host);
    s->port = port;
    s->is_unix = is_unix;
    if (is_unix) {
        s->address = zend_string_copy(host);
    } else if (memchr(ZSTR_VAL(host), ':', ZSTR_LEN(host))) {
        s->address = zend_strpprintf(0, "[%s]:%d", ZSTR_VAL(host), port);
    } else {
        s->address = zend_strpprintf(0, "%s:%d", ZSTR_VAL(host), port);
    }
    s->type = SRV_UNKNOWN;
    s->max_bson = 16 * 1024 * 1024;
    s->max_msg = SWM_MAX_MESSAGE;
    s->max_batch = 100000;
    s->session_timeout = -1;
    ZVAL_UNDEF(&s->tags);
    ZVAL_UNDEF(&s->hosts);
    ZVAL_UNDEF(&s->hello);
    return s;
}

static void conn_close(swm_conn *conn);

static void server_clear_pool(swm_server *s)
{
    s->generation++;
    while (s->idle_count > 0) {
        conn_close(s->idle[--s->idle_count]);
        s->total--;
    }
}

static void server_addref(swm_server *s) { s->refcount++; }

static void server_release(swm_server *s)
{
    if (--s->refcount > 0) return;
    while (s->idle_count > 0) conn_close(s->idle[--s->idle_count]);
    if (s->monitor) conn_close(s->monitor);
    if (s->idle) efree(s->idle);
    zend_string_release(s->address);
    zend_string_release(s->host);
    if (s->set_name) zend_string_release(s->set_name);
    if (s->error) zend_string_release(s->error);
    zval_ptr_dtor(&s->tags);
    zval_ptr_dtor(&s->hosts);
    zval_ptr_dtor(&s->hello);
    efree(s);
}

static swm_server *find_server(swm_client *c, zend_string *address)
{
    for (int i = 0; i < c->nservers; i++) {
        if (!c->servers[i]->removed && zend_string_equals_ci(c->servers[i]->address, address)) return c->servers[i];
    }
    return NULL;
}

static int parse_host(const char *h, size_t len, zend_string **host, int *port, zend_bool *is_unix)
{
    zend_string *dec = url_decode(h, len);
    const char *s = ZSTR_VAL(dec);
    size_t l = ZSTR_LEN(dec);
    *port = 27017;
    *is_unix = 0;
    if (l == 0) { zend_string_release(dec); return FAILURE; }
    if (l > 5 && strcmp(s + l - 5, ".sock") == 0) {
        *is_unix = 1;
        *port = 0;
        *host = dec;
        return SUCCESS;
    }
    if (s[0] == '[') {
        const char *rb = memchr(s, ']', l);
        if (!rb) { zend_string_release(dec); return FAILURE; }
        *host = zend_string_init(s + 1, rb - s - 1, 0);
        if (rb + 1 < s + l) {
            if (rb[1] != ':') { zend_string_release(*host); zend_string_release(dec); return FAILURE; }
            *port = atoi(rb + 2);
        }
    } else {
        const char *colon = zend_memrchr(s, ':', l);
        if (colon) {
            *host = zend_string_init(s, colon - s, 0);
            *port = atoi(colon + 1);
        } else {
            *host = zend_string_init(s, l, 0);
        }
    }
    zend_string_release(dec);
    if (*port <= 0 || *port > 65535 || ZSTR_LEN(*host) == 0) { zend_string_release(*host); return FAILURE; }
    {
        zend_string *lower = zend_string_tolower(*host);
        zend_string_release(*host);
        *host = lower;
    }
    return SUCCESS;
}

static void add_server(swm_client *c, zend_string *host, int port, zend_bool is_unix)
{
    swm_server *s = server_new(host, port, is_unix);
    if (find_server(c, s->address)) {
        server_release(s);
        return;
    }
    if (c->nservers == c->servers_cap) {
        c->servers_cap = c->servers_cap ? c->servers_cap * 2 : 4;
        c->servers = erealloc(c->servers, sizeof(swm_server *) * c->servers_cap);
    }
    c->servers[c->nservers++] = s;
    if (c->topology_opened && has_sdam(c)) emit_server_event(c, 1, "serveropening", s);
}

static void add_server_address(swm_client *c, const char *addr, size_t len)
{
    zend_string *host;
    int port;
    zend_bool is_unix;
    if (parse_host(addr, len, &host, &port, &is_unix) == SUCCESS) {
        add_server(c, host, port, is_unix);
        zend_string_release(host);
    }
}

static zend_long dns_const(const char *name)
{
    zval *v = zend_get_constant_str(name, strlen(name));
    return v && Z_TYPE_P(v) == IS_LONG ? Z_LVAL_P(v) : 0;
}

static int resolve_srv(swm_client *c, zend_string *host, swm_error *err)
{
    zval args[2], ret, *rec;
    const char *dot = memchr(ZSTR_VAL(host), '.', ZSTR_LEN(host));
    const char *parent;
    int found = 0;
    if (!dot || !memchr(dot + 1, '.', ZSTR_VAL(host) + ZSTR_LEN(host) - dot - 1)) {
        swm_error_set(err, SWM_ERR_ARG, 0, "mongodb+srv host \"%s\" must have at least three domain parts", ZSTR_VAL(host));
        return FAILURE;
    }
    parent = dot; /* includes leading '.' */
    {
        zend_string *svc = opt_str(c, "srvservicename");
        ZVAL_STR(&args[0], zend_strpprintf(0, "_%s._tcp.%s", svc ? ZSTR_VAL(svc) : "mongodb", ZSTR_VAL(host)));
        if (svc) zend_string_release(svc);
    }
    ZVAL_LONG(&args[1], dns_const("DNS_SRV"));
    if (swm_call_function("dns_get_record", &ret, 2, args) == FAILURE || Z_TYPE(ret) != IS_ARRAY) {
        zval_ptr_dtor(&args[0]);
        zval_ptr_dtor(&ret);
        if (EG(exception)) zend_clear_exception();
        swm_error_set(err, SWM_ERR_NETWORK, 0, "Failed to look up SRV record for \"%s\"", ZSTR_VAL(host));
        return FAILURE;
    }
    zval_ptr_dtor(&args[0]);
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(ret), rec) {
        zval *target = hfind(rec, "target"), *port = hfind(rec, "port");
        size_t tl, pl = strlen(parent);
        if (!target || Z_TYPE_P(target) != IS_STRING) continue;
        tl = Z_STRLEN_P(target);
        if (Z_STRVAL_P(target)[tl - 1] == '.') tl--;
        if (tl <= pl || strncasecmp(Z_STRVAL_P(target) + tl - pl, parent, pl) != 0) continue; /* foreign domain */
        {
            zend_string *h = zend_string_init(Z_STRVAL_P(target), tl, 0);
            zend_string *lh = zend_string_tolower(h);
            add_server(c, lh, (int) zv_long(port, 27017), 0);
            zend_string_release(h);
            zend_string_release(lh);
            found++;
        }
    } ZEND_HASH_FOREACH_END();
    zval_ptr_dtor(&ret);
    if (!found) {
        swm_error_set(err, SWM_ERR_NETWORK, 0, "SRV lookup for \"%s\" returned no usable hosts", ZSTR_VAL(host));
        return FAILURE;
    }
    {
        zend_long maxh = opt_long(c, "srvmaxhosts", 0);
        if (maxh > 0 && opt(c, "replicaset")) {
            swm_error_set(err, SWM_ERR_ARG, 0, "srvMaxHosts cannot be combined with replicaSet");
            return FAILURE;
        }
        if (maxh > 0 && c->nservers > maxh) {
            for (int i = c->nservers - 1; i > 0; i--) {
                int j = (int) php_mt_rand_range(0, i);
                swm_server *t = c->servers[i];
                c->servers[i] = c->servers[j];
                c->servers[j] = t;
            }
            while (c->nservers > maxh) server_release(c->servers[--c->nservers]);
        }
    }
    /* TXT record may carry authSource / replicaSet / loadBalanced */
    ZVAL_STR_COPY(&args[0], host);
    ZVAL_LONG(&args[1], dns_const("DNS_TXT"));
    if (swm_call_function("dns_get_record", &ret, 2, args) == SUCCESS && Z_TYPE(ret) == IS_ARRAY) {
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(ret), rec) {
            zval *txt = hfind(rec, "txt");
            if (txt && Z_TYPE_P(txt) == IS_STRING) {
                zval saved;
                ZVAL_COPY(&saved, &c->options);
                SEPARATE_ARRAY(&c->options);
                parse_query(c, Z_STRVAL_P(txt), Z_STRLEN_P(txt));
                /* explicit URI options win over TXT options */
                zend_hash_merge(Z_ARRVAL(c->options), Z_ARRVAL(saved), zval_add_ref, 1);
                zval_ptr_dtor(&saved);
            }
        } ZEND_HASH_FOREACH_END();
    }
    if (EG(exception)) zend_clear_exception();
    zval_ptr_dtor(&args[0]);
    zval_ptr_dtor(&ret);
    if (!opt(c, "tls") && !opt(c, "ssl")) {
        zval t;
        ZVAL_TRUE(&t);
        zend_hash_str_update(Z_ARRVAL(c->options), ZEND_STRL("tls"), &t);
    }
    return SUCCESS;
}

static int parse_read_pref(zval *v, readpref *rp, swm_error *err);

static int interpret_options(swm_client *c, zend_bool srv, swm_error *err)
{
    zend_string *s;
    zend_long l;
    zval *v;

    c->connect_timeout = opt_long(c, "connecttimeoutms", 10000) * 1000;
    c->socket_timeout = opt_long(c, "sockettimeoutms", 0) * 1000;
    c->sst = opt_long(c, "serverselectiontimeoutms", 30000) * 1000;
    c->heartbeat = opt_long(c, "heartbeatfrequencyms", 10000) * 1000;
    if (c->heartbeat < 500000) c->heartbeat = 500000;
    c->local_threshold = opt_long(c, "localthresholdms", 15) * 1000;
    c->max_idle = opt_long(c, "maxidletimems", 0) * 1000;
    c->wait_queue_timeout = opt_long(c, "waitqueuetimeoutms", 0) * 1000;
    if (c->wait_queue_timeout <= 0) c->wait_queue_timeout = c->sst;
    c->max_pool = opt_long(c, "maxpoolsize", 100);
    c->timeout = opt_long(c, "timeoutms", 0) * 1000;
    c->retry_writes = opt_bool(c, "retrywrites", 1);
    c->retry_reads = opt_bool(c, "retryreads", 1);
    c->direct = opt_bool(c, "directconnection", 0);
    c->set_name = opt_str(c, "replicaset");
    c->appname = opt_str(c, "appname");
    c->auth_mechanism = opt_str(c, "authmechanism");
    c->tls = opt_bool(c, "tls", 0) || opt_bool(c, "ssl", 0);

    c->load_balanced = opt_bool(c, "loadbalanced", 0);
    if (c->load_balanced && (c->nseeds > 1 || c->direct || opt(c, "replicaset"))) {
        swm_error_set(err, SWM_ERR_ARG, 0, "loadBalanced=true requires a single host and is incompatible with directConnection and replicaSet");
        return FAILURE;
    }
    if (c->direct && c->nseeds > 1) {
        swm_error_set(err, SWM_ERR_ARG, 0, "directConnection=true requires exactly one host");
        return FAILURE;
    }
    if (c->direct && srv) {
        swm_error_set(err, SWM_ERR_ARG, 0, "directConnection=true is incompatible with mongodb+srv");
        return FAILURE;
    }

    /* credentials */
    if ((s = opt_str(c, "username"))) { if (c->username) zend_string_release(c->username); c->username = s; }
    if ((s = opt_str(c, "password"))) { if (c->password) zend_string_release(c->password); c->password = s; }
    c->auth_source = opt_str(c, "authsource");
    if (!c->auth_source) {
        if (c->auth_mechanism && (zend_string_equals_literal_ci(c->auth_mechanism, "PLAIN")
                                  || zend_string_equals_literal_ci(c->auth_mechanism, "MONGODB-X509")
                                  || zend_string_equals_literal_ci(c->auth_mechanism, "MONGODB-AWS")
                                  || zend_string_equals_literal_ci(c->auth_mechanism, "MONGODB-OIDC")
                                  || zend_string_equals_literal_ci(c->auth_mechanism, "GSSAPI"))) {
            c->auth_source = zend_string_init(ZEND_STRL("$external"), 0);
        } else if (c->default_db) {
            c->auth_source = zend_string_copy(c->default_db);
        } else {
            c->auth_source = zend_string_init(ZEND_STRL("admin"), 0);
        }
    }
    if (c->auth_mechanism) {
        const char *m = ZSTR_VAL(c->auth_mechanism);
        if (strcasecmp(m, "SCRAM-SHA-1") && strcasecmp(m, "SCRAM-SHA-256") && strcasecmp(m, "PLAIN") && strcasecmp(m, "MONGODB-X509")
            && strcasecmp(m, "MONGODB-AWS") && strcasecmp(m, "MONGODB-OIDC") && strcasecmp(m, "GSSAPI")) {
            swm_error_set(err, SWM_ERR_ARG, 0, "Unsupported authMechanism \"%s\"", m);
            return FAILURE;
        }
    }

    /* authMechanismProperties: "KEY:VALUE,KEY:VALUE" or array */
    array_init(&c->auth_props);
    if ((v = opt(c, "authmechanismproperties"))) {
        if (Z_TYPE_P(v) == IS_ARRAY) {
            zend_string *k;
            zval *pv;
            ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(v), k, pv) {
                if (k) { Z_TRY_ADDREF_P(pv); zend_hash_update(Z_ARRVAL(c->auth_props), k, pv); }
            } ZEND_HASH_FOREACH_END();
        } else if (Z_TYPE_P(v) == IS_STRING) {
            parse_tags_string(Z_STRVAL_P(v), Z_STRLEN_P(v), &c->auth_props);
        }
    }
#ifndef SWM_HAVE_GSSAPI
    if (c->auth_mechanism && zend_string_equals_literal_ci(c->auth_mechanism, "GSSAPI")) {
        swm_error_set(err, SWM_ERR_ARG, 0, "GSSAPI (Kerberos) support was not compiled in; rebuild swmongo with --with-swmongo-gssapi");
        return FAILURE;
    }
#endif
    if (c->auth_mechanism && zend_string_equals_literal_ci(c->auth_mechanism, "GSSAPI") && !c->username) {
        swm_error_set(err, SWM_ERR_ARG, 0, "GSSAPI requires a username (the Kerberos principal)");
        return FAILURE;
    }
    if (c->auth_mechanism && zend_string_equals_literal_ci(c->auth_mechanism, "MONGODB-OIDC")) {
        zval *env = hfind(&c->auth_props, "ENVIRONMENT");
        if (c->password) { swm_error_set(err, SWM_ERR_ARG, 0, "MONGODB-OIDC does not accept a password"); return FAILURE; }
        if (!env && Z_TYPE(c->oidc_callback) == IS_UNDEF) {
            swm_error_set(err, SWM_ERR_ARG, 0, "MONGODB-OIDC requires authMechanismProperties ENVIRONMENT or the \"oidcCallback\" driver option");
            return FAILURE;
        }
        if (env && Z_TYPE_P(env) == IS_STRING && (!strcmp(Z_STRVAL_P(env), "azure") || !strcmp(Z_STRVAL_P(env), "gcp")) && !hfind(&c->auth_props, "TOKEN_RESOURCE")) {
            swm_error_set(err, SWM_ERR_ARG, 0, "MONGODB-OIDC ENVIRONMENT:%s requires TOKEN_RESOURCE", Z_STRVAL_P(env));
            return FAILURE;
        }
    }

    /* compression */
    if ((s = opt_str(c, "compressors"))) {
        const char *p = ZSTR_VAL(s), *e = p + ZSTR_LEN(s);
        while (p < e && c->ncompressors < 3) {
            const char *comma = memchr(p, ',', e - p);
            size_t l = (comma ? comma : e) - p;
            int id = 0;
            if (l == 4 && !strncasecmp(p, "zlib", 4)) id = 2;
            else if (l == 4 && !strncasecmp(p, "zstd", 4) && zend_hash_str_exists(EG(function_table), ZEND_STRL("zstd_compress"))) id = 3;
            else if (l == 6 && !strncasecmp(p, "snappy", 6) && zend_hash_str_exists(EG(function_table), ZEND_STRL("snappy_compress"))) id = 1;
            if (id) c->compressors[c->ncompressors++] = id;
            p = comma ? comma + 1 : e;
        }
        zend_string_release(s);
    }
    c->zlib_level = (int) opt_long(c, "zlibcompressionlevel", -1);
    if (c->zlib_level < -1 || c->zlib_level > 9) c->zlib_level = -1;

    /* TLS stream context */
    if (c->tls) {
        zval z;
        c->ctx = php_stream_context_alloc();
        ZVAL_RES(&c->ctx_zv, c->ctx->res);
        Z_ADDREF(c->ctx_zv);
        if ((s = opt_str(c, "tlscafile"))) {
            ZVAL_STR(&z, s); php_stream_context_set_option(c->ctx, "ssl", "cafile", &z); zval_ptr_dtor(&z);
        }
        if ((s = opt_str(c, "tlscertificatekeyfile"))) {
            ZVAL_STR(&z, s); php_stream_context_set_option(c->ctx, "ssl", "local_cert", &z); zval_ptr_dtor(&z);
        }
        if ((s = opt_str(c, "tlscertificatekeyfilepassword"))) {
            ZVAL_STR(&z, s); php_stream_context_set_option(c->ctx, "ssl", "passphrase", &z); zval_ptr_dtor(&z);
        }
        if (opt_bool(c, "tlsinsecure", 0) || opt_bool(c, "tlsallowinvalidcertificates", 0)) {
            ZVAL_FALSE(&z);
            php_stream_context_set_option(c->ctx, "ssl", "verify_peer", &z);
            php_stream_context_set_option(c->ctx, "ssl", "verify_peer_name", &z);
            ZVAL_TRUE(&z);
            php_stream_context_set_option(c->ctx, "ssl", "allow_self_signed", &z);
        } else if (opt_bool(c, "tlsallowinvalidhostnames", 0)) {
            ZVAL_FALSE(&z);
            php_stream_context_set_option(c->ctx, "ssl", "verify_peer_name", &z);
        }
    } else {
        ZVAL_UNDEF(&c->ctx_zv);
    }

    /* read preference */
    array_init(&c->read_pref);
    if ((s = opt_str(c, "readpreference"))) {
        readpref rp;
        zval tmp;
        ZVAL_STR(&tmp, s);
        if (parse_read_pref(&tmp, &rp, err) == FAILURE) { zval_ptr_dtor(&tmp); return FAILURE; }
        add_assoc_zval(&c->read_pref, "mode", &tmp);
    } else {
        add_assoc_string(&c->read_pref, "mode", "primary");
    }
    if ((v = opt(c, "readpreferencetags")) && Z_TYPE_P(v) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(v))) {
        Z_ADDREF_P(v);
        add_assoc_zval(&c->read_pref, "tags", v);
    }
    if ((l = opt_long(c, "maxstalenessseconds", -1)) > 0) add_assoc_long(&c->read_pref, "maxStalenessSeconds", l);
    {
        readpref rp;
        if (parse_read_pref(&c->read_pref, &rp, err) == FAILURE) return FAILURE;
    }

    /* write concern */
    ZVAL_UNDEF(&c->write_concern);
    if (opt(c, "w") || opt(c, "journal") || opt(c, "wtimeoutms")) {
        array_init(&c->write_concern);
        if ((v = opt(c, "w"))) {
            if (Z_TYPE_P(v) == IS_STRING && is_numeric_string(Z_STRVAL_P(v), Z_STRLEN_P(v), &l, NULL, 0) == IS_LONG) {
                add_assoc_long(&c->write_concern, "w", l);
            } else {
                Z_TRY_ADDREF_P(v);
                add_assoc_zval(&c->write_concern, "w", v);
            }
        }
        if (opt(c, "journal")) add_assoc_bool(&c->write_concern, "j", opt_bool(c, "journal", 0));
        if ((l = opt_long(c, "wtimeoutms", 0)) > 0) add_assoc_long(&c->write_concern, "wtimeout", l);
    }
    ZVAL_UNDEF(&c->read_concern);
    if ((s = opt_str(c, "readconcernlevel"))) {
        array_init(&c->read_concern);
        add_assoc_str(&c->read_concern, "level", s);
    }
    return SUCCESS;
}

static int parse_uri(swm_client *c, zend_string *uri, zval *uri_options, swm_error *err)
{
    const char *s = ZSTR_VAL(uri), *end = ZSTR_VAL(uri) + ZSTR_LEN(uri), *hostend, *at = NULL, *hp;
    zend_bool srv = 0;

    if (ZSTR_LEN(uri) >= 10 && strncmp(s, "mongodb://", 10) == 0) {
        s += 10;
    } else if (ZSTR_LEN(uri) >= 14 && strncmp(s, "mongodb+srv://", 14) == 0) {
        s += 14;
        srv = 1;
    } else {
        swm_error_set(err, SWM_ERR_ARG, 0, "Invalid URI scheme, expected mongodb:// or mongodb+srv://");
        return FAILURE;
    }
    hostend = s;
    while (hostend < end && *hostend != '/' && *hostend != '?') hostend++;
    for (const char *p = s; p < hostend; p++) if (*p == '@') at = p;
    if (at) {
        const char *colon = memchr(s, ':', at - s);
        c->username = url_decode(s, (colon ? colon : at) - s);
        if (colon) c->password = url_decode(colon + 1, at - colon - 1);
        hp = at + 1;
    } else {
        hp = s;
    }
    if (hostend < end && *hostend == '/') {
        const char *q = memchr(hostend + 1, '?', end - hostend - 1);
        const char *dbend = q ? q : end;
        if (dbend > hostend + 1) c->default_db = url_decode(hostend + 1, dbend - hostend - 1);
        if (q) parse_query(c, q + 1, end - q - 1);
    } else if (hostend < end && *hostend == '?') {
        parse_query(c, hostend + 1, end - hostend - 1);
    }
    if (uri_options) {
        zend_string *k;
        zval *v;
        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(uri_options), k, v) {
            if (k) set_option(c, k, v);
        } ZEND_HASH_FOREACH_END();
    }

    /* hosts */
    if (srv) {
        zend_string *host;
        int port;
        zend_bool is_unix;
        if (memchr(hp, ',', hostend - hp) || memchr(hp, ':', hostend - hp)) {
            swm_error_set(err, SWM_ERR_ARG, 0, "mongodb+srv URI must contain exactly one host and no port");
            return FAILURE;
        }
        if (parse_host(hp, hostend - hp, &host, &port, &is_unix) == FAILURE) {
            swm_error_set(err, SWM_ERR_ARG, 0, "Invalid host in URI");
            return FAILURE;
        }
        if (resolve_srv(c, host, err) == FAILURE) { zend_string_release(host); return FAILURE; }
        zend_string_release(host);
        c->nseeds = c->nservers;
    } else {
        const char *p = hp;
        while (p < hostend) {
            const char *comma = memchr(p, ',', hostend - p);
            const char *e = comma ? comma : hostend;
            zend_string *host;
            int port;
            zend_bool is_unix;
            if (parse_host(p, e - p, &host, &port, &is_unix) == FAILURE) {
                swm_error_set(err, SWM_ERR_ARG, 0, "Invalid host \"%.*s\" in URI", (int) (e - p), p);
                return FAILURE;
            }
            add_server(c, host, port, is_unix);
            zend_string_release(host);
            c->nseeds++;
            p = comma ? comma + 1 : hostend;
        }
    }
    if (c->nservers == 0) {
        swm_error_set(err, SWM_ERR_ARG, 0, "URI must contain at least one host");
        return FAILURE;
    }
    return interpret_options(c, srv, err);
}

/* ============================================================ read prefs */
static int parse_read_pref(zval *v, readpref *rp, swm_error *err)
{
    zval *mode = NULL;
    rp->mode = RP_PRIMARY;
    rp->tags = NULL;
    rp->max_staleness = -1;
    if (!v || Z_TYPE_P(v) == IS_NULL) return SUCCESS;
    if (Z_TYPE_P(v) == IS_OBJECT && Z_OBJCE_P(v) == swm_ce_ReadPreference) {
        zval *t = OBJ_PROP_NUM(Z_OBJ_P(v), 1);
        mode = OBJ_PROP_NUM(Z_OBJ_P(v), 0);
        if (Z_TYPE_P(t) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(t))) rp->tags = t;
        rp->max_staleness = Z_TYPE_P(OBJ_PROP_NUM(Z_OBJ_P(v), 2)) == IS_LONG ? Z_LVAL_P(OBJ_PROP_NUM(Z_OBJ_P(v), 2)) : -1;
    } else if (Z_TYPE_P(v) == IS_STRING) {
        mode = v;
    } else if (Z_TYPE_P(v) == IS_ARRAY) {
        zval *t;
        mode = hfind(v, "mode");
        t = hfind(v, "tags");
        if (t && Z_TYPE_P(t) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(t))) rp->tags = t;
        rp->max_staleness = zv_long(hfind(v, "maxStalenessSeconds"), -1);
    } else {
        swm_error_set(err, SWM_ERR_ARG, 0, "readPreference must be a string or an array");
        return FAILURE;
    }
    if (mode) {
        int found = 0;
        if (Z_TYPE_P(mode) != IS_STRING) {
            swm_error_set(err, SWM_ERR_ARG, 0, "readPreference mode must be a string");
            return FAILURE;
        }
        for (int i = 0; i < 5; i++) {
            if (strcasecmp(Z_STRVAL_P(mode), rp_names[i]) == 0) { rp->mode = (rp_mode) i; found = 1; break; }
        }
        if (!found) {
            swm_error_set(err, SWM_ERR_ARG, 0, "Invalid readPreference mode \"%s\"", Z_STRVAL_P(mode));
            return FAILURE;
        }
    }
    if (rp->mode == RP_PRIMARY && (rp->tags || rp->max_staleness > 0)) {
        swm_error_set(err, SWM_ERR_ARG, 0, "Primary read preference mode conflicts with tags or maxStalenessSeconds");
        return FAILURE;
    }
    return SUCCESS;
}

static void read_pref_doc(readpref *rp, zval *out)
{
    array_init(out);
    add_assoc_string(out, "mode", rp_names[rp->mode]);
    if (rp->tags) {
        Z_ADDREF_P(rp->tags);
        add_assoc_zval(out, "tags", rp->tags);
    }
    if (rp->max_staleness > 0) add_assoc_long(out, "maxStalenessSeconds", rp->max_staleness);
}

/* ===================================================== wire protocol I/O */
static int32_t next_request_id(void)
{
    static int32_t id = 0;
    if (++id <= 0) id = 1;
    return id;
}

static void conn_close(swm_conn *conn)
{
    if (conn->stream) {
        php_stream_close(conn->stream);
        conn->stream = NULL;
    }
    if (conn->server) server_release(conn->server);
    efree(conn);
}

static void io_error(swm_conn *conn, const char *what, swm_error *err)
{
    zval meta;
    zend_bool timed_out = 0;
    conn->broken = 1;
    array_init(&meta);
    if (php_stream_set_option(conn->stream, PHP_STREAM_OPTION_META_DATA_API, 0, &meta) == PHP_STREAM_OPTION_RETURN_OK) {
        zval *t = hfind(&meta, "timed_out");
        timed_out = t && Z_TYPE_P(t) == IS_TRUE;
    }
    zval_ptr_dtor(&meta);
    if (timed_out) {
        swm_error_set(err, SWM_ERR_TIMEOUT, 0, "Socket timed out while %s %s", what, ZSTR_VAL(conn->server->address));
    } else {
        swm_error_set(err, SWM_ERR_NETWORK, 0, "Connection to %s closed while %s", ZSTR_VAL(conn->server->address), what);
    }
}

static int write_all(swm_conn *conn, const char *buf, size_t len, swm_error *err)
{
    while (len > 0) {
        ssize_t n = php_stream_write(conn->stream, buf, len);
        if (n <= 0) { io_error(conn, "writing to", err); return FAILURE; }
        buf += n;
        len -= (size_t) n;
    }
    return SUCCESS;
}

static int read_all(swm_conn *conn, char *buf, size_t len, swm_error *err)
{
    while (len > 0) {
        ssize_t n = php_stream_read(conn->stream, buf, len);
        if (n <= 0) { io_error(conn, "reading from", err); return FAILURE; }
        buf += n;
        len -= (size_t) n;
    }
    return SUCCESS;
}

static zend_bool compressible(zval *cmd)
{
    static const char *never[] = {"hello", "ismaster", "saslstart", "saslcontinue", "getnonce", "authenticate",
                                  "createuser", "updateuser", "copydbsaslstart", "copydbgetnonce", "copydb", NULL};
    zend_string *k = NULL;
    zend_ulong idx;
    zval *v;
    ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(cmd), idx, k, v) { (void) idx; (void) v; break; } ZEND_HASH_FOREACH_END();
    if (!k) return 0;
    for (int i = 0; never[i]; i++) if (strcasecmp(ZSTR_VAL(k), never[i]) == 0) return 0;
    return 1;
}

static int encode_sequences(smart_str *body, zval *sequences, swm_error *err)
{
    zend_string *id;
    zval *docs, *d;
    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(sequences), id, docs) {
        size_t start;
        if (!id || Z_TYPE_P(docs) != IS_ARRAY) {
            swm_error_set(err, SWM_ERR_ARG, 0, "sequences must map an identifier to a list of documents");
            return FAILURE;
        }
        smart_str_appendc(body, 1);
        start = sstr_len(body);
        put_i32(body, 0);
        smart_str_append(body, id);
        smart_str_appendc(body, '\0');
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(docs), d) {
            ZVAL_DEREF(d);
            if (Z_TYPE_P(d) == IS_STRING) {
                if (Z_STRLEN_P(d) < 5 || (size_t) rd_i32(Z_STRVAL_P(d)) != Z_STRLEN_P(d)) {
                    swm_error_set(err, SWM_ERR_BSON, 0, "Pre-encoded document in sequence \"%s\" is not valid BSON", ZSTR_VAL(id));
                    return FAILURE;
                }
                smart_str_append(body, Z_STR_P(d));
            } else if (swm_bson_encode(body, d, err) == FAILURE) {
                return FAILURE;
            }
        } ZEND_HASH_FOREACH_END();
        patch_i32(body, start, (int32_t) (sstr_len(body) - start));
    } ZEND_HASH_FOREACH_END();
    return SUCCESS;
}

/* Sends cmd (an array we own; "$db" is added) and reads the reply document. */
static int conn_command(swm_client *c, swm_conn *conn, const char *db, zval *cmd, zval *sequences,
                        zval *reply, zend_string **raw, zend_bool allow_compress, swm_error *err)
{
    smart_str body = {0}, msg = {0};
    int32_t req_id = next_request_id(), mlen, opcode;
    char hdr[16];
    zend_string *payload = NULL;
    const char *p;
    size_t plen, pos, end;
    uint32_t flags;
    zval doc;

    zend_bool more_to_come = conn->more_to_come_next;
    int64_t deadline = conn->deadline;
    conn->more_to_come_next = 0;
    conn->deadline = 0;
    ZVAL_UNDEF(reply);
    add_assoc_string(cmd, "$db", (char *) db);
    put_i32(&body, more_to_come ? 2 : 0);
    smart_str_appendc(&body, 0);
    if (swm_bson_encode(&body, cmd, err) == FAILURE) { smart_str_free(&body); return FAILURE; }
    if (sequences && Z_TYPE_P(sequences) == IS_ARRAY && encode_sequences(&body, sequences, err) == FAILURE) {
        smart_str_free(&body);
        return FAILURE;
    }
    if (sstr_len(&body) + 16 > (size_t) conn->server->max_msg) {
        swm_error_set(err, SWM_ERR_ARG, 0, "Message size %zu exceeds maxMessageSizeBytes %ld",
                      sstr_len(&body) + 16, (long) conn->server->max_msg);
        smart_str_free(&body);
        return FAILURE;
    }
    if (allow_compress && conn->compressor && compressible(cmd)) {
        zend_string *cz = NULL;
        if (conn->compressor == 2) {
            uLongf clen = compressBound((uLong) ZSTR_LEN(body.s));
            cz = zend_string_alloc(clen, 0);
            if (compress2((Bytef *) ZSTR_VAL(cz), &clen, (Bytef *) ZSTR_VAL(body.s), (uLong) ZSTR_LEN(body.s), c->zlib_level) != Z_OK) {
                zend_string_efree(cz);
                cz = NULL;
            } else {
                ZSTR_LEN(cz) = clen;
            }
        } else {
            zval a, r;
            ZVAL_STR_COPY(&a, body.s);
            if (swm_call_function(conn->compressor == 3 ? "zstd_compress" : "snappy_compress", &r, 1, &a) == SUCCESS && Z_TYPE(r) == IS_STRING) {
                cz = Z_STR(r);
            } else {
                zval_ptr_dtor(&r);
                if (EG(exception)) zend_clear_exception();
            }
            zval_ptr_dtor(&a);
        }
        if (!cz) {
            smart_str_free(&body);
            swm_error_set(err, SWM_ERR_PROTOCOL, 0, "Message compression failed");
            return FAILURE;
        }
        put_i32(&msg, (int32_t) (16 + 9 + ZSTR_LEN(cz)));
        put_i32(&msg, req_id);
        put_i32(&msg, 0);
        put_i32(&msg, OP_COMPRESSED);
        put_i32(&msg, OP_MSG);
        put_i32(&msg, (int32_t) ZSTR_LEN(body.s));
        smart_str_appendc(&msg, (char) conn->compressor);
        smart_str_append(&msg, cz);
        zend_string_release(cz);
    } else {
        put_i32(&msg, (int32_t) (16 + ZSTR_LEN(body.s)));
        put_i32(&msg, req_id);
        put_i32(&msg, 0);
        put_i32(&msg, OP_MSG);
        smart_str_append(&msg, body.s);
    }
    smart_str_free(&body);

    if (deadline) {
        int64_t rem = deadline - swm_now_us();
        struct timeval tv;
        if (rem <= 0) {
            smart_str_free(&msg);
            swm_error_set(err, SWM_ERR_SERVER, 50, "Operation exceeded the timeoutMS limit");
            return FAILURE;
        }
        tv.tv_sec = (time_t) (rem / 1000000);
        tv.tv_usec = (suseconds_t) (rem % 1000000);
        php_stream_set_option(conn->stream, PHP_STREAM_OPTION_READ_TIMEOUT, 0, &tv);
    }
    if (write_all(conn, ZSTR_VAL(msg.s), ZSTR_LEN(msg.s), err) == FAILURE) { smart_str_free(&msg); return FAILURE; }
    smart_str_free(&msg);
    if (more_to_come) {
        /* OP_MSG moreToCome: the server sends no reply (w:0) */
        array_init(reply);
        add_assoc_long(reply, "ok", 1);
        return SUCCESS;
    }

    if (read_all(conn, hdr, 16, err) == FAILURE) {
        if (deadline && swm_now_us() >= deadline) swm_error_set(err, SWM_ERR_SERVER, 50, "Operation exceeded the timeoutMS limit");
        return FAILURE;
    }
    mlen = rd_i32(hdr);
    opcode = rd_i32(hdr + 12);
    if (mlen < 21 || mlen > SWM_MAX_REPLY || rd_i32(hdr + 8) != req_id) {
        conn->broken = 1;
        swm_error_set(err, SWM_ERR_PROTOCOL, 0, "Invalid reply header from %s", ZSTR_VAL(conn->server->address));
        return FAILURE;
    }
    payload = zend_string_alloc((size_t) mlen - 16, 0);
    if (read_all(conn, ZSTR_VAL(payload), (size_t) mlen - 16, err) == FAILURE) {
        zend_string_release(payload);
        return FAILURE;
    }
    if (opcode == OP_COMPRESSED) {
        int32_t orig, usize;
        uint8_t cid;
        zend_string *u;
        if (ZSTR_LEN(payload) < 9) goto proto;
        orig = rd_i32(ZSTR_VAL(payload));
        usize = rd_i32(ZSTR_VAL(payload) + 4);
        cid = (uint8_t) ZSTR_VAL(payload)[8];
        if (usize < 5 || usize > SWM_MAX_REPLY) goto proto;
        u = zend_string_alloc((size_t) usize, 0);
        if (cid == 2) {
            uLongf dl = (uLongf) usize;
            if (uncompress((Bytef *) ZSTR_VAL(u), &dl, (Bytef *) ZSTR_VAL(payload) + 9, (uLong) ZSTR_LEN(payload) - 9) != Z_OK
                || dl != (uLongf) usize) {
                zend_string_release(u);
                goto proto;
            }
        } else if (cid == 0 && ZSTR_LEN(payload) - 9 == (size_t) usize) {
            memcpy(ZSTR_VAL(u), ZSTR_VAL(payload) + 9, usize);
        } else if (cid == 1 || cid == 3) {
            zval a, r;
            int ok = 0;
            ZVAL_STRINGL(&a, ZSTR_VAL(payload) + 9, ZSTR_LEN(payload) - 9);
            if (swm_call_function(cid == 3 ? "zstd_uncompress" : "snappy_uncompress", &r, 1, &a) == SUCCESS
                && Z_TYPE(r) == IS_STRING && Z_STRLEN(r) == (size_t) usize) {
                memcpy(ZSTR_VAL(u), Z_STRVAL(r), usize);
                ok = 1;
            }
            zval_ptr_dtor(&r);
            zval_ptr_dtor(&a);
            if (EG(exception)) zend_clear_exception();
            if (!ok) { zend_string_release(u); goto proto; }
        } else {
            zend_string_release(u);
            goto proto;
        }
        zend_string_release(payload);
        payload = u;
        opcode = orig;
    }
    if (opcode != OP_MSG) goto proto;
    p = ZSTR_VAL(payload);
    plen = ZSTR_LEN(payload);
    flags = (uint32_t) rd_i32(p);
    end = plen - ((flags & 1) ? 4 : 0);
    pos = 4;
    ZVAL_UNDEF(&doc);
    while (pos < end) {
        uint8_t kind = (uint8_t) p[pos++];
        if (kind == 0) {
            size_t used;
            if (Z_TYPE(doc) != IS_UNDEF) zval_ptr_dtor(&doc);
            if (swm_bson_decode_one(p + pos, end - pos, &used, &doc, err) == FAILURE) {
                conn->broken = 1;
                zend_string_release(payload);
                return FAILURE;
            }
            if (raw) {
                if (*raw) zend_string_release(*raw);
                *raw = zend_string_init(p + pos, used, 0);
            }
            pos += used;
        } else if (kind == 1) {
            int32_t sz;
            if (end - pos < 4) break;
            sz = rd_i32(p + pos);
            if (sz < 4 || (size_t) sz > end - pos) break;
            pos += (size_t) sz;
        } else {
            break;
        }
    }
    zend_string_release(payload);
    if (Z_TYPE(doc) == IS_UNDEF) {
        conn->broken = 1;
        swm_error_set(err, SWM_ERR_PROTOCOL, 0, "Reply from %s contains no document", ZSTR_VAL(conn->server->address));
        return FAILURE;
    }
    ZVAL_COPY_VALUE(reply, &doc);
    if (deadline) {
        struct timeval tv;
        if (c->socket_timeout > 0) { tv.tv_sec = (time_t) (c->socket_timeout / 1000000); tv.tv_usec = (suseconds_t) (c->socket_timeout % 1000000); }
        else { tv.tv_sec = -1; tv.tv_usec = 0; }
        php_stream_set_option(conn->stream, PHP_STREAM_OPTION_READ_TIMEOUT, 0, &tv);
    }
    return SUCCESS;
proto:
    zend_string_release(payload);
    conn->broken = 1;
    swm_error_set(err, SWM_ERR_PROTOCOL, 0, "Malformed reply from %s", ZSTR_VAL(conn->server->address));
    return FAILURE;
}

/* ======================================================= authentication */
static zend_string *call_str(const char *fn, int argc, zval *args)
{
    zval r;
    if (swm_call_function(fn, &r, argc, args) == FAILURE || Z_TYPE(r) != IS_STRING) {
        zval_ptr_dtor(&r);
        return NULL;
    }
    return Z_STR(r);
}

static zend_string *hmac(const char *alg, zend_string *key, const char *data, size_t dlen)
{
    zval a[4];
    zend_string *r;
    ZVAL_STRING(&a[0], alg);
    ZVAL_STRINGL(&a[1], data, dlen);
    ZVAL_STR_COPY(&a[2], key);
    ZVAL_TRUE(&a[3]);
    r = call_str("hash_hmac", 4, a);
    zval_ptr_dtor(&a[0]); zval_ptr_dtor(&a[1]); zval_ptr_dtor(&a[2]);
    return r;
}

static zend_string *hash_raw(const char *alg, zend_string *data)
{
    zval a[3];
    zend_string *r;
    ZVAL_STRING(&a[0], alg);
    ZVAL_STR_COPY(&a[1], data);
    ZVAL_TRUE(&a[2]);
    r = call_str("hash", 3, a);
    zval_ptr_dtor(&a[0]); zval_ptr_dtor(&a[1]);
    return r;
}

static zend_string *sasl_name(zend_string *u)
{
    smart_str s = {0};
    for (size_t i = 0; i < ZSTR_LEN(u); i++) {
        char ch = ZSTR_VAL(u)[i];
        if (ch == '=') smart_str_appendl(&s, "=3D", 3);
        else if (ch == ',') smart_str_appendl(&s, "=2C", 3);
        else smart_str_appendc(&s, ch);
    }
    smart_str_0(&s);
    return s.s ? s.s : ZSTR_EMPTY_ALLOC();
}

static zend_string *payload_of(zval *reply)
{
    zval *p = hfind(reply, "payload");
    zend_string *d;
    if (!p) return NULL;
    d = swm_binary_data(p);
    return d;
}

static const char *scram_field(zend_string *msg, char name, size_t *len)
{
    const char *p = ZSTR_VAL(msg), *end = p + ZSTR_LEN(msg);
    while (p < end) {
        const char *comma = memchr(p, ',', end - p);
        const char *e = comma ? comma : end;
        if (e - p >= 2 && p[0] == name && p[1] == '=') {
            *len = (size_t) (e - p - 2);
            return p + 2;
        }
        p = comma ? comma + 1 : end;
    }
    return NULL;
}

static int sasl_command(swm_client *c, swm_conn *conn, zval *cmd, zval *reply, swm_error *err)
{
    if (conn_command(c, conn, ZSTR_VAL(c->auth_source), cmd, NULL, reply, NULL, 0, err) == FAILURE) return FAILURE;
    if (!reply_ok(reply)) {
        zval *m = hfind(reply, "errmsg");
        swm_error_set(err, SWM_ERR_AUTH, zv_long(hfind(reply, "code"), 0), "Authentication failed: %s",
                      m && Z_TYPE_P(m) == IS_STRING ? Z_STRVAL_P(m) : "unknown error");
        zval_ptr_dtor(reply);
        ZVAL_UNDEF(reply);
        return FAILURE;
    }
    return SUCCESS;
}

static zend_string *saslprep(zend_string *in, swm_error *err);

static int auth_scram(swm_client *c, swm_conn *conn, zend_bool sha256, swm_error *err)
{
    const char *alg = sha256 ? "sha256" : "sha1";
    const char *mech = sha256 ? "SCRAM-SHA-256" : "SCRAM-SHA-1";
    uint8_t nb[24];
    zend_string *nonce, *uname, *first_bare, *server_first = NULL, *password = NULL, *salt = NULL;
    zend_string *salted = NULL, *client_key = NULL, *stored = NULL, *client_sig = NULL, *server_key = NULL, *server_sig = NULL;
    zend_string *auth_msg = NULL, *without_proof = NULL, *proof_b64 = NULL, *final_msg = NULL, *expected = NULL;
    zval cmd, reply, payload, conv, opts;
    const char *r, *s, *i;
    size_t rl, sl, il;
    long iters;
    int rc = FAILURE;

    ZVAL_UNDEF(&reply);
    ZVAL_UNDEF(&conv);
    php_random_bytes_silent(nb, sizeof(nb));
    nonce = php_base64_encode(nb, sizeof(nb));
    uname = sasl_name(c->username);
    first_bare = zend_strpprintf(0, "n=%s,r=%s", ZSTR_VAL(uname), ZSTR_VAL(nonce));

    array_init(&cmd);
    add_assoc_long(&cmd, "saslStart", 1);
    add_assoc_string(&cmd, "mechanism", (char *) mech);
    {
        zend_string *first = zend_strpprintf(0, "n,,%s", ZSTR_VAL(first_bare));
        swm_new_binary(&payload, ZSTR_VAL(first), ZSTR_LEN(first), 0);
        zend_string_release(first);
    }
    add_assoc_zval(&cmd, "payload", &payload);
    add_assoc_long(&cmd, "autoAuthorize", 1);
    array_init(&opts);
    add_assoc_bool(&opts, "skipEmptyExchange", 1);
    add_assoc_zval(&cmd, "options", &opts);
    rc = sasl_command(c, conn, &cmd, &reply, err);
    zval_ptr_dtor(&cmd);
    if (rc == FAILURE) goto done;
    rc = FAILURE;

    {
        zend_string *pl = payload_of(&reply);
        zval *cid = hfind(&reply, "conversationId");
        if (!pl || !cid) { swm_error_set(err, SWM_ERR_AUTH, 0, "Invalid SCRAM server response"); goto done; }
        server_first = zend_string_copy(pl);
        ZVAL_COPY(&conv, cid);
    }
    r = scram_field(server_first, 'r', &rl);
    s = scram_field(server_first, 's', &sl);
    i = scram_field(server_first, 'i', &il);
    if (!r || !s || !i || rl < ZSTR_LEN(nonce) || strncmp(r, ZSTR_VAL(nonce), ZSTR_LEN(nonce)) != 0) {
        swm_error_set(err, SWM_ERR_AUTH, 0, "Invalid SCRAM server nonce");
        goto done;
    }
    iters = strtol(i, NULL, 10);
    if (iters < 4096) { swm_error_set(err, SWM_ERR_AUTH, 0, "SCRAM iteration count too low"); goto done; }

    if (sha256) {
        password = saslprep(c->password, err);
        if (!password) goto done;
    } else {
        zval a;
        ZVAL_STR(&a, zend_strpprintf(0, "%s:mongo:%s", ZSTR_VAL(c->username), ZSTR_VAL(c->password)));
        password = call_str("md5", 1, &a);
        zval_ptr_dtor(&a);
        if (!password) goto done;
    }
    salt = php_base64_decode((unsigned char *) s, sl);
    if (!salt) { swm_error_set(err, SWM_ERR_AUTH, 0, "Invalid SCRAM salt"); goto done; }

    {
        zend_string *key = zend_strpprintf(0, "%s|%s|%ld|%s|%.*s", mech, ZSTR_VAL(c->username), iters, ZSTR_VAL(password), (int) sl, s);
        if (c->scram_key && zend_string_equals(c->scram_key, key)) {
            salted = zend_string_copy(c->scram_salted);
            zend_string_release(key);
        } else {
            zval a[6];
            ZVAL_STRING(&a[0], alg);
            ZVAL_STR_COPY(&a[1], password);
            ZVAL_STR_COPY(&a[2], salt);
            ZVAL_LONG(&a[3], iters);
            ZVAL_LONG(&a[4], 0);
            ZVAL_TRUE(&a[5]);
            salted = call_str("hash_pbkdf2", 6, a);
            zval_ptr_dtor(&a[0]); zval_ptr_dtor(&a[1]); zval_ptr_dtor(&a[2]);
            if (!salted) { zend_string_release(key); goto done; }
            if (c->scram_key) zend_string_release(c->scram_key);
            if (c->scram_salted) zend_string_release(c->scram_salted);
            c->scram_key = key;
            c->scram_salted = zend_string_copy(salted);
        }
    }
    client_key = hmac(alg, salted, ZEND_STRL("Client Key"));
    if (!client_key) goto done;
    stored = hash_raw(alg, client_key);
    without_proof = zend_strpprintf(0, "c=biws,r=%.*s", (int) rl, r);
    auth_msg = zend_strpprintf(0, "%s,%s,%s", ZSTR_VAL(first_bare), ZSTR_VAL(server_first), ZSTR_VAL(without_proof));
    client_sig = hmac(alg, stored, ZSTR_VAL(auth_msg), ZSTR_LEN(auth_msg));
    if (!stored || !client_sig) goto done;
    {
        zend_string *proof = zend_string_alloc(ZSTR_LEN(client_key), 0);
        for (size_t k = 0; k < ZSTR_LEN(client_key); k++) ZSTR_VAL(proof)[k] = ZSTR_VAL(client_key)[k] ^ ZSTR_VAL(client_sig)[k];
        ZSTR_VAL(proof)[ZSTR_LEN(proof)] = '\0';
        proof_b64 = php_base64_encode((unsigned char *) ZSTR_VAL(proof), ZSTR_LEN(proof));
        zend_string_release(proof);
    }
    final_msg = zend_strpprintf(0, "%s,p=%s", ZSTR_VAL(without_proof), ZSTR_VAL(proof_b64));
    server_key = hmac(alg, salted, ZEND_STRL("Server Key"));
    if (!server_key) goto done;
    server_sig = hmac(alg, server_key, ZSTR_VAL(auth_msg), ZSTR_LEN(auth_msg));
    if (!server_sig) goto done;
    expected = php_base64_encode((unsigned char *) ZSTR_VAL(server_sig), ZSTR_LEN(server_sig));

    zval_ptr_dtor(&reply);
    array_init(&cmd);
    add_assoc_long(&cmd, "saslContinue", 1);
    Z_TRY_ADDREF(conv);
    add_assoc_zval(&cmd, "conversationId", &conv);
    swm_new_binary(&payload, ZSTR_VAL(final_msg), ZSTR_LEN(final_msg), 0);
    add_assoc_zval(&cmd, "payload", &payload);
    rc = sasl_command(c, conn, &cmd, &reply, err);
    zval_ptr_dtor(&cmd);
    if (rc == FAILURE) goto done;
    rc = FAILURE;
    {
        zend_string *pl = payload_of(&reply);
        const char *v;
        size_t vl;
        if (!pl) { swm_error_set(err, SWM_ERR_AUTH, 0, "Invalid SCRAM final message"); goto done; }
        if ((v = scram_field(pl, 'e', &vl))) {
            swm_error_set(err, SWM_ERR_AUTH, 0, "Authentication failed: %.*s", (int) vl, v);
            goto done;
        }
        v = scram_field(pl, 'v', &vl);
        if (!v || vl != ZSTR_LEN(expected) || memcmp(v, ZSTR_VAL(expected), vl) != 0) {
            swm_error_set(err, SWM_ERR_AUTH, 0, "Server signature verification failed");
            goto done;
        }
    }
    {
        int guard = 0;
        while (!zend_is_true(hfind(&reply, "done") ? hfind(&reply, "done") : &EG(uninitialized_zval)) && guard++ < 5) {
            zval_ptr_dtor(&reply);
            array_init(&cmd);
            add_assoc_long(&cmd, "saslContinue", 1);
            Z_TRY_ADDREF(conv);
            add_assoc_zval(&cmd, "conversationId", &conv);
            swm_new_binary(&payload, "", 0, 0);
            add_assoc_zval(&cmd, "payload", &payload);
            rc = sasl_command(c, conn, &cmd, &reply, err);
            zval_ptr_dtor(&cmd);
            if (rc == FAILURE) goto done;
            rc = FAILURE;
        }
    }
    rc = SUCCESS;
done:
    zval_ptr_dtor(&reply);
    zval_ptr_dtor(&conv);
    zend_string_release(nonce);
    zend_string_release(uname);
    zend_string_release(first_bare);
#define REL(x) if (x) zend_string_release(x)
    REL(server_first); REL(password); REL(salt); REL(salted); REL(client_key); REL(stored); REL(client_sig);
    REL(server_key); REL(server_sig); REL(auth_msg); REL(without_proof); REL(proof_b64); REL(final_msg); REL(expected);
#undef REL
    return rc;
}

/* ------------------------------------------------------------- SASLprep */
static int utf8_next(const unsigned char *s, size_t len, size_t *i, uint32_t *cp)
{
    unsigned char b = s[*i];
    int n;
    if (b < 0x80) { *cp = b; n = 1; }
    else if ((b & 0xE0) == 0xC0) { *cp = b & 0x1F; n = 2; }
    else if ((b & 0xF0) == 0xE0) { *cp = b & 0x0F; n = 3; }
    else if ((b & 0xF8) == 0xF0) { *cp = b & 0x07; n = 4; }
    else return FAILURE;
    if (*i + n > len) return FAILURE;
    for (int k = 1; k < n; k++) {
        if ((s[*i + k] & 0xC0) != 0x80) return FAILURE;
        *cp = (*cp << 6) | (s[*i + k] & 0x3F);
    }
    *i += n;
    return SUCCESS;
}

static void utf8_put(smart_str *o, uint32_t cp)
{
    if (cp < 0x80) smart_str_appendc(o, (char) cp);
    else if (cp < 0x800) { smart_str_appendc(o, (char) (0xC0 | (cp >> 6))); smart_str_appendc(o, (char) (0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { smart_str_appendc(o, (char) (0xE0 | (cp >> 12))); smart_str_appendc(o, (char) (0x80 | ((cp >> 6) & 0x3F))); smart_str_appendc(o, (char) (0x80 | (cp & 0x3F))); }
    else { smart_str_appendc(o, (char) (0xF0 | (cp >> 18))); smart_str_appendc(o, (char) (0x80 | ((cp >> 12) & 0x3F))); smart_str_appendc(o, (char) (0x80 | ((cp >> 6) & 0x3F))); smart_str_appendc(o, (char) (0x80 | (cp & 0x3F))); }
}

static zend_bool saslprep_prohibited(uint32_t cp)
{
    return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) || (cp >= 0xE000 && cp <= 0xF8FF)
        || (cp >= 0xF0000 && cp <= 0xFFFFD) || (cp >= 0x100000 && cp <= 0x10FFFD) || (cp >= 0xFDD0 && cp <= 0xFDEF)
        || (cp & 0xFFFE) == 0xFFFE || (cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0xFFF9 && cp <= 0xFFFD)
        || (cp >= 0x2FF0 && cp <= 0x2FFB) || cp == 0x340 || cp == 0x341 || (cp >= 0x200E && cp <= 0x200F)
        || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x206A && cp <= 0x206F) || cp == 0xE0001 || (cp >= 0xE0020 && cp <= 0xE007F);
}

/* RFC 4013 SASLprep (mapping, NFKC via ext/intl when available, prohibited output) */
static zend_string *saslprep(zend_string *in, swm_error *err)
{
    const unsigned char *p = (const unsigned char *) ZSTR_VAL(in);
    size_t len = ZSTR_LEN(in), i = 0;
    zend_bool ascii = 1;
    smart_str o = {0};
    zend_string *mapped, *norm = NULL;
    for (size_t k = 0; k < len; k++) if (p[k] >= 0x80) { ascii = 0; break; }
    if (ascii) {
        for (size_t k = 0; k < len; k++) {
            if (p[k] < 0x20 || p[k] == 0x7F) { swm_error_set(err, SWM_ERR_AUTH, 0, "Password contains prohibited characters (SASLprep)"); return NULL; }
        }
        return zend_string_copy(in);
    }
    while (i < len) {
        uint32_t cp;
        if (utf8_next(p, len, &i, &cp) == FAILURE) { smart_str_free(&o); swm_error_set(err, SWM_ERR_AUTH, 0, "Password is not valid UTF-8"); return NULL; }
        if (cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000) cp = 0x20;
        else if (cp == 0xAD || cp == 0x34F || cp == 0x1806 || (cp >= 0x180B && cp <= 0x180D) || (cp >= 0x200B && cp <= 0x200D)
                 || cp == 0x2060 || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF) continue;
        utf8_put(&o, cp);
    }
    smart_str_0(&o);
    mapped = o.s ? o.s : ZSTR_EMPTY_ALLOC();
    if (zend_hash_str_exists(EG(function_table), ZEND_STRL("normalizer_normalize"))) {
        zval a[2], r;
        zend_string *cn = zend_string_init(ZEND_STRL("Normalizer::FORM_KC"), 0);
        zval *form = zend_get_constant_ex(cn, NULL, 0);
        zend_string_release(cn);
        ZVAL_STR_COPY(&a[0], mapped);
        ZVAL_LONG(&a[1], form && Z_TYPE_P(form) == IS_LONG ? Z_LVAL_P(form) : 5);
        if (swm_call_function("normalizer_normalize", &r, 2, a) == SUCCESS && Z_TYPE(r) == IS_STRING) norm = Z_STR(r);
        else { zval_ptr_dtor(&r); if (EG(exception)) zend_clear_exception(); }
        zval_ptr_dtor(&a[0]);
    }
    if (norm) { zend_string_release(mapped); mapped = norm; }
    p = (const unsigned char *) ZSTR_VAL(mapped);
    len = ZSTR_LEN(mapped);
    i = 0;
    while (i < len) {
        uint32_t cp;
        if (utf8_next(p, len, &i, &cp) == FAILURE || saslprep_prohibited(cp)) {
            zend_string_release(mapped);
            swm_error_set(err, SWM_ERR_AUTH, 0, "Password contains prohibited characters (SASLprep)");
            return NULL;
        }
    }
    return mapped;
}

/* -------------------------------------------------------- HTTP / JSON helpers */
/* Blocking-looking but coroutine friendly when Swoole hooks streams (HOOK_TCP/SSL/FILE). */
static zend_string *http_request(const char *method, const char *url, const char *headers, const char *body, swm_error *err)
{
    zval opts, http, ctx, args[3], r;
    zend_string *res = NULL;
    array_init(&opts);
    array_init(&http);
    add_assoc_string(&http, "method", (char *) method);
    if (headers) add_assoc_string(&http, "header", (char *) headers);
    if (body) add_assoc_string(&http, "content", (char *) body);
    add_assoc_long(&http, "timeout", 10);
    add_assoc_bool(&http, "ignore_errors", 1);
    add_assoc_zval(&opts, "http", &http);
    if (swm_call_function("stream_context_create", &ctx, 1, &opts) == FAILURE) {
        zval_ptr_dtor(&opts);
        swm_error_set(err, SWM_ERR_AUTH, 0, "Unable to create HTTP context");
        return NULL;
    }
    ZVAL_STRING(&args[0], url);
    ZVAL_FALSE(&args[1]);
    ZVAL_COPY_VALUE(&args[2], &ctx);
    {
        int saved = EG(error_reporting);
        EG(error_reporting) = 0;
        if (swm_call_function("file_get_contents", &r, 3, args) == SUCCESS && Z_TYPE(r) == IS_STRING) {
            res = zend_string_copy(Z_STR(r));
        }
        EG(error_reporting) = saved;
    }
    if (EG(exception)) zend_clear_exception();
    zval_ptr_dtor(&r);
    zval_ptr_dtor(&args[0]);
    zval_ptr_dtor(&ctx);
    zval_ptr_dtor(&opts);
    if (!res) swm_error_set(err, SWM_ERR_AUTH, 0, "HTTP request to %s failed", url);
    return res;
}

static int json_decode_assoc(zend_string *s, zval *out)
{
    if (php_json_decode_ex(out, ZSTR_VAL(s), ZSTR_LEN(s), PHP_JSON_OBJECT_AS_ARRAY, 64) == FAILURE || Z_TYPE_P(out) != IS_ARRAY) {
        if (EG(exception)) zend_clear_exception();
        zval_ptr_dtor(out);
        ZVAL_UNDEF(out);
        return FAILURE;
    }
    return SUCCESS;
}

static zend_string *read_file(const char *path)
{
    zval a, r;
    zend_string *res = NULL;
    ZVAL_STRING(&a, path);
    {
        int saved = EG(error_reporting);
        EG(error_reporting) = 0;
        if (swm_call_function("file_get_contents", &r, 1, &a) == SUCCESS && Z_TYPE(r) == IS_STRING) res = zend_string_copy(Z_STR(r));
        EG(error_reporting) = saved;
    }
    if (EG(exception)) zend_clear_exception();
    zval_ptr_dtor(&r);
    zval_ptr_dtor(&a);
    if (res) {
        while (ZSTR_LEN(res) && (ZSTR_VAL(res)[ZSTR_LEN(res) - 1] == '\n' || ZSTR_VAL(res)[ZSTR_LEN(res) - 1] == '\r' || ZSTR_VAL(res)[ZSTR_LEN(res) - 1] == ' ')) {
            ZSTR_LEN(res)--;
        }
        ZSTR_VAL(res)[ZSTR_LEN(res)] = '\0';
    }
    return res;
}

static int64_t parse_iso_expiration(const char *s)
{
    int y, mo, d, h, mi, sec;
    struct tm tmv;
    if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d; tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = sec;
    return (int64_t) timegm(&tmv) * 1000000;
}

static zend_string *hex_lower(zend_string *bin)
{
    static const char hx[] = "0123456789abcdef";
    zend_string *o = zend_string_alloc(ZSTR_LEN(bin) * 2, 0);
    for (size_t i = 0; i < ZSTR_LEN(bin); i++) {
        ZSTR_VAL(o)[i * 2] = hx[(unsigned char) ZSTR_VAL(bin)[i] >> 4];
        ZSTR_VAL(o)[i * 2 + 1] = hx[(unsigned char) ZSTR_VAL(bin)[i] & 15];
    }
    ZSTR_VAL(o)[ZSTR_LEN(o)] = '\0';
    return o;
}

static int bson_payload_cmd(swm_client *c, swm_conn *conn, zval *cmd_base, zval *payload_doc, zval *reply, swm_error *err)
{
    smart_str b = {0};
    zval bin;
    int rc;
    if (swm_bson_encode(&b, payload_doc, err) == FAILURE) { smart_str_free(&b); return FAILURE; }
    swm_new_binary(&bin, ZSTR_VAL(b.s), ZSTR_LEN(b.s), 0);
    smart_str_free(&b);
    add_assoc_zval(cmd_base, "payload", &bin);
    rc = sasl_command(c, conn, cmd_base, reply, err);
    return rc;
}

/* ----------------------------------------------------------- MONGODB-AWS */
static int aws_credentials(swm_client *c, zend_string **key, zend_string **secret, zend_string **token, swm_error *err)
{
    const char *ek = getenv("AWS_ACCESS_KEY_ID"), *es = getenv("AWS_SECRET_ACCESS_KEY");
    zval *pt;
    *key = *secret = *token = NULL;
    if (c->username) {
        if (!c->password) { swm_error_set(err, SWM_ERR_AUTH, 0, "MONGODB-AWS requires a secret access key"); return FAILURE; }
        *key = zend_string_copy(c->username);
        *secret = zend_string_copy(c->password);
        if ((pt = hfind(&c->auth_props, "AWS_SESSION_TOKEN")) && Z_TYPE_P(pt) == IS_STRING) *token = zend_string_copy(Z_STR_P(pt));
        return SUCCESS;
    }
    if (ek && es && *ek && *es) {
        const char *et = getenv("AWS_SESSION_TOKEN");
        *key = zend_string_init(ek, strlen(ek), 0);
        *secret = zend_string_init(es, strlen(es), 0);
        if (et && *et) *token = zend_string_init(et, strlen(et), 0);
        return SUCCESS;
    }
    if (c->aws_key && swm_now_us() < c->aws_expire - 300000000) {
        *key = zend_string_copy(c->aws_key);
        *secret = zend_string_copy(c->aws_secret);
        if (c->aws_token) *token = zend_string_copy(c->aws_token);
        return SUCCESS;
    }
    {
        zend_string *resp = NULL;
        zval j, *creds = NULL;
        const char *kf = "AccessKeyId", *sf = "SecretAccessKey", *tf = "Token";
        const char *wif = getenv("AWS_WEB_IDENTITY_TOKEN_FILE"), *role = getenv("AWS_ROLE_ARN"), *rel = getenv("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI");
        ZVAL_UNDEF(&j);
        if (wif && role && *wif && *role) {
            zend_string *jwt = read_file(wif), *ejwt, *erole, *url;
            const char *sess = getenv("AWS_ROLE_SESSION_NAME");
            if (!jwt) { swm_error_set(err, SWM_ERR_AUTH, 0, "Unable to read AWS_WEB_IDENTITY_TOKEN_FILE"); return FAILURE; }
            ejwt = php_url_encode(ZSTR_VAL(jwt), ZSTR_LEN(jwt));
            erole = php_url_encode(role, strlen(role));
            url = zend_strpprintf(0, "https://sts.amazonaws.com/?Action=AssumeRoleWithWebIdentity&RoleSessionName=%s&RoleArn=%s&WebIdentityToken=%s&Version=2011-06-15",
                                  sess && *sess ? sess : "swmongo", ZSTR_VAL(erole), ZSTR_VAL(ejwt));
            resp = http_request("GET", ZSTR_VAL(url), "Accept: application/json\r\n", NULL, err);
            zend_string_release(url); zend_string_release(ejwt); zend_string_release(erole); zend_string_release(jwt);
            if (resp && json_decode_assoc(resp, &j) == SUCCESS) {
                creds = hfind(hfind(hfind(&j, "AssumeRoleWithWebIdentityResponse"), "AssumeRoleWithWebIdentityResult"), "Credentials");
                tf = "SessionToken";
            }
        } else if (rel && *rel) {
            zend_string *url = zend_strpprintf(0, "http://169.254.170.2%s", rel);
            resp = http_request("GET", ZSTR_VAL(url), NULL, NULL, err);
            zend_string_release(url);
            if (resp && json_decode_assoc(resp, &j) == SUCCESS) creds = &j;
        } else {
            zend_string *tok = http_request("PUT", "http://169.254.169.254/latest/api/token", "X-aws-ec2-metadata-token-ttl-seconds: 30\r\n", "", err);
            if (tok) {
                zend_string *hdr = zend_strpprintf(0, "X-aws-ec2-metadata-token: %s\r\n", ZSTR_VAL(tok));
                zend_string *rolename = http_request("GET", "http://169.254.169.254/latest/meta-data/iam/security-credentials/", ZSTR_VAL(hdr), NULL, err);
                if (rolename) {
                    zend_string *url = zend_strpprintf(0, "http://169.254.169.254/latest/meta-data/iam/security-credentials/%s", ZSTR_VAL(rolename));
                    resp = http_request("GET", ZSTR_VAL(url), ZSTR_VAL(hdr), NULL, err);
                    zend_string_release(url);
                    zend_string_release(rolename);
                    if (resp && json_decode_assoc(resp, &j) == SUCCESS) creds = &j;
                }
                zend_string_release(hdr);
                zend_string_release(tok);
            }
        }
        if (resp) zend_string_release(resp);
        if (!creds || !hfind(creds, kf) || !hfind(creds, sf)) {
            zval_ptr_dtor(&j);
            if (!err->msg) swm_error_set(err, SWM_ERR_AUTH, 0, "Unable to obtain AWS credentials");
            else swm_error_set(err, SWM_ERR_AUTH, 0, "Unable to obtain AWS credentials: %s", ZSTR_VAL(err->msg));
            return FAILURE;
        }
        swm_error_clear(err);
        *key = zval_get_string(hfind(creds, kf));
        *secret = zval_get_string(hfind(creds, sf));
        if (hfind(creds, tf)) *token = zval_get_string(hfind(creds, tf));
#define REPL(f, v) do { if (c->f) zend_string_release(c->f); c->f = (v) ? zend_string_copy(v) : NULL; } while (0)
        REPL(aws_key, *key); REPL(aws_secret, *secret); REPL(aws_token, *token);
#undef REPL
        c->aws_expire = hfind(creds, "Expiration") && Z_TYPE_P(hfind(creds, "Expiration")) == IS_STRING
            ? parse_iso_expiration(Z_STRVAL_P(hfind(creds, "Expiration"))) : swm_now_us() + 3600000000LL;
        zval_ptr_dtor(&j);
        return SUCCESS;
    }
}

static int auth_aws(swm_client *c, swm_conn *conn, swm_error *err)
{
    zend_string *key, *secret, *token, *server_nonce = NULL, *host = NULL;
    zval cmd, reply, pdoc, nonce, *pl, sdoc, *v;
    uint8_t nb[32];
    int rc = FAILURE;
    if (aws_credentials(c, &key, &secret, &token, err) == FAILURE) return FAILURE;
    ZVAL_UNDEF(&sdoc);
    php_random_bytes_silent(nb, sizeof(nb));
    array_init(&cmd);
    add_assoc_long(&cmd, "saslStart", 1);
    add_assoc_string(&cmd, "mechanism", "MONGODB-AWS");
    array_init(&pdoc);
    swm_new_binary(&nonce, (char *) nb, 32, 0);
    add_assoc_zval(&pdoc, "r", &nonce);
    add_assoc_long(&pdoc, "p", 110);
    rc = bson_payload_cmd(c, conn, &cmd, &pdoc, &reply, err);
    zval_ptr_dtor(&cmd);
    zval_ptr_dtor(&pdoc);
    if (rc == FAILURE) goto out;
    rc = FAILURE;
    pl = hfind(&reply, "payload");
    if (!pl || !swm_binary_data(pl) || swm_bson_decode(ZSTR_VAL(swm_binary_data(pl)), ZSTR_LEN(swm_binary_data(pl)), &sdoc, err) == FAILURE) {
        swm_error_set(err, SWM_ERR_AUTH, 0, "Invalid MONGODB-AWS server payload");
        zval_ptr_dtor(&reply);
        goto out;
    }
    if (!(v = hfind(&sdoc, "s")) || !swm_binary_data(v) || ZSTR_LEN(swm_binary_data(v)) != 64
        || memcmp(ZSTR_VAL(swm_binary_data(v)), nb, 32) != 0 || !(v = hfind(&sdoc, "h")) || Z_TYPE_P(v) != IS_STRING
        || Z_STRLEN_P(v) == 0 || Z_STRLEN_P(v) > 255 || strstr(Z_STRVAL_P(v), "..")) {
        swm_error_set(err, SWM_ERR_AUTH, 0, "Invalid MONGODB-AWS server nonce or host");
        zval_ptr_dtor(&reply);
        goto out;
    }
    server_nonce = zend_string_copy(swm_binary_data(hfind(&sdoc, "s")));
    host = zend_string_copy(Z_STR_P(hfind(&sdoc, "h")));
    {
        static const char *body = "Action=GetCallerIdentity&Version=2011-06-15";
        char amzdate[17], date[9], region[64];
        time_t now = time(NULL);
        struct tm g;
        zend_string *nonce_b64 = php_base64_encode((unsigned char *) ZSTR_VAL(server_nonce), ZSTR_LEN(server_nonce));
        zend_string *body_hash, *body_hex, *canon, *canon_hash, *canon_hex, *sts, *k1, *k2, *k3, *k4, *sig, *sig_hex, *authz, *sk;
        zend_string *b = zend_string_init(body, strlen(body), 0);
        const char *signed_headers = token ? "content-length;content-type;host;x-amz-date;x-amz-security-token;x-mongodb-gs2-cb-flag;x-mongodb-server-nonce"
                                           : "content-length;content-type;host;x-amz-date;x-mongodb-gs2-cb-flag;x-mongodb-server-nonce";
        zval cdoc;
        gmtime_r(&now, &g);
        strftime(amzdate, sizeof(amzdate), "%Y%m%dT%H%M%SZ", &g);
        strftime(date, sizeof(date), "%Y%m%d", &g);
        if (zend_string_equals_literal(host, "sts.amazonaws.com") || !memchr(ZSTR_VAL(host), '.', ZSTR_LEN(host))) {
            strcpy(region, "us-east-1");
        } else {
            const char *d1 = memchr(ZSTR_VAL(host), '.', ZSTR_LEN(host));
            const char *d2 = memchr(d1 + 1, '.', ZSTR_VAL(host) + ZSTR_LEN(host) - d1 - 1);
            size_t rl = (d2 ? d2 : ZSTR_VAL(host) + ZSTR_LEN(host)) - d1 - 1;
            if (rl >= sizeof(region)) rl = sizeof(region) - 1;
            memcpy(region, d1 + 1, rl);
            region[rl] = 0;
        }
        body_hash = hash_raw("sha256", b);
        body_hex = hex_lower(body_hash);
        if (token) {
            canon = zend_strpprintf(0, "POST\n/\n\ncontent-length:%zu\ncontent-type:application/x-www-form-urlencoded\nhost:%s\nx-amz-date:%s\nx-amz-security-token:%s\nx-mongodb-gs2-cb-flag:n\nx-mongodb-server-nonce:%s\n\n%s\n%s",
                                    strlen(body), ZSTR_VAL(host), amzdate, ZSTR_VAL(token), ZSTR_VAL(nonce_b64), signed_headers, ZSTR_VAL(body_hex));
        } else {
            canon = zend_strpprintf(0, "POST\n/\n\ncontent-length:%zu\ncontent-type:application/x-www-form-urlencoded\nhost:%s\nx-amz-date:%s\nx-mongodb-gs2-cb-flag:n\nx-mongodb-server-nonce:%s\n\n%s\n%s",
                                    strlen(body), ZSTR_VAL(host), amzdate, ZSTR_VAL(nonce_b64), signed_headers, ZSTR_VAL(body_hex));
        }
        canon_hash = hash_raw("sha256", canon);
        canon_hex = hex_lower(canon_hash);
        sts = zend_strpprintf(0, "AWS4-HMAC-SHA256\n%s\n%s/%s/sts/aws4_request\n%s", amzdate, date, region, ZSTR_VAL(canon_hex));
        sk = zend_strpprintf(0, "AWS4%s", ZSTR_VAL(secret));
        k1 = hmac("sha256", sk, date, strlen(date));
        k2 = hmac("sha256", k1, region, strlen(region));
        k3 = hmac("sha256", k2, "sts", 3);
        k4 = hmac("sha256", k3, "aws4_request", 12);
        sig = hmac("sha256", k4, ZSTR_VAL(sts), ZSTR_LEN(sts));
        sig_hex = hex_lower(sig);
        authz = zend_strpprintf(0, "AWS4-HMAC-SHA256 Credential=%s/%s/%s/sts/aws4_request, SignedHeaders=%s, Signature=%s",
                                ZSTR_VAL(key), date, region, signed_headers, ZSTR_VAL(sig_hex));
        array_init(&cmd);
        add_assoc_long(&cmd, "saslContinue", 1);
        if ((v = hfind(&reply, "conversationId"))) { Z_TRY_ADDREF_P(v); add_assoc_zval(&cmd, "conversationId", v); }
        array_init(&cdoc);
        add_assoc_str(&cdoc, "a", authz);
        add_assoc_string(&cdoc, "d", amzdate);
        if (token) add_assoc_str(&cdoc, "t", zend_string_copy(token));
        zval_ptr_dtor(&reply);
        rc = bson_payload_cmd(c, conn, &cmd, &cdoc, &reply, err);
        if (rc == SUCCESS) zval_ptr_dtor(&reply);
        zval_ptr_dtor(&cmd);
        zval_ptr_dtor(&cdoc);
#define REL2(x) zend_string_release(x)
        REL2(nonce_b64); REL2(b); REL2(body_hash); REL2(body_hex); REL2(canon); REL2(canon_hash); REL2(canon_hex); REL2(sts);
        REL2(sk); REL2(k1); REL2(k2); REL2(k3); REL2(k4); REL2(sig); REL2(sig_hex);
#undef REL2
    }
out:
    zval_ptr_dtor(&sdoc);
    if (server_nonce) zend_string_release(server_nonce);
    if (host) zend_string_release(host);
    zend_string_release(key);
    zend_string_release(secret);
    if (token) zend_string_release(token);
    return rc;
}

/* ---------------------------------------------------------- MONGODB-OIDC */
static zend_string *oidc_fetch_token(swm_client *c, swm_error *err)
{
    zval *env = hfind(&c->auth_props, "ENVIRONMENT"), *res = hfind(&c->auth_props, "TOKEN_RESOURCE");
    if (Z_TYPE(c->oidc_callback) != IS_UNDEF) {
        zval params, r, *tok;
        zend_string *t = NULL;
        array_init(&params);
        add_assoc_long(&params, "version", 1);
        add_assoc_long(&params, "timeoutMS", 60000);
        if (c->username) add_assoc_str(&params, "username", zend_string_copy(c->username));
        if (call_user_function(NULL, NULL, &c->oidc_callback, &r, 1, &params) == SUCCESS && !EG(exception)) {
            if (Z_TYPE(r) == IS_STRING) t = zend_string_copy(Z_STR(r));
            else if ((tok = hfind(&r, "accessToken")) && Z_TYPE_P(tok) == IS_STRING) t = zend_string_copy(Z_STR_P(tok));
        }
        zval_ptr_dtor(&r);
        zval_ptr_dtor(&params);
        if (!t && !EG(exception)) swm_error_set(err, SWM_ERR_AUTH, 0, "oidcCallback must return ['accessToken' => string]");
        return t;
    }
    if (!env || Z_TYPE_P(env) != IS_STRING) { swm_error_set(err, SWM_ERR_AUTH, 0, "MONGODB-OIDC has no token source"); return NULL; }
    if (!strcmp(Z_STRVAL_P(env), "test")) {
        const char *f = getenv("OIDC_TOKEN_FILE");
        zend_string *t = f ? read_file(f) : NULL;
        if (!t) swm_error_set(err, SWM_ERR_AUTH, 0, "Unable to read OIDC_TOKEN_FILE");
        return t;
    }
    if (!strcmp(Z_STRVAL_P(env), "k8s")) {
        const char *f = getenv("AZURE_FEDERATED_TOKEN_FILE");
        zend_string *t;
        if (!f || !*f) f = getenv("AWS_WEB_IDENTITY_TOKEN_FILE");
        if (!f || !*f) f = "/var/run/secrets/kubernetes.io/serviceaccount/token";
        t = read_file(f);
        if (!t) swm_error_set(err, SWM_ERR_AUTH, 0, "Unable to read Kubernetes token file %s", f);
        return t;
    }
    if (!strcmp(Z_STRVAL_P(env), "azure") || !strcmp(Z_STRVAL_P(env), "gcp")) {
        zend_string *eres = php_url_encode(Z_STRVAL_P(res), Z_STRLEN_P(res)), *url, *body, *t = NULL;
        zend_bool azure = !strcmp(Z_STRVAL_P(env), "azure");
        if (azure) {
            zend_string *cid = c->username ? php_url_encode(ZSTR_VAL(c->username), ZSTR_LEN(c->username)) : NULL;
            url = zend_strpprintf(0, "http://169.254.169.254/metadata/identity/oauth2/token?api-version=2018-02-01&resource=%s%s%s",
                                  ZSTR_VAL(eres), cid ? "&client_id=" : "", cid ? ZSTR_VAL(cid) : "");
            if (cid) zend_string_release(cid);
        } else {
            url = zend_strpprintf(0, "http://metadata/computeMetadata/v1/instance/service-accounts/default/identity?audience=%s", ZSTR_VAL(eres));
        }
        body = http_request("GET", ZSTR_VAL(url), azure ? "Accept: application/json\r\nMetadata: true\r\n" : "Metadata-Flavor: Google\r\n", NULL, err);
        zend_string_release(url);
        zend_string_release(eres);
        if (body) {
            if (azure) {
                zval j, *at;
                if (json_decode_assoc(body, &j) == SUCCESS && (at = hfind(&j, "access_token")) && Z_TYPE_P(at) == IS_STRING) t = zend_string_copy(Z_STR_P(at));
                zval_ptr_dtor(&j);
            } else {
                t = zend_string_copy(body);
            }
            zend_string_release(body);
        }
        if (!t) swm_error_set(err, SWM_ERR_AUTH, 0, "Unable to obtain an OIDC token from the %s metadata service", Z_STRVAL_P(env));
        return t;
    }
    swm_error_set(err, SWM_ERR_AUTH, 0, "Unsupported MONGODB-OIDC ENVIRONMENT \"%s\"", Z_STRVAL_P(env));
    return NULL;
}

static int auth_oidc(swm_client *c, swm_conn *conn, swm_error *err)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        zval cmd, reply, pdoc;
        int rc;
        zend_bool cached = c->oidc_token != NULL;
        if (!c->oidc_token && !(c->oidc_token = oidc_fetch_token(c, err))) return FAILURE;
        array_init(&cmd);
        add_assoc_long(&cmd, "saslStart", 1);
        add_assoc_string(&cmd, "mechanism", "MONGODB-OIDC");
        array_init(&pdoc);
        add_assoc_str(&pdoc, "jwt", zend_string_copy(c->oidc_token));
        rc = bson_payload_cmd(c, conn, &cmd, &pdoc, &reply, err);
        zval_ptr_dtor(&cmd);
        zval_ptr_dtor(&pdoc);
        if (rc == SUCCESS) { zval_ptr_dtor(&reply); return SUCCESS; }
        /* a cached token may have expired: drop it and try once with a fresh one */
        zend_string_release(c->oidc_token);
        c->oidc_token = NULL;
        if (!cached || err->code != 18) return FAILURE;
        swm_error_clear(err);
    }
    return FAILURE;
}

#ifdef SWM_HAVE_GSSAPI
/* ------------------------------------------------------- GSSAPI (Kerberos) */
static void gss_errmsg(OM_uint32 major, OM_uint32 minor, smart_str *out)
{
    OM_uint32 ctx = 0, min2;
    gss_buffer_desc b;
    int type = GSS_C_GSS_CODE;
    OM_uint32 code = major;
    for (int pass = 0; pass < 2; pass++) {
        do {
            if (gss_display_status(&min2, code, type, GSS_C_NO_OID, &ctx, &b) != GSS_S_COMPLETE) break;
            if (out->s && ZSTR_LEN(out->s)) smart_str_appends(out, "; ");
            smart_str_appendl(out, b.value, b.length);
            gss_release_buffer(&min2, &b);
        } while (ctx != 0);
        type = GSS_C_MECH_CODE;
        code = minor;
        ctx = 0;
    }
    smart_str_0(out);
}

static void gss_fail(swm_error *err, const char *what, OM_uint32 major, OM_uint32 minor)
{
    smart_str m = {0};
    gss_errmsg(major, minor, &m);
    swm_error_set(err, SWM_ERR_AUTH, 0, "GSSAPI %s failed: %s", what, m.s ? ZSTR_VAL(m.s) : "unknown error");
    smart_str_free(&m);
}

static int auth_gssapi(swm_client *c, swm_conn *conn, swm_error *err)
{
    zval *v;
    const char *service = "mongodb";
    zend_string *host = NULL, *target_str;
    OM_uint32 major, minor;
    gss_name_t target = GSS_C_NO_NAME;
    gss_cred_id_t cred = GSS_C_NO_CREDENTIAL;
    gss_ctx_id_t ctx = GSS_C_NO_CONTEXT;
    gss_buffer_desc in = GSS_C_EMPTY_BUFFER, out = GSS_C_EMPTY_BUFFER, nb;
    zval reply, conv;
    int rc = FAILURE, step = 0;
    zend_bool complete = 0;

    ZVAL_UNDEF(&reply);
    ZVAL_UNDEF(&conv);
    if ((v = hfind(&c->auth_props, "SERVICE_NAME")) && Z_TYPE_P(v) == IS_STRING) service = Z_STRVAL_P(v);
    if ((v = hfind(&c->auth_props, "SERVICE_HOST")) && Z_TYPE_P(v) == IS_STRING) {
        host = zend_string_copy(Z_STR_P(v));
    } else {
        zval *canon = hfind(&c->auth_props, "CANONICALIZE_HOST_NAME");
        host = zend_string_copy(conn->server->host);
        if (canon && Z_TYPE_P(canon) == IS_STRING && (!strcasecmp(Z_STRVAL_P(canon), "true") || !strcasecmp(Z_STRVAL_P(canon), "forward"))) {
            struct addrinfo hints, *res = NULL;
            memset(&hints, 0, sizeof(hints));
            hints.ai_flags = AI_CANONNAME;
            if (getaddrinfo(ZSTR_VAL(host), NULL, &hints, &res) == 0 && res && res->ai_canonname) {
                zend_string_release(host);
                host = zend_string_init(res->ai_canonname, strlen(res->ai_canonname), 0);
            }
            if (res) freeaddrinfo(res);
        }
    }
    if ((v = hfind(&c->auth_props, "SERVICE_REALM")) && Z_TYPE_P(v) == IS_STRING) {
        target_str = zend_strpprintf(0, "%s/%s@%s", service, ZSTR_VAL(host), Z_STRVAL_P(v));
        nb.value = ZSTR_VAL(target_str);
        nb.length = ZSTR_LEN(target_str);
        major = gss_import_name(&minor, &nb, (gss_OID) GSS_KRB5_NT_PRINCIPAL_NAME, &target);
    } else {
        target_str = zend_strpprintf(0, "%s@%s", service, ZSTR_VAL(host));
        nb.value = ZSTR_VAL(target_str);
        nb.length = ZSTR_LEN(target_str);
        major = gss_import_name(&minor, &nb, GSS_C_NT_HOSTBASED_SERVICE, &target);
    }
    zend_string_release(target_str);
    zend_string_release(host);
    if (GSS_ERROR(major)) { gss_fail(err, "import_name", major, minor); goto out; }

    if (c->password) {
#ifdef SWM_HAVE_GSS_PASSWORD
        gss_name_t user = GSS_C_NO_NAME;
        gss_buffer_desc pw;
        nb.value = ZSTR_VAL(c->username);
        nb.length = ZSTR_LEN(c->username);
        major = gss_import_name(&minor, &nb, GSS_C_NT_USER_NAME, &user);
        if (GSS_ERROR(major)) { gss_fail(err, "import_name(user)", major, minor); goto out; }
        pw.value = ZSTR_VAL(c->password);
        pw.length = ZSTR_LEN(c->password);
        major = gss_acquire_cred_with_password(&minor, user, &pw, GSS_C_INDEFINITE, GSS_C_NO_OID_SET, GSS_C_INITIATE, &cred, NULL, NULL);
        gss_release_name(&minor, &user);
        if (GSS_ERROR(major)) { gss_fail(err, "acquire_cred_with_password", major, minor); goto out; }
#else
        swm_error_set(err, SWM_ERR_AUTH, 0, "GSSAPI with a password requires MIT Kerberos (gss_acquire_cred_with_password); use kinit instead");
        goto out;
#endif
    }

    for (step = 0; step < 10; step++) {
        zval cmd, payload, *pl;
        zend_string *srv;
        if (!complete) {
            major = gss_init_sec_context(&minor, cred, &ctx, target, GSS_C_NO_OID, GSS_C_MUTUAL_FLAG, 0,
                                         GSS_C_NO_CHANNEL_BINDINGS, &in, NULL, &out, NULL, NULL);
            if (GSS_ERROR(major)) { gss_fail(err, "init_sec_context", major, minor); goto out; }
            if (major == GSS_S_COMPLETE) complete = 1;
        } else {
            /* RFC 4752: unwrap the server's security layer offer, answer "no layer" + authzid */
            gss_buffer_desc wrapped, unwrapped = GSS_C_EMPTY_BUFFER, msg;
            int conf = 0;
            unsigned char *buf;
            wrapped.value = in.value;
            wrapped.length = in.length;
            major = gss_unwrap(&minor, ctx, &wrapped, &unwrapped, &conf, NULL);
            if (GSS_ERROR(major) || unwrapped.length < 4) { gss_fail(err, "unwrap", major, minor); gss_release_buffer(&minor, &unwrapped); goto out; }
            msg.length = 4 + ZSTR_LEN(c->username);
            buf = emalloc(msg.length);
            memcpy(buf, unwrapped.value, 4);
            buf[0] = 1;
            memcpy(buf + 4, ZSTR_VAL(c->username), ZSTR_LEN(c->username));
            msg.value = buf;
            gss_release_buffer(&minor, &unwrapped);
            major = gss_wrap(&minor, ctx, 0, GSS_C_QOP_DEFAULT, &msg, &conf, &out);
            efree(buf);
            if (GSS_ERROR(major)) { gss_fail(err, "wrap", major, minor); goto out; }
        }
        array_init(&cmd);
        if (Z_TYPE(conv) == IS_UNDEF) {
            add_assoc_long(&cmd, "saslStart", 1);
            add_assoc_string(&cmd, "mechanism", "GSSAPI");
            add_assoc_long(&cmd, "autoAuthorize", 1);
        } else {
            add_assoc_long(&cmd, "saslContinue", 1);
            Z_TRY_ADDREF(conv);
            add_assoc_zval(&cmd, "conversationId", &conv);
        }
        swm_new_binary(&payload, out.value ? (char *) out.value : "", out.length, 0);
        add_assoc_zval(&cmd, "payload", &payload);
        gss_release_buffer(&minor, &out);
        if (in.value && step > 0) { efree(in.value); }
        in.value = NULL;
        in.length = 0;
        zval_ptr_dtor(&reply);
        rc = sasl_command(c, conn, &cmd, &reply, err);
        zval_ptr_dtor(&cmd);
        if (rc == FAILURE) goto out;
        rc = FAILURE;
        if (Z_TYPE(conv) == IS_UNDEF && (v = hfind(&reply, "conversationId"))) ZVAL_COPY(&conv, v);
        if ((v = hfind(&reply, "done")) && zend_is_true(v)) { rc = SUCCESS; goto out; }
        pl = hfind(&reply, "payload");
        srv = pl ? swm_binary_data(pl) : NULL;
        if (srv && ZSTR_LEN(srv)) {
            in.value = emalloc(ZSTR_LEN(srv));
            memcpy(in.value, ZSTR_VAL(srv), ZSTR_LEN(srv));
            in.length = ZSTR_LEN(srv);
        }
    }
    swm_error_set(err, SWM_ERR_AUTH, 0, "GSSAPI conversation did not complete");
out:
    if (in.value) efree(in.value);
    zval_ptr_dtor(&reply);
    zval_ptr_dtor(&conv);
    if (ctx != GSS_C_NO_CONTEXT) gss_delete_sec_context(&minor, &ctx, GSS_C_NO_BUFFER);
    if (cred != GSS_C_NO_CREDENTIAL) gss_release_cred(&minor, &cred);
    if (target != GSS_C_NO_NAME) gss_release_name(&minor, &target);
    return rc;
}
#endif

static int authenticate(swm_client *c, swm_conn *conn, zval *hello, swm_error *err)
{
    const char *mech = c->auth_mechanism ? ZSTR_VAL(c->auth_mechanism) : NULL;
    zval cmd, reply;
    int rc;
    if (!mech) {
        zval *mechs = hfind(hello, "saslSupportedMechs"), *m;
        mech = "SCRAM-SHA-1";
        if (mechs && Z_TYPE_P(mechs) == IS_ARRAY) {
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(mechs), m) {
                if (Z_TYPE_P(m) == IS_STRING && strcmp(Z_STRVAL_P(m), "SCRAM-SHA-256") == 0) mech = "SCRAM-SHA-256";
            } ZEND_HASH_FOREACH_END();
        }
    }
    if (!strcasecmp(mech, "MONGODB-AWS")) return auth_aws(c, conn, err);
#ifdef SWM_HAVE_GSSAPI
    if (!strcasecmp(mech, "GSSAPI")) return auth_gssapi(c, conn, err);
#endif
    if (!strcasecmp(mech, "MONGODB-OIDC")) return auth_oidc(c, conn, err);
    if (!strcasecmp(mech, "MONGODB-X509")) {
        array_init(&cmd);
        add_assoc_long(&cmd, "authenticate", 1);
        add_assoc_string(&cmd, "mechanism", "MONGODB-X509");
        if (c->username) add_assoc_str(&cmd, "user", zend_string_copy(c->username));
        rc = sasl_command(c, conn, &cmd, &reply, err);
        zval_ptr_dtor(&cmd);
        if (rc == SUCCESS) zval_ptr_dtor(&reply);
        return rc;
    }
    if (!c->password) {
        swm_error_set(err, SWM_ERR_AUTH, 0, "A password is required for %s", mech);
        return FAILURE;
    }
    if (!strcasecmp(mech, "PLAIN")) {
        zend_string *pl = zend_strpprintf(0, "%c%s%c%s", 0, ZSTR_VAL(c->username), 0, ZSTR_VAL(c->password));
        zval payload;
        array_init(&cmd);
        add_assoc_long(&cmd, "saslStart", 1);
        add_assoc_string(&cmd, "mechanism", "PLAIN");
        swm_new_binary(&payload, ZSTR_VAL(pl), ZSTR_LEN(pl), 0);
        zend_string_release(pl);
        add_assoc_zval(&cmd, "payload", &payload);
        add_assoc_long(&cmd, "autoAuthorize", 1);
        rc = sasl_command(c, conn, &cmd, &reply, err);
        zval_ptr_dtor(&cmd);
        if (rc == SUCCESS) zval_ptr_dtor(&reply);
        return rc;
    }
    return auth_scram(c, conn, strcasecmp(mech, "SCRAM-SHA-256") == 0, err);
}

/* ========================================================== connections */
static void build_hello(swm_client *c, zval *cmd, zend_bool legacy, zend_bool handshake, zend_bool for_auth)
{
    array_init(cmd);
    add_assoc_long(cmd, legacy ? "isMaster" : "hello", 1);
    add_assoc_bool(cmd, "helloOk", 1);
    if (c->api_version) {
        add_assoc_str(cmd, "apiVersion", zend_string_copy(c->api_version));
        if (c->api_strict >= 0) add_assoc_bool(cmd, "apiStrict", c->api_strict);
        if (c->api_deprecation >= 0) add_assoc_bool(cmd, "apiDeprecationErrors", c->api_deprecation);
    }
    if (handshake && c->load_balanced) add_assoc_bool(cmd, "loadBalanced", 1);
    if (handshake) {
        zval client, driver, os, comp;
        array_init(&client);
        array_init(&driver);
        add_assoc_string(&driver, "name", SWM_DRIVER_NAME);
        add_assoc_string(&driver, "version", PHP_SWMONGO_VERSION);
        add_assoc_zval(&client, "driver", &driver);
        array_init(&os);
        add_assoc_string(&os, "type", PHP_OS_FAMILY);
        add_assoc_zval(&client, "os", &os);
        add_assoc_str(&client, "platform", zend_strpprintf(0, "PHP %s%s", PHP_VERSION, swm_swoole_co_ce() ? " / Swoole" : ""));
        if (c->appname) {
            zval app;
            array_init(&app);
            add_assoc_str(&app, "name", zend_string_copy(c->appname));
            add_assoc_zval(&client, "application", &app);
        }
        add_assoc_zval(cmd, "client", &client);
        array_init(&comp);
        for (int i = 0; i < c->ncompressors; i++) {
            add_next_index_string(&comp, c->compressors[i] == 2 ? "zlib" : c->compressors[i] == 3 ? "zstd" : "snappy");
        }
        add_assoc_zval(cmd, "compression", &comp);
        if (for_auth && c->username && !c->auth_mechanism) {
            add_assoc_str(cmd, "saslSupportedMechs", zend_strpprintf(0, "%s.%s", ZSTR_VAL(c->auth_source), ZSTR_VAL(c->username)));
        }
    }
}

/* RFC 1928 SOCKS5 CONNECT (+ RFC 1929 username/password), then optional TLS */
static int socks5_connect(swm_client *c, swm_conn *conn, swm_server *s, swm_error *err)
{
    zend_string *user = opt_str(c, "proxyusername"), *pass = opt_str(c, "proxypassword");
    unsigned char buf[600];
    size_t n = 0, hl = ZSTR_LEN(s->host);
    int rc = FAILURE;
    if (hl > 255) { swm_error_set(err, SWM_ERR_ARG, 0, "Host name too long for SOCKS5"); goto out; }
    buf[n++] = 5;
    if (user) { buf[n++] = 2; buf[n++] = 0; buf[n++] = 2; } else { buf[n++] = 1; buf[n++] = 0; }
    if (write_all(conn, (char *) buf, n, err) == FAILURE || read_all(conn, (char *) buf, 2, err) == FAILURE) goto out;
    if (buf[0] != 5 || buf[1] == 0xFF) { swm_error_set(err, SWM_ERR_NETWORK, 0, "SOCKS5 proxy rejected the authentication methods"); goto out; }
    if (buf[1] == 2) {
        size_t ul = user ? ZSTR_LEN(user) : 0, pl = pass ? ZSTR_LEN(pass) : 0;
        if (ul > 255 || pl > 255) { swm_error_set(err, SWM_ERR_ARG, 0, "SOCKS5 credentials too long"); goto out; }
        n = 0;
        buf[n++] = 1;
        buf[n++] = (unsigned char) ul; if (ul) memcpy(buf + n, ZSTR_VAL(user), ul); n += ul;
        buf[n++] = (unsigned char) pl; if (pl) memcpy(buf + n, ZSTR_VAL(pass), pl); n += pl;
        if (write_all(conn, (char *) buf, n, err) == FAILURE || read_all(conn, (char *) buf, 2, err) == FAILURE) goto out;
        if (buf[1] != 0) { swm_error_set(err, SWM_ERR_AUTH, 0, "SOCKS5 proxy authentication failed"); goto out; }
    }
    n = 0;
    buf[n++] = 5; buf[n++] = 1; buf[n++] = 0; buf[n++] = 3;
    buf[n++] = (unsigned char) hl;
    memcpy(buf + n, ZSTR_VAL(s->host), hl); n += hl;
    buf[n++] = (unsigned char) (s->port >> 8);
    buf[n++] = (unsigned char) (s->port & 0xFF);
    if (write_all(conn, (char *) buf, n, err) == FAILURE || read_all(conn, (char *) buf, 4, err) == FAILURE) goto out;
    if (buf[1] != 0) { swm_error_set(err, SWM_ERR_NETWORK, 0, "SOCKS5 proxy could not connect to %s (code %d)", ZSTR_VAL(s->address), buf[1]); goto out; }
    {
        size_t skip = buf[3] == 1 ? 4 : buf[3] == 4 ? 16 : 0;
        if (buf[3] == 3) { if (read_all(conn, (char *) buf, 1, err) == FAILURE) goto out; skip = buf[0]; }
        if (read_all(conn, (char *) buf, skip + 2, err) == FAILURE) goto out;
    }
    if (c->tls) {
        php_stream_context *ctx = php_stream_context_alloc();
        zval peer, *opts;
        if (c->ctx && (opts = zend_hash_str_find(Z_ARRVAL(c->ctx->options), ZEND_STRL("ssl")))) {
            zend_string *k;
            zval *v;
            ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(opts), k, v) { if (k) php_stream_context_set_option(ctx, "ssl", ZSTR_VAL(k), v); } ZEND_HASH_FOREACH_END();
        }
        ZVAL_STR_COPY(&peer, s->host);
        php_stream_context_set_option(ctx, "ssl", "peer_name", &peer);
        zval_ptr_dtor(&peer);
        php_stream_context_set(conn->stream, ctx);
        if (php_stream_xport_crypto_setup(conn->stream, STREAM_CRYPTO_METHOD_TLS_CLIENT, NULL) < 0
            || php_stream_xport_crypto_enable(conn->stream, 1) < 0) {
            conn->broken = 1;
            swm_error_set(err, SWM_ERR_NETWORK, 0, "TLS handshake with %s through the SOCKS5 proxy failed", ZSTR_VAL(s->address));
            goto out;
        }
    }
    rc = SUCCESS;
out:
    if (user) zend_string_release(user);
    if (pass) zend_string_release(pass);
    return rc;
}

static swm_conn *conn_open(swm_client *c, swm_server *s, zend_bool do_auth, zval *hello_out, swm_error *err)
{
    zend_string *target, *errstr = NULL;
    int errcode = 0;
    struct timeval tv;
    php_stream *stream;
    swm_conn *conn;
    zval cmd, reply;

    zend_string *proxy = s->is_unix ? NULL : opt_str(c, "proxyhost");
    if (s->is_unix) {
        target = zend_strpprintf(0, "unix://%s", ZSTR_VAL(s->host));
    } else if (proxy) {
        /* SOCKS5: plain TCP to the proxy, TLS (if any) is negotiated after the tunnel is up */
        target = zend_strpprintf(0, "tcp://%s:%ld", ZSTR_VAL(proxy), (long) opt_long(c, "proxyport", 1080));
        zend_string_release(proxy);
    } else {
        target = zend_strpprintf(0, "%s://%s", c->tls ? "tls" : "tcp", ZSTR_VAL(s->address));
    }
    check_hooks(c, s->is_unix);
    tv.tv_sec = (time_t) (c->connect_timeout / 1000000);
    tv.tv_usec = (suseconds_t) (c->connect_timeout % 1000000);
    stream = php_stream_xport_create(ZSTR_VAL(target), ZSTR_LEN(target), 0,
                                     STREAM_XPORT_CLIENT | STREAM_XPORT_CONNECT, NULL, &tv, c->ctx, &errstr, &errcode);
    zend_string_release(target);
    if (!stream) {
        swm_error_set(err, SWM_ERR_NETWORK, errcode, "Failed to connect to %s: %s", ZSTR_VAL(s->address),
                      errstr ? ZSTR_VAL(errstr) : "unknown error");
        if (errstr) zend_string_release(errstr);
        return NULL;
    }
    if (errstr) zend_string_release(errstr);
    if (c->socket_timeout > 0) {
        tv.tv_sec = (time_t) (c->socket_timeout / 1000000);
        tv.tv_usec = (suseconds_t) (c->socket_timeout % 1000000);
    } else {
        tv.tv_sec = -1;
        tv.tv_usec = 0;
    }
    php_stream_set_option(stream, PHP_STREAM_OPTION_READ_TIMEOUT, 0, &tv);
    {
        int one = 1;
        (void) one;
    }

    conn = ecalloc(1, sizeof(*conn));
    conn->stream = stream;
    conn->server = s;
    server_addref(s);
    conn->generation = s->generation;
    conn->last_used = swm_now_us();

    if (!s->is_unix && opt(c, "proxyhost") && socks5_connect(c, conn, s, err) == FAILURE) {
        conn_close(conn);
        return NULL;
    }

    build_hello(c, &cmd, 0, 1, do_auth);
    if (conn_command(c, conn, "admin", &cmd, NULL, &reply, NULL, 0, err) == FAILURE) goto fail_cmd;
    if (!c->api_version && !reply_ok(&reply) && zv_long(hfind(&reply, "code"), 0) == 59) {
        /* pre 4.4.2 server: no "hello" command */
        zval_ptr_dtor(&cmd);
        zval_ptr_dtor(&reply);
        conn->legacy_hello = 1;
        build_hello(c, &cmd, 1, 1, do_auth);
        if (conn_command(c, conn, "admin", &cmd, NULL, &reply, NULL, 0, err) == FAILURE) goto fail_cmd;
    }
    zval_ptr_dtor(&cmd);
    if (!reply_ok(&reply)) {
        error_from_reply(err, &reply);
        zval_ptr_dtor(&reply);
        conn_close(conn);
        return NULL;
    }
    if (c->load_balanced) {
        if (!hfind(&reply, "serviceId")) {
            zval_ptr_dtor(&reply);
            conn_close(conn);
            swm_error_set(err, SWM_ERR_NETWORK, 0, "Driver attempted to initialize in load balancing mode, but the server does not support this mode");
            return NULL;
        }
        s->max_wire = zv_long(hfind(&reply, "maxWireVersion"), s->max_wire);
        s->max_bson = zv_long(hfind(&reply, "maxBsonObjectSize"), s->max_bson);
        s->max_batch = zv_long(hfind(&reply, "maxWriteBatchSize"), s->max_batch);
        s->session_timeout = zv_long(hfind(&reply, "logicalSessionTimeoutMinutes"), s->session_timeout);
        if (Z_TYPE(s->hello) == IS_UNDEF) ZVAL_COPY(&s->hello, &reply);
    }
    {
        zval *comp = hfind(&reply, "compression"), *v;
        if (comp && Z_TYPE_P(comp) == IS_ARRAY) {
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(comp), v) {
                if (conn->compressor || Z_TYPE_P(v) != IS_STRING) continue;
                if (!strcmp(Z_STRVAL_P(v), "zlib")) conn->compressor = 2;
                else if (!strcmp(Z_STRVAL_P(v), "zstd")) conn->compressor = 3;
                else if (!strcmp(Z_STRVAL_P(v), "snappy")) conn->compressor = 1;
            } ZEND_HASH_FOREACH_END();
        }
        s->max_msg = zv_long(hfind(&reply, "maxMessageSizeBytes"), s->max_msg);
    }
    if (do_auth && (c->username || (c->auth_mechanism && (zend_string_equals_literal_ci(c->auth_mechanism, "MONGODB-X509")
                                                         || zend_string_equals_literal_ci(c->auth_mechanism, "MONGODB-AWS")
                                                         || zend_string_equals_literal_ci(c->auth_mechanism, "MONGODB-OIDC"))))) {
        zval *arb = hfind(&reply, "arbiterOnly");
        if (!(arb && zend_is_true(arb)) && authenticate(c, conn, &reply, err) == FAILURE) {
            zval_ptr_dtor(&reply);
            conn_close(conn);
            return NULL;
        }
    }
    if (hello_out) ZVAL_COPY_VALUE(hello_out, &reply); else zval_ptr_dtor(&reply);
    return conn;
fail_cmd:
    zval_ptr_dtor(&cmd);
    conn_close(conn);
    return NULL;
}

/* =============================================================== topology */
static zend_bool has_sdam(swm_client *c)
{
    zval *sub;
    if (Z_TYPE(c->subscribers) != IS_ARRAY) return 0;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(c->subscribers), sub) {
        if (Z_TYPE_P(sub) == IS_OBJECT && instanceof_function(Z_OBJCE_P(sub), swm_ce_SDAMSubscriber)) return 1;
    } ZEND_HASH_FOREACH_END();
    return 0;
}

static void notify_sdam(swm_client *c, const char *method, zval *event)
{
    zval *sub, subs;
    ZVAL_COPY(&subs, &c->subscribers);
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(subs), sub) {
        zval ret;
        if (Z_TYPE_P(sub) != IS_OBJECT || !instanceof_function(Z_OBJCE_P(sub), swm_ce_SDAMSubscriber)) continue;
        zend_call_method(Z_OBJ_P(sub), Z_OBJCE_P(sub), NULL, method, strlen(method), &ret, 1, event, NULL);
        zval_ptr_dtor(&ret);
        if (EG(exception)) zend_clear_exception();
    } ZEND_HASH_FOREACH_END();
    zval_ptr_dtor(&subs);
}

static void server_desc(swm_server *s, zval *out)
{
    swm_new_server_description(out, s->host, s->port, srv_type_names[s->type], &s->hello, s->last_update,
                               s->type == SRV_UNKNOWN ? -1 : s->rtt / 1000);
}

static void topology_desc(swm_client *c, zval *out)
{
    zval list;
    array_init(&list);
    for (int i = 0; i < c->nservers; i++) {
        zval d;
        if (c->servers[i]->removed) continue;
        server_desc(c->servers[i], &d);
        add_next_index_zval(&list, &d);
    }
    swm_new_topology_description(out, topo_names[c->topo], &list);
    zval_ptr_dtor(&list);
}

static void emit_server_event(swm_client *c, int kind, const char *method, swm_server *s)
{
    zval ev;
    swm_new_sdam_event(&ev, kind, s->host, s->port, &c->topology_id, NULL, NULL, 0, NULL, NULL);
    notify_sdam(c, method, &ev);
    zval_ptr_dtor(&ev);
}

static zend_string *topology_signature(swm_client *c)
{
    smart_str sig = {0};
    smart_str_appends(&sig, topo_names[c->topo]);
    for (int i = 0; i < c->nservers; i++) {
        if (c->servers[i]->removed) continue;
        smart_str_appendc(&sig, '|');
        smart_str_append(&sig, c->servers[i]->address);
        smart_str_appendc(&sig, '=');
        smart_str_appends(&sig, srv_type_names[c->servers[i]->type]);
    }
    smart_str_0(&sig);
    return sig.s;
}

static void server_set_unknown(swm_client *c, swm_server *s, const char *msg, zend_bool clear_pool)
{
    s->type = SRV_UNKNOWN;
    if (s->error) zend_string_release(s->error);
    s->error = msg ? zend_string_init(msg, strlen(msg), 0) : NULL;
    s->last_update = swm_now_us();
    c->last_scan = 0; /* force a rescan before the next selection */
    if (clear_pool) server_clear_pool(s);
}

static void collect_hosts(zval *reply, const char *key, zval *out)
{
    zval *list = hfind(reply, key), *h;
    if (!list || Z_TYPE_P(list) != IS_ARRAY) return;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(list), h) {
        if (Z_TYPE_P(h) == IS_STRING) add_next_index_str(out, zend_string_tolower(Z_STR_P(h)));
    } ZEND_HASH_FOREACH_END();
}

static void server_update(swm_client *c, swm_server *s, zval *reply, int64_t rtt)
{
    zval *v;
    if (!reply_ok(reply)) {
        zval *m = hfind(reply, "errmsg");
        char buf[512];
        snprintf(buf, sizeof(buf), "hello command failed: %s", m && Z_TYPE_P(m) == IS_STRING ? Z_STRVAL_P(m) : "unknown error");
        server_set_unknown(c, s, buf, 1);
        return;
    }
    if ((v = hfind(reply, "msg")) && Z_TYPE_P(v) == IS_STRING && strcmp(Z_STRVAL_P(v), "isdbgrid") == 0) {
        s->type = SRV_MONGOS;
    } else if ((v = hfind(reply, "isreplicaset")) && zend_is_true(v)) {
        s->type = SRV_RS_GHOST;
    } else if ((v = hfind(reply, "setName")) && Z_TYPE_P(v) == IS_STRING) {
        zval *w = hfind(reply, "isWritablePrimary");
        if (!w) w = hfind(reply, "ismaster");
        if (w && zend_is_true(w)) s->type = SRV_RS_PRIMARY;
        else if ((w = hfind(reply, "secondary")) && zend_is_true(w)) s->type = SRV_RS_SECONDARY;
        else if ((w = hfind(reply, "arbiterOnly")) && zend_is_true(w)) s->type = SRV_RS_ARBITER;
        else s->type = SRV_RS_OTHER;
    } else {
        s->type = SRV_STANDALONE;
    }
    if (s->set_name) { zend_string_release(s->set_name); s->set_name = NULL; }
    if ((v = hfind(reply, "setName")) && Z_TYPE_P(v) == IS_STRING) s->set_name = zend_string_copy(Z_STR_P(v));
    s->rtt = s->rtt == 0 ? rtt : (rtt + 4 * s->rtt) / 5;
    s->max_wire = zv_long(hfind(reply, "maxWireVersion"), 0);
    s->max_bson = zv_long(hfind(reply, "maxBsonObjectSize"), 16 * 1024 * 1024);
    s->max_msg = zv_long(hfind(reply, "maxMessageSizeBytes"), SWM_MAX_MESSAGE);
    s->max_batch = zv_long(hfind(reply, "maxWriteBatchSize"), 100000);
    s->session_timeout = zv_long(hfind(reply, "logicalSessionTimeoutMinutes"), -1);
    zval_ptr_dtor(&s->tags);
    if ((v = hfind(reply, "tags")) && Z_TYPE_P(v) == IS_ARRAY) ZVAL_COPY(&s->tags, v); else array_init(&s->tags);
    zval_ptr_dtor(&s->hello);
    ZVAL_COPY(&s->hello, reply);
    {
        zval *lw = hfind(reply, "lastWrite"), *d;
        s->last_write_ms = 0;
        if (lw && (d = hfind(lw, "lastWriteDate")) && Z_TYPE_P(d) == IS_OBJECT && Z_OBJCE_P(d) == swm_ce_UTCDateTime) {
            s->last_write_ms = Z_LVAL_P(OBJ_PROP_NUM(Z_OBJ_P(d), 0));
        }
    }
    zval_ptr_dtor(&s->hosts);
    array_init(&s->hosts);
    collect_hosts(reply, "hosts", &s->hosts);
    collect_hosts(reply, "passives", &s->hosts);
    collect_hosts(reply, "arbiters", &s->hosts);
    if (s->error) { zend_string_release(s->error); s->error = NULL; }
    s->last_update = swm_now_us();
}

static void server_check(swm_client *c, swm_server *s)
{
    swm_error err;
    zval reply, prev;
    int64_t t0 = swm_now_us();
    int ok = FAILURE;
    zend_bool sdam = has_sdam(c);
    srv_type prev_type = s->type;
    swm_error_init(&err);
    ZVAL_UNDEF(&reply);
    ZVAL_UNDEF(&prev);
    if (sdam) {
        zval ev;
        server_desc(s, &prev);
        swm_new_sdam_event(&ev, 3, s->host, s->port, NULL, NULL, NULL, 0, NULL, NULL);
        notify_sdam(c, "serverheartbeatstarted", &ev);
        zval_ptr_dtor(&ev);
    }
    if (s->monitor) {
        zval cmd;
        swm_conn *m = s->monitor;
        build_hello(c, &cmd, m->legacy_hello, 0, 0);
        ok = conn_command(c, m, "admin", &cmd, NULL, &reply, NULL, 0, &err);
        zval_ptr_dtor(&cmd);
        if (ok == FAILURE || m->broken) {
            if (s->monitor == m) s->monitor = NULL;
            conn_close(m);
            if (Z_TYPE(reply) != IS_UNDEF) { zval_ptr_dtor(&reply); ZVAL_UNDEF(&reply); }
            ok = FAILURE;
            swm_error_clear(&err);
            t0 = swm_now_us();
        }
    }
    if (ok == FAILURE) {
        swm_conn *m = conn_open(c, s, 0, &reply, &err);
        if (m) {
            if (s->monitor) conn_close(s->monitor);
            s->monitor = m;
            ok = SUCCESS;
        }
    }
    if (sdam) {
        zval ev;
        if (ok == SUCCESS) swm_new_sdam_event(&ev, 4, s->host, s->port, NULL, NULL, NULL, swm_now_us() - t0, &reply, NULL);
        else swm_new_sdam_event(&ev, 5, s->host, s->port, NULL, NULL, NULL, swm_now_us() - t0, NULL, &err);
        notify_sdam(c, ok == SUCCESS ? "serverheartbeatsucceeded" : "serverheartbeatfailed", &ev);
        zval_ptr_dtor(&ev);
    }
    if (ok == SUCCESS) {
        server_update(c, s, &reply, swm_now_us() - t0);
    } else {
        server_set_unknown(c, s, err.msg ? ZSTR_VAL(err.msg) : "unknown error", 1);
    }
    if (sdam && (prev_type != s->type)) {
        zval ev, now;
        server_desc(s, &now);
        swm_new_sdam_event(&ev, 0, s->host, s->port, &c->topology_id, &now, &prev, 0, NULL, NULL);
        notify_sdam(c, "serverchanged", &ev);
        zval_ptr_dtor(&ev);
        zval_ptr_dtor(&now);
    }
    zval_ptr_dtor(&prev);
    zval_ptr_dtor(&reply);
    swm_error_clear(&err);
}

static zend_bool is_rs_member(swm_server *s)
{
    return s->type == SRV_RS_PRIMARY || s->type == SRV_RS_SECONDARY || s->type == SRV_RS_ARBITER
        || s->type == SRV_RS_OTHER || s->type == SRV_RS_GHOST;
}

static void remove_server(swm_server *s)
{
    s->removed = 1;
}

static void discover(swm_client *c, swm_server *s)
{
    zval *h;
    if (c->direct) return;
    if (is_rs_member(s) && s->type != SRV_RS_GHOST) {
        if (c->set_name && (!s->set_name || !zend_string_equals(c->set_name, s->set_name))) {
            remove_server(s);
            return;
        }
        if (!c->set_name && s->set_name) c->set_name = zend_string_copy(s->set_name);
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(s->hosts), h) {
            add_server_address(c, Z_STRVAL_P(h), Z_STRLEN_P(h));
        } ZEND_HASH_FOREACH_END();
        if (s->type == SRV_RS_PRIMARY) {
            for (int i = 0; i < c->nservers; i++) {
                swm_server *o = c->servers[i];
                zend_bool listed = 0;
                if (o == s || o->removed) continue;
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL(s->hosts), h) {
                    if (zend_string_equals_ci(Z_STR_P(h), o->address)) { listed = 1; break; }
                } ZEND_HASH_FOREACH_END();
                if (!listed) remove_server(o);
                else if (o->type == SRV_RS_PRIMARY) server_set_unknown(c, o, "stale primary", 0);
            }
        }
    } else if (s->type == SRV_STANDALONE && (c->set_name || c->nservers > 1)) {
        remove_server(s);
    }
}

static void update_topology_type(swm_client *c)
{
    int mongos = 0, rs = 0, primary = 0, standalone = 0, alive = 0;
    zend_long st = -1;
    int j = 0;
    /* compact removed servers */
    for (int i = 0; i < c->nservers; i++) {
        if (c->servers[i]->removed) {
            if (has_sdam(c)) emit_server_event(c, 2, "serverclosed", c->servers[i]);
            server_release(c->servers[i]);
            continue;
        }
        c->servers[j++] = c->servers[i];
    }
    c->nservers = j;
    for (int i = 0; i < c->nservers; i++) {
        swm_server *s = c->servers[i];
        if (s->type == SRV_UNKNOWN) continue;
        alive++;
        if (s->type == SRV_MONGOS) mongos++;
        else if (s->type == SRV_STANDALONE) standalone++;
        else if (is_rs_member(s)) rs++;
        if (s->type == SRV_RS_PRIMARY) primary++;
        if (s->type != SRV_RS_ARBITER && s->type != SRV_RS_GHOST && s->type != SRV_RS_OTHER) {
            if (s->session_timeout < 0) st = -2; /* some data bearing server lacks sessions */
            else if (st != -2 && (st == -1 || s->session_timeout < st)) st = s->session_timeout;
        }
    }
    if (st >= 0) c->session_timeout = st;
    if (c->direct) c->topo = TOPO_SINGLE;
    else if (mongos) c->topo = TOPO_SHARDED;
    else if (rs || c->set_name) c->topo = primary ? TOPO_RS_WITH_PRIMARY : TOPO_RS_NO_PRIMARY;
    else if (standalone && c->nservers == 1) c->topo = TOPO_SINGLE;
    else if (!alive) { /* keep previous type, servers are just unreachable */ }
    else c->topo = TOPO_UNKNOWN;
}

static swm_client *client_by_id(zend_long id);

/* Spawns Swoole\Coroutine::create("SwooleMongo\Driver\Manager::__swmTask", id, address).
 * address == NULL means "background topology refresh". */
static int spawn_task(swm_client *c, zend_string *address)
{
    zval args[3], fn, ret;
    int rc;
    if (!swm_in_coroutine()) return FAILURE;
    ZVAL_STRING(&fn, "Swoole\\Coroutine::create");
    ZVAL_STRING(&args[0], "SwooleMongo\\Driver\\Manager::__swmTask");
    ZVAL_LONG(&args[1], c->id);
    if (address) ZVAL_STR_COPY(&args[2], address); else ZVAL_EMPTY_STRING(&args[2]);
    rc = call_user_function(NULL, NULL, &fn, &ret, 3, args);
    zval_ptr_dtor(&fn);
    zval_ptr_dtor(&args[0]);
    zval_ptr_dtor(&args[2]);
    if (rc != SUCCESS || EG(exception) || Z_TYPE(ret) == IS_FALSE) {
        if (EG(exception)) zend_clear_exception();
        zval_ptr_dtor(&ret);
        return FAILURE;
    }
    zval_ptr_dtor(&ret);
    return SUCCESS;
}

static void topology_scan(swm_client *c)
{
    int round;
    if (c->scanning) {
        int64_t deadline = swm_now_us() + c->connect_timeout * 2 + 1000000;
        while (c->scanning && swm_now_us() < deadline) swm_sleep_us(2000);
        return;
    }
    zend_bool sdam;
    zval prev_td;
    zend_string *prev_sig = NULL;
    if (c->load_balanced) {
        /* load balanced mode: no monitoring, the single seed is the load balancer */
        for (int i = 0; i < c->nservers; i++) {
            if (c->servers[i]->type != SRV_LOAD_BALANCER) {
                c->servers[i]->type = SRV_LOAD_BALANCER;
                c->servers[i]->last_update = swm_now_us();
                if (c->servers[i]->error) { zend_string_release(c->servers[i]->error); c->servers[i]->error = NULL; }
            }
        }
        c->topo = TOPO_LOAD_BALANCED;
        c->last_scan = swm_now_us();
        c->topology_opened = 1;
        return;
    }
    c->scanning = 1;
    swm_client_addref(c);
    sdam = has_sdam(c);
    ZVAL_UNDEF(&prev_td);
    if (sdam && !c->topology_opened) {
        zval ev;
        swm_new_sdam_event(&ev, 7, NULL, 0, &c->topology_id, NULL, NULL, 0, NULL, NULL);
        notify_sdam(c, "topologyopening", &ev);
        zval_ptr_dtor(&ev);
        for (int i = 0; i < c->nservers; i++) emit_server_event(c, 1, "serveropening", c->servers[i]);
    }
    c->topology_opened = 1;
    if (sdam) { topology_desc(c, &prev_td); prev_sig = topology_signature(c); }
    for (round = 0; round < 3 && !c->closed; round++) {
        int n = c->nservers, k = 0;
        swm_server **snap = emalloc(sizeof(swm_server *) * (n + 1));
        zend_bool parallel;
        for (int i = 0; i < n; i++) {
            swm_server *s = c->servers[i];
            if (s->removed || (round > 0 && s->last_update != 0)) continue;
            server_addref(s);
            snap[k++] = s;
        }
        if (k == 0) { efree(snap); break; }
        parallel = k > 1 && swm_in_coroutine();
        if (parallel) {
            int64_t deadline;
            for (int i = 0; i < k; i++) {
                c->pending_checks++;
                if (spawn_task(c, snap[i]->address) == FAILURE) {
                    c->pending_checks--;
                    server_check(c, snap[i]);
                }
            }
            deadline = swm_now_us() + c->connect_timeout * 2 + 1000000;
            while (c->pending_checks > 0 && swm_now_us() < deadline) swm_sleep_us(1000);
        } else {
            for (int i = 0; i < k; i++) server_check(c, snap[i]);
        }
        for (int i = 0; i < k; i++) {
            if (!snap[i]->removed && !c->closed) discover(c, snap[i]);
            server_release(snap[i]);
        }
        efree(snap);
    }
    update_topology_type(c);
    c->last_scan = swm_now_us();
    c->scanning = 0;
    if (prev_sig) {
        zend_string *sig = topology_signature(c);
        if (!zend_string_equals(sig, prev_sig)) {
            zval ev, now;
            topology_desc(c, &now);
            swm_new_sdam_event(&ev, 6, NULL, 0, &c->topology_id, &now, &prev_td, 0, NULL, NULL);
            notify_sdam(c, "topologychanged", &ev);
            zval_ptr_dtor(&ev);
            zval_ptr_dtor(&now);
        }
        zend_string_release(sig);
        zend_string_release(prev_sig);
    }
    zval_ptr_dtor(&prev_td);
    swm_client_release(c);
}

/* entry point of coroutines spawned by spawn_task() */
void swm_client_task(zend_long id, zend_string *address)
{
    swm_client *c = client_by_id(id);
    if (!c) return;
    swm_client_addref(c);
    if (!address || ZSTR_LEN(address) == 0) {
        if (!c->closed) topology_scan(c);
        c->bg_scan = 0;
    } else {
        swm_server *s = find_server(c, address);
        if (s && !c->closed) {
            server_addref(s);
            server_check(c, s);
            server_release(s);
        }
        if (c->pending_checks > 0) c->pending_checks--;
    }
    swm_client_release(c);
}

/* ======================================================= server selection */
static zend_bool tags_match(zval *tagset, zval *server_tags)
{
    zend_string *k;
    zval *v;
    if (Z_TYPE_P(tagset) != IS_ARRAY) return 0;
    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(tagset), k, v) {
        zval *sv;
        if (!k) return 0;
        sv = Z_TYPE_P(server_tags) == IS_ARRAY ? zend_hash_find(Z_ARRVAL_P(server_tags), k) : NULL;
        if (!sv || Z_TYPE_P(sv) != IS_STRING || Z_TYPE_P(v) != IS_STRING || !zend_string_equals(Z_STR_P(sv), Z_STR_P(v))) return 0;
    } ZEND_HASH_FOREACH_END();
    return 1;
}

static swm_server *choose(swm_client *c, swm_server **cand, int n, readpref *rp)
{
    swm_server **sel = cand;
    swm_server **tmp = NULL;
    int ns = n;
    int64_t min_rtt = -1;
    int nwin = 0;
    swm_server *r;
    if (n == 0) return NULL;
    if (rp && rp->tags) {
        zval *set;
        tmp = emalloc(sizeof(swm_server *) * n);
        ns = 0;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(rp->tags), set) {
            ns = 0;
            for (int i = 0; i < n; i++) if (tags_match(set, &cand[i]->tags)) tmp[ns++] = cand[i];
            if (ns) break;
        } ZEND_HASH_FOREACH_END();
        sel = tmp;
        if (!ns) { efree(tmp); return NULL; }
    }
    for (int i = 0; i < ns; i++) if (min_rtt < 0 || sel[i]->rtt < min_rtt) min_rtt = sel[i]->rtt;
    {
        swm_server **win = emalloc(sizeof(swm_server *) * ns);
        for (int i = 0; i < ns; i++) if (sel[i]->rtt <= min_rtt + c->local_threshold) win[nwin++] = sel[i];
        r = win[nwin > 1 ? php_mt_rand_range(0, nwin - 1) : 0];
        efree(win);
    }
    if (tmp) efree(tmp);
    return r;
}

static swm_server *pick(swm_client *c, readpref *rp, zend_bool write)
{
    swm_server *primary = NULL, *r = NULL;
    swm_server **cand;
    int n = 0;
    if (c->nservers == 0) return NULL;
    cand = emalloc(sizeof(swm_server *) * c->nservers);
    switch (c->topo) {
        case TOPO_LOAD_BALANCED:
        case TOPO_SINGLE:
            for (int i = 0; i < c->nservers; i++) {
                if (!c->servers[i]->removed && c->servers[i]->type != SRV_UNKNOWN) { r = c->servers[i]; break; }
            }
            break;
        case TOPO_SHARDED:
            for (int i = 0; i < c->nservers; i++) if (c->servers[i]->type == SRV_MONGOS) cand[n++] = c->servers[i];
            r = choose(c, cand, n, NULL);
            break;
        case TOPO_RS_NO_PRIMARY:
        case TOPO_RS_WITH_PRIMARY:
            for (int i = 0; i < c->nservers; i++) {
                swm_server *s = c->servers[i];
                if (s->removed) continue;
                if (s->type == SRV_RS_PRIMARY) primary = s;
                else if (s->type == SRV_RS_SECONDARY) cand[n++] = s;
            }
            if (write || rp->mode == RP_PRIMARY) { r = primary; break; }
            if (rp->max_staleness > 0 && n > 0) {
                /* Server Selection spec: drop secondaries that are too stale */
                int64_t max_us = (int64_t) rp->max_staleness * 1000000, smax_write = 0;
                int m = 0;
                for (int i = 0; i < n; i++) if (cand[i]->last_write_ms > smax_write) smax_write = cand[i]->last_write_ms;
                for (int i = 0; i < n; i++) {
                    swm_server *x = cand[i];
                    int64_t staleness;
                    if (primary) {
                        staleness = (x->last_update - x->last_write_ms * 1000) - (primary->last_update - primary->last_write_ms * 1000) + c->heartbeat;
                    } else {
                        staleness = (smax_write - x->last_write_ms) * 1000 + c->heartbeat;
                    }
                    if (staleness <= max_us) cand[m++] = x;
                }
                n = m;
            }
            switch (rp->mode) {
                case RP_PRIMARY_PREFERRED: r = primary ? primary : choose(c, cand, n, rp); break;
                case RP_SECONDARY: r = choose(c, cand, n, rp); break;
                case RP_SECONDARY_PREFERRED: r = choose(c, cand, n, rp); if (!r) r = primary; break;
                case RP_NEAREST: if (primary) cand[n++] = primary; r = choose(c, cand, n, rp); break;
                default: break;
            }
            break;
        default:
            break;
    }
    efree(cand);
    return r;
}

static swm_server *select_server(swm_client *c, readpref *rp, zend_bool write, zend_string *pinned, int64_t op_deadline, swm_error *err)
{
    int64_t deadline = swm_now_us() + c->sst;
    if (op_deadline && op_deadline < deadline) deadline = op_deadline;
    zend_bool force = 0;
    for (;;) {
        swm_server *s;
        if (c->closed) {
            swm_error_set(err, SWM_ERR_LOGIC, 0, "The client has been closed");
            return NULL;
        }
        if (!write && rp->max_staleness > 0 && rp->max_staleness * 1000000 < c->heartbeat + 10000000) {
            swm_error_set(err, SWM_ERR_ARG, 0, "maxStalenessSeconds must be at least heartbeatFrequencyMS + 10 seconds");
            return NULL;
        }
        if (pinned) {
            s = find_server(c, pinned);
            if (s && s->type == SRV_UNKNOWN) s = NULL;
        } else {
            s = c->last_scan ? pick(c, rp, write) : NULL;
        }
        if (force || c->last_scan == 0 || swm_now_us() - c->last_scan >= c->heartbeat) {
            if (s && !force && c->last_scan != 0) {
                /* a usable server is known: refresh in the background instead of blocking this request */
                if (!c->scanning && !c->bg_scan) {
                    c->bg_scan = 1;
                    if (spawn_task(c, NULL) == FAILURE) {
                        c->bg_scan = 0;
                        topology_scan(c);
                    }
                }
            } else if (!(s && c->scanning)) {
                topology_scan(c);
                if (pinned) {
                    s = find_server(c, pinned);
                    if (s && s->type == SRV_UNKNOWN) s = NULL;
                } else {
                    s = pick(c, rp, write);
                }
            }
        }
        if (s) {
            server_addref(s);
            return s;
        }
        if (swm_now_us() >= deadline) {
            smart_str msg = {0};
            smart_str_appends(&msg, "No suitable server found");
            if (pinned) { smart_str_appends(&msg, " (pinned to "); smart_str_append(&msg, pinned); smart_str_appendc(&msg, ')'); }
            smart_str_appends(&msg, " for ");
            smart_str_appends(&msg, write ? "write" : rp_names[rp->mode]);
            smart_str_appends(&msg, ", topology ");
            smart_str_appends(&msg, topo_names[c->topo]);
            smart_str_appends(&msg, ":");
            for (int i = 0; i < c->nservers; i++) {
                swm_server *x = c->servers[i];
                smart_str_appends(&msg, " [");
                smart_str_append(&msg, x->address);
                smart_str_appends(&msg, " ");
                smart_str_appends(&msg, srv_type_names[x->type]);
                if (x->error) { smart_str_appends(&msg, ": "); smart_str_append(&msg, x->error); }
                smart_str_appendc(&msg, ']');
            }
            smart_str_0(&msg);
            swm_error_set(err, SWM_ERR_SELECTION, 0, "%s", ZSTR_VAL(msg.s));
            smart_str_free(&msg);
            return NULL;
        }
        swm_sleep_us(MIN(500000, deadline - swm_now_us()));
        force = 1;
    }
}

/* =================================================================== pool */
static zend_bool conn_alive(swm_conn *conn)
{
    return php_stream_set_option(conn->stream, PHP_STREAM_OPTION_CHECK_LIVENESS, -1, NULL) != PHP_STREAM_OPTION_RETURN_ERR;
}

static swm_conn *pool_checkout(swm_client *c, swm_server *s, swm_error *err)
{
    int64_t deadline = swm_now_us() + c->wait_queue_timeout;
    for (;;) {
        while (s->idle_count > 0) {
            swm_conn *conn = s->idle[--s->idle_count];
            int64_t idle = swm_now_us() - conn->last_used;
            if (conn->generation != s->generation || (c->max_idle > 0 && idle > c->max_idle) || (idle > 1000000 && !conn_alive(conn))) {
                conn_close(conn);
                s->total--;
                continue;
            }
            return conn;
        }
        if (c->max_pool <= 0 || s->total < c->max_pool) {
            uint64_t gen = s->generation;
            swm_conn *conn;
            s->total++;
            conn = conn_open(c, s, 1, NULL, err);
            if (!conn) { s->total--; return NULL; }
            conn->generation = gen;
            return conn;
        }
        if (swm_now_us() >= deadline) {
            swm_error_set(err, SWM_ERR_TIMEOUT, 0, "Timed out waiting for a connection to %s (maxPoolSize=%ld)",
                          ZSTR_VAL(s->address), (long) c->max_pool);
            return NULL;
        }
        swm_sleep_us(1000);
    }
}

static void pool_checkin(swm_client *c, swm_server *s, swm_conn *conn)
{
    if (conn->broken || conn->generation != s->generation || s->removed || c->closed) {
        conn_close(conn);
        s->total--;
        return;
    }
    conn->last_used = swm_now_us();
    if (s->idle_count == s->idle_cap) {
        s->idle_cap = s->idle_cap ? s->idle_cap * 2 : 8;
        s->idle = erealloc(s->idle, sizeof(swm_conn *) * s->idle_cap);
    }
    s->idle[s->idle_count++] = conn;
}

void swm_conn_unpin(swm_client *c, void *p, zend_bool close)
{
    swm_conn *conn = (swm_conn *) p;
    swm_server *s;
    if (!conn) return;
    s = conn->server;
    conn->pinned = 0;
    if (close) conn->broken = 1;
    server_addref(s);
    pool_checkin(c, s, conn);
    server_release(s);
}

zend_bool swm_client_load_balanced(swm_client *c)
{
    return c->load_balanced;
}

/* ============================================================ sessions */
void swm_session_pool_acquire(swm_client *c, swm_server_session *out)
{
    int64_t limit = (int64_t) (c->session_timeout - 1) * 60 * 1000000;
    while (c->sess_count > 0) {
        swm_server_session ss = c->sess_pool[--c->sess_count];
        if (swm_now_us() - ss.last_use < limit) {
            *out = ss;
            out->dirty = 0;
            return;
        }
    }
    swm_uuid4(out->id);
    out->txn_number = 0;
    out->last_use = swm_now_us();
    out->dirty = 0;
}

void swm_session_pool_release(swm_client *c, swm_server_session *ss)
{
    if (ss->dirty || c->closed) return;
    if (swm_now_us() - ss->last_use >= (int64_t) (c->session_timeout - 1) * 60 * 1000000) return;
    if (c->sess_count == c->sess_cap) {
        c->sess_cap = c->sess_cap ? c->sess_cap * 2 : 16;
        c->sess_pool = erealloc(c->sess_pool, sizeof(swm_server_session) * c->sess_cap);
    }
    c->sess_pool[c->sess_count++] = *ss;
}

static void lsid_doc(swm_server_session *ss, zval *out)
{
    zval id;
    array_init(out);
    swm_new_binary(&id, (char *) ss->id, 16, 4);
    add_assoc_zval(out, "id", &id);
}

/* ============================================================== client */
static swm_client **g_clients = NULL;
static int g_nclients = 0, g_clients_cap = 0;
static zend_long g_next_client_id = 0;

static swm_client *client_by_id(zend_long id)
{
    for (int i = 0; i < g_nclients; i++) if (g_clients[i]->id == id) return g_clients[i];
    return NULL;
}

swm_client *swm_client_create(zend_string *uri, zval *uri_options, zval *driver_options, swm_error *err)
{
    swm_client *c = ecalloc(1, sizeof(*c));
    (void) driver_options;
    c->refcount = 1;
    c->session_timeout = 30;
    array_init(&c->options);
    ZVAL_UNDEF(&c->cluster_time);
    ZVAL_UNDEF(&c->read_pref);
    ZVAL_UNDEF(&c->write_concern);
    ZVAL_UNDEF(&c->read_concern);
    ZVAL_UNDEF(&c->ctx_zv);
    array_init(&c->subscribers);
    array_init(&c->kill_queue);
    swm_new_objectid(&c->topology_id);
    c->api_strict = -1;
    c->api_deprecation = -1;
    ZVAL_UNDEF(&c->auth_props);
    ZVAL_UNDEF(&c->oidc_callback);
    if (driver_options && Z_TYPE_P(driver_options) == IS_ARRAY) {
        zval *cb = hfind(driver_options, "oidcCallback");
        if (cb && Z_TYPE_P(cb) != IS_NULL) {
            if (!zend_is_callable(cb, 0, NULL)) {
                swm_error_set(err, SWM_ERR_ARG, 0, "Expected \"oidcCallback\" driver option to be callable");
                swm_client_release(c);
                return NULL;
            }
            ZVAL_COPY(&c->oidc_callback, cb);
        }
    }
    if (driver_options && Z_TYPE_P(driver_options) == IS_ARRAY) {
        zval *api = hfind(driver_options, "serverApi");
        if (api && Z_TYPE_P(api) != IS_NULL) {
            zval *p;
            if (Z_TYPE_P(api) != IS_OBJECT || Z_OBJCE_P(api) != swm_ce_ServerApi) {
                swm_error_set(err, SWM_ERR_ARG, 0, "Expected \"serverApi\" driver option to be SwooleMongo\\Driver\\ServerApi");
                swm_client_release(c);
                return NULL;
            }
            c->api_version = zend_string_copy(Z_STR_P(OBJ_PROP_NUM(Z_OBJ_P(api), 0)));
            p = OBJ_PROP_NUM(Z_OBJ_P(api), 1);
            if (Z_TYPE_P(p) == IS_TRUE || Z_TYPE_P(p) == IS_FALSE) c->api_strict = Z_TYPE_P(p) == IS_TRUE;
            p = OBJ_PROP_NUM(Z_OBJ_P(api), 2);
            if (Z_TYPE_P(p) == IS_TRUE || Z_TYPE_P(p) == IS_FALSE) c->api_deprecation = Z_TYPE_P(p) == IS_TRUE;
        }
    }
    if (parse_uri(c, uri, uri_options, err) == FAILURE) {
        swm_client_release(c);
        return NULL;
    }
    if (driver_options && Z_TYPE_P(driver_options) == IS_ARRAY) {
        zval *ae = hfind(driver_options, "autoEncryption");
        if (ae && Z_TYPE_P(ae) != IS_NULL) {
#ifdef SWM_HAVE_MONGOCRYPT
            zval *kv = hfind(ae, "keyVaultClient");
            swm_client *kvc = NULL;
            if (Z_TYPE_P(ae) != IS_ARRAY) {
                swm_error_set(err, SWM_ERR_ARG, 0, "Expected \"autoEncryption\" driver option to be an array");
                swm_client_release(c);
                return NULL;
            }
            if (kv && Z_TYPE_P(kv) != IS_NULL) {
                if (Z_TYPE_P(kv) != IS_OBJECT || Z_OBJCE_P(kv) != swm_ce_Manager) {
                    swm_error_set(err, SWM_ERR_ARG, 0, "Expected \"keyVaultClient\" option to be SwooleMongo\\Driver\\Manager");
                    swm_client_release(c);
                    return NULL;
                }
                kvc = swm_manager_from(Z_OBJ_P(kv))->client;
            }
            c->crypt = swm_crypt_new(c, ae, 1, kvc, err);
            if (!c->crypt) { swm_client_release(c); return NULL; }
#else
            swm_error_set(err, SWM_ERR_ARG, 0, "Client-side field level encryption was not compiled in; rebuild swmongo with --with-swmongo-mongocrypt");
            swm_client_release(c);
            return NULL;
#endif
        }
    }
    if (g_nclients == g_clients_cap) {
        g_clients_cap = g_clients_cap ? g_clients_cap * 2 : 8;
        g_clients = erealloc(g_clients, sizeof(swm_client *) * g_clients_cap);
    }
    c->id = ++g_next_client_id;
    g_clients[g_nclients++] = c;
    return c;
}

void swm_client_addref(swm_client *c) { c->refcount++; }

static zend_bool g_shutting_down = 0;

void swm_client_close(swm_client *c)
{
    if (!c->closed && c->topology_opened && !g_shutting_down && has_sdam(c)) {
        zval ev;
        swm_new_sdam_event(&ev, 8, NULL, 0, &c->topology_id, NULL, NULL, 0, NULL, NULL);
        notify_sdam(c, "topologyclosed", &ev);
        zval_ptr_dtor(&ev);
    }
    c->closed = 1;
    for (int i = 0; i < c->nservers; i++) {
        swm_server *s = c->servers[i];
        server_clear_pool(s);
        if (s->monitor) { conn_close(s->monitor); s->monitor = NULL; }
    }
}

void swm_client_release(swm_client *c)
{
    if (--c->refcount > 0) return;
    swm_client_close(c);
    for (int i = 0; i < g_nclients; i++) {
        if (g_clients[i] == c) { g_clients[i] = g_clients[--g_nclients]; break; }
    }
    for (int i = 0; i < c->nservers; i++) server_release(c->servers[i]);
    if (c->servers) efree(c->servers);
    if (c->sess_pool) efree(c->sess_pool);
#define REL(x) if (x) zend_string_release(x)
    REL(c->set_name); REL(c->username); REL(c->password); REL(c->auth_source); REL(c->auth_mechanism);
    REL(c->default_db); REL(c->appname); REL(c->scram_key); REL(c->scram_salted); REL(c->api_version);
    REL(c->oidc_token); REL(c->aws_key); REL(c->aws_secret); REL(c->aws_token);
    zval_ptr_dtor(&c->auth_props);
    zval_ptr_dtor(&c->oidc_callback);
#undef REL
    zval_ptr_dtor(&c->options);
    zval_ptr_dtor(&c->read_pref);
    zval_ptr_dtor(&c->write_concern);
    zval_ptr_dtor(&c->read_concern);
    zval_ptr_dtor(&c->cluster_time);
    zval_ptr_dtor(&c->subscribers);
    zval_ptr_dtor(&c->kill_queue);
    zval_ptr_dtor(&c->topology_id);
    zval_ptr_dtor(&c->ctx_zv);
#ifdef SWM_HAVE_MONGOCRYPT
    if (c->crypt) swm_crypt_free(c->crypt);
#endif
    efree(c);
}

swm_crypt *swm_client_crypt(swm_client *c) { return c->crypt; }

zval *swm_client_encrypted_fields_map(swm_client *c)
{
#ifdef SWM_HAVE_MONGOCRYPT
    return c->crypt ? swm_crypt_encrypted_fields_map(c->crypt) : NULL;
#else
    return NULL;
#endif
}

/* Called from RSHUTDOWN: streams must be closed before the resource list is torn down. */
void swm_client_close_all(void)
{
    g_shutting_down = 1;
    for (int i = 0; i < g_nclients; i++) swm_client_close(g_clients[i]);
}

zend_string *swm_client_select(swm_client *c, zval *read_pref, zend_bool write, swm_error *err)
{
    readpref rp;
    swm_server *s;
    zend_string *r;
    if (parse_read_pref(read_pref ? read_pref : &c->read_pref, &rp, err) == FAILURE) return NULL;
    s = select_server(c, &rp, write, NULL, 0, err);
    if (!s) return NULL;
    r = zend_string_copy(s->address);
    server_release(s);
    return r;
}


static const zend_long ext_server_types[] = {0, 1, 2, 4, 5, 6, 7, 8, 9};

zend_string **swm_client_addresses(swm_client *c, int *n)
{
    zend_string **out;
    int k = 0;
    swm_error err;
    swm_error_init(&err);
    if (c->last_scan == 0 && !c->closed) topology_scan(c);
    out = emalloc(sizeof(zend_string *) * (c->nservers + 1));
    for (int i = 0; i < c->nservers; i++) {
        if (c->servers[i]->removed) continue;
        out[k++] = zend_string_copy(c->servers[i]->address);
    }
    *n = k;
    return out;
}

int swm_client_server_info(swm_client *c, zend_string *address, zval *out)
{
    swm_server *s = find_server(c, address);
    if (!s) return FAILURE;
    array_init(out);
    add_assoc_str(out, "address", zend_string_copy(s->address));
    add_assoc_str(out, "host", zend_string_copy(s->host));
    add_assoc_long(out, "port", s->port);
    add_assoc_long(out, "type", ext_server_types[s->type]);
    add_assoc_string(out, "typeName", srv_type_names[s->type]);
    add_assoc_long(out, "latency", s->type == SRV_UNKNOWN ? -1 : (zend_long) (s->rtt / 1000));
    if (Z_TYPE(s->hello) == IS_ARRAY) { Z_ADDREF(s->hello); add_assoc_zval(out, "hello", &s->hello); }
    else add_assoc_zval(out, "hello", &EG(uninitialized_zval));
    if (Z_TYPE(s->tags) == IS_ARRAY) { Z_ADDREF(s->tags); add_assoc_zval(out, "tags", &s->tags); }
    else { zval e; array_init(&e); add_assoc_zval(out, "tags", &e); }
    add_assoc_long(out, "lastUpdateMicros", (zend_long) s->last_update);
    if (s->error) add_assoc_str(out, "error", zend_string_copy(s->error));
    return SUCCESS;
}

zend_long swm_client_max_batch(swm_client *c)
{
    for (int i = 0; i < c->nservers; i++) if (c->servers[i]->type != SRV_UNKNOWN) return c->servers[i]->max_batch;
    return 100000;
}

zend_long swm_client_max_bson(swm_client *c)
{
    for (int i = 0; i < c->nservers; i++) if (c->servers[i]->type != SRV_UNKNOWN) return c->servers[i]->max_bson;
    return 16 * 1024 * 1024;
}

void swm_client_get_option(swm_client *c, const char *what, zval *out)
{
    if (!strcmp(what, "readPreference")) ZVAL_COPY(out, &c->read_pref);
    else if (!strcmp(what, "writeConcern")) { if (Z_TYPE(c->write_concern) == IS_ARRAY) ZVAL_COPY(out, &c->write_concern); else ZVAL_NULL(out); }
    else if (!strcmp(what, "readConcern")) { if (Z_TYPE(c->read_concern) == IS_ARRAY) ZVAL_COPY(out, &c->read_concern); else ZVAL_NULL(out); }
    else if (!strcmp(what, "defaultDatabase")) { if (c->default_db) ZVAL_STR_COPY(out, c->default_db); else ZVAL_NULL(out); }
    else if (!strcmp(what, "options")) {
        ZVAL_COPY(out, &c->options);
    } else if (!strcmp(what, "clusterTime")) {
        if (Z_TYPE(c->cluster_time) == IS_ARRAY) ZVAL_COPY(out, &c->cluster_time); else ZVAL_NULL(out);
    } else ZVAL_NULL(out);
}

void swm_client_add_subscriber(swm_client *c, zval *sub)
{
    zval *v;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(c->subscribers), v) {
        if (Z_OBJ_P(v) == Z_OBJ_P(sub)) return;
    } ZEND_HASH_FOREACH_END();
    Z_ADDREF_P(sub);
    add_next_index_zval(&c->subscribers, sub);
}

void swm_client_remove_subscriber(swm_client *c, zval *sub)
{
    zend_ulong idx;
    zval *v;
    ZEND_HASH_FOREACH_NUM_KEY_VAL(Z_ARRVAL(c->subscribers), idx, v) {
        if (Z_OBJ_P(v) == Z_OBJ_P(sub)) {
            zend_hash_index_del(Z_ARRVAL(c->subscribers), idx);
            return;
        }
    } ZEND_HASH_FOREACH_END();
}

void swm_client_queue_kill(swm_client *c, zend_string *server, zend_string *db, zend_string *coll, int64_t id)
{
    zval e;
    if (c->closed) return;
    array_init(&e);
    add_next_index_str(&e, zend_string_copy(server));
    add_next_index_str(&e, zend_string_copy(db));
    add_next_index_str(&e, zend_string_copy(coll));
    add_next_index_long(&e, (zend_long) id);
    add_next_index_zval(&c->kill_queue, &e);
}

static void flush_kill_queue(swm_client *c)
{
    zval q, *e;
    if (zend_hash_num_elements(Z_ARRVAL(c->kill_queue)) == 0) return;
    ZVAL_COPY_VALUE(&q, &c->kill_queue);
    array_init(&c->kill_queue);
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(q), e) {
        zval cmd, ids, idz, reply;
        swm_exec_opts o = {0};
        swm_error err;
        swm_error_init(&err);
        array_init(&cmd);
        add_assoc_str(&cmd, "killCursors", zend_string_copy(Z_STR_P(zend_hash_index_find(Z_ARRVAL_P(e), 2))));
        array_init(&ids);
        swm_new_int64(&idz, Z_LVAL_P(zend_hash_index_find(Z_ARRVAL_P(e), 3)));
        add_next_index_zval(&ids, &idz);
        add_assoc_zval(&cmd, "cursors", &ids);
        o.server = Z_STR_P(zend_hash_index_find(Z_ARRVAL_P(e), 0));
        o.no_flush = 1;
        if (swm_execute(c, Z_STR_P(zend_hash_index_find(Z_ARRVAL_P(e), 1)), &cmd, &o, &reply, NULL, NULL, &err) == SUCCESS) {
            zval_ptr_dtor(&reply);
        }
        swm_error_clear(&err);
        zval_ptr_dtor(&cmd);
    } ZEND_HASH_FOREACH_END();
    zval_ptr_dtor(&q);
}

/* ====================================================== command monitoring */
static void notify(swm_client *c, const char *method, zval *event)
{
    zval *sub;
    zval subs;
    ZVAL_COPY(&subs, &c->subscribers);
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(subs), sub) {
        zval ret;
        if (Z_TYPE_P(sub) != IS_OBJECT || !instanceof_function(Z_OBJCE_P(sub), swm_ce_CommandSubscriber)) continue;
        zend_call_method(Z_OBJ_P(sub), Z_OBJCE_P(sub), NULL, method, strlen(method), &ret, 1, event, NULL);
        zval_ptr_dtor(&ret);
        if (EG(exception)) zend_clear_exception(); /* subscribers must not break the operation */
    } ZEND_HASH_FOREACH_END();
    zval_ptr_dtor(&subs);
}

static zend_string *command_name(zval *cmd)
{
    zend_string *k = NULL;
    zend_ulong idx;
    zval *v;
    ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(cmd), idx, k, v) { (void) idx; (void) v; break; } ZEND_HASH_FOREACH_END();
    return k ? zend_string_copy(k) : ZSTR_EMPTY_ALLOC();
}

/* ============================================================== execute */
static const zend_long retryable_codes[] = {11600, 11602, 10107, 13435, 13436, 189, 91, 7, 6, 89, 9001, 262, 134, 0};
static const zend_long state_change_codes[] = {10107, 13435, 13436, 11600, 11602, 189, 91, 10058, 0};

static zend_bool code_in(zend_long code, const zend_long *list)
{
    for (int i = 0; list[i]; i++) if (list[i] == code) return 1;
    return 0;
}

static void gossip_cluster_time(swm_client *c, swm_session_obj *sess, zval *reply)
{
    zval *ct = hfind(reply, "$clusterTime"), *ts, *cur;
    zval *op = hfind(reply, "operationTime");
    if (ct && Z_TYPE_P(ct) == IS_ARRAY && (ts = hfind(ct, "clusterTime")) && Z_TYPE_P(ts) == IS_OBJECT
        && Z_OBJCE_P(ts) == swm_ce_Timestamp) {
        cur = Z_TYPE(c->cluster_time) == IS_ARRAY ? hfind(&c->cluster_time, "clusterTime") : NULL;
        if (!cur || swm_timestamp_compare(ts, cur) > 0) {
            zval_ptr_dtor(&c->cluster_time);
            ZVAL_COPY(&c->cluster_time, ct);
        }
        if (sess) {
            cur = Z_TYPE(sess->cluster_time) == IS_ARRAY ? hfind(&sess->cluster_time, "clusterTime") : NULL;
            if (!cur || swm_timestamp_compare(ts, cur) > 0) {
                zval_ptr_dtor(&sess->cluster_time);
                ZVAL_COPY(&sess->cluster_time, ct);
            }
        }
    }
    if (sess && op && Z_TYPE_P(op) == IS_OBJECT && Z_OBJCE_P(op) == swm_ce_Timestamp) {
        if (Z_TYPE(sess->operation_time) == IS_UNDEF || swm_timestamp_compare(op, &sess->operation_time) > 0) {
            zval_ptr_dtor(&sess->operation_time);
            ZVAL_COPY(&sess->operation_time, op);
        }
    }
}

static int copy_command(zval *cmd, zval *out, swm_error *err)
{
    if (Z_TYPE_P(cmd) == IS_ARRAY) {
        ZVAL_ARR(out, zend_array_dup(Z_ARRVAL_P(cmd)));
        return SUCCESS;
    }
    if (Z_TYPE_P(cmd) == IS_OBJECT) {
        if (Z_OBJCE_P(cmd) == swm_ce_Document) {
            smart_str s = {0};
            if (swm_bson_encode(&s, cmd, err) == FAILURE) { smart_str_free(&s); return FAILURE; }
            if (swm_bson_decode(ZSTR_VAL(s.s), ZSTR_LEN(s.s), out, err) == FAILURE) { smart_str_free(&s); return FAILURE; }
            smart_str_free(&s);
            return SUCCESS;
        } else {
            HashTable *props = zend_get_properties_for(cmd, ZEND_PROP_PURPOSE_JSON);
            zend_string *k;
            zval *v;
            array_init(out);
            if (props) {
                ZEND_HASH_FOREACH_STR_KEY_VAL_IND(props, k, v) {
                    if (!k || (ZSTR_LEN(k) && ZSTR_VAL(k)[0] == '\0')) continue;
                    Z_TRY_ADDREF_P(v);
                    zend_hash_update(Z_ARRVAL_P(out), k, v);
                } ZEND_HASH_FOREACH_END();
                zend_release_properties(props);
            }
            return SUCCESS;
        }
    }
    swm_error_set(err, SWM_ERR_ARG, 0, "Command must be an array or object");
    return FAILURE;
}

static void merge_read_concern(zval *final, zval *base, zval *after)
{
    zval rc;
    if (base && Z_TYPE_P(base) == IS_ARRAY) {
        ZVAL_ARR(&rc, zend_array_dup(Z_ARRVAL_P(base)));
    } else {
        array_init(&rc);
    }
    if (after) {
        Z_TRY_ADDREF_P(after);
        add_assoc_zval(&rc, "afterClusterTime", after);
    }
    if (zend_hash_num_elements(Z_ARRVAL(rc))) {
        zend_hash_str_update(Z_ARRVAL_P(final), ZEND_STRL("readConcern"), &rc);
    } else {
        zval_ptr_dtor(&rc);
    }
}

static int execute_inner(swm_client *c, zend_string *db, zval *cmd, swm_exec_opts *o, zval *reply, zend_string **raw,
                zend_string **used_server, swm_error *err);

int swm_execute(swm_client *c, zend_string *db, zval *cmd, swm_exec_opts *o, zval *reply, zend_string **raw,
                zend_string **used_server, swm_error *err)
{
#ifdef SWM_HAVE_MONGOCRYPT
    if (c->crypt && !o->no_crypt && !c->closed) {
        zval enc, *seq = o->sequences, *saved = o->sequences;
        zend_string *own_raw = NULL;
        int rc;
        /* libmongocrypt works on a single document: fold document sequences back into the command */
        if (Z_TYPE_P(cmd) == IS_ARRAY) {
            ZVAL_ARR(&enc, zend_array_dup(Z_ARRVAL_P(cmd)));
        } else {
            smart_str b = {0};
            if (swm_bson_encode(&b, cmd, err) == FAILURE) { smart_str_free(&b); return FAILURE; }
            rc = swm_bson_decode(ZSTR_VAL(b.s), ZSTR_LEN(b.s), &enc, err);
            smart_str_free(&b);
            if (rc == FAILURE) return FAILURE;
        }
        if (seq && Z_TYPE_P(seq) == IS_ARRAY) {
            zend_string *id;
            zval *docs, *d;
            ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(seq), id, docs) {
                zval list;
                if (!id || Z_TYPE_P(docs) != IS_ARRAY) continue;
                array_init(&list);
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(docs), d) {
                    zval item;
                    ZVAL_DEREF(d);
                    if (Z_TYPE_P(d) == IS_STRING) swm_new_document_raw(&item, swm_ce_Document, Z_STRVAL_P(d), Z_STRLEN_P(d));
                    else ZVAL_COPY(&item, d);
                    add_next_index_zval(&list, &item);
                } ZEND_HASH_FOREACH_END();
                zend_hash_update(Z_ARRVAL(enc), id, &list);
            } ZEND_HASH_FOREACH_END();
        }
        if (swm_crypt_auto_encrypt(c->crypt, db, &enc, err) == FAILURE) { zval_ptr_dtor(&enc); return FAILURE; }
        o->sequences = NULL;
        rc = execute_inner(c, db, &enc, o, reply, raw ? raw : &own_raw, used_server, err);
        o->sequences = saved;
        zval_ptr_dtor(&enc);
        if (rc == SUCCESS) {
            rc = swm_crypt_auto_decrypt(c->crypt, reply, raw ? raw : &own_raw, err);
            if (rc == FAILURE) { zval_ptr_dtor(reply); ZVAL_UNDEF(reply); }
        }
        if (own_raw) zend_string_release(own_raw);
        return rc;
    }
#endif
    return execute_inner(c, db, cmd, o, reply, raw, used_server, err);
}

static int execute_inner(swm_client *c, zend_string *db, zval *cmd, swm_exec_opts *o, zval *reply, zend_string **raw,
                zend_string **used_server, swm_error *err)
{
    swm_session_obj *sess = o->session ? swm_session_from(Z_OBJ_P(o->session)) : NULL;
    swm_server_session implicit;
    zend_bool have_implicit = 0, use_txn = 0, retryable, in_txn, write = o->write;
    int attempts = 0;
    int64_t txn_number = 0;
    readpref rp;
    zend_string *cmd_name = NULL;
    int64_t timeout = o->timeout_us > 0 ? o->timeout_us : c->timeout, deadline = timeout ? swm_now_us() + timeout : 0;
    zend_bool unack = 0;

    ZVAL_UNDEF(reply);
    if (c->closed) {
        swm_error_set(err, SWM_ERR_LOGIC, 0, "The client has been closed");
        return FAILURE;
    }
    if (sess && sess->ended) {
        swm_error_set(err, SWM_ERR_LOGIC, 0, "Cannot use a session that has ended");
        return FAILURE;
    }
    if (sess && sess->client != c) {
        swm_error_set(err, SWM_ERR_ARG, 0, "Session was created by a different Manager");
        return FAILURE;
    }
    if (sess && !o->txn_control && (sess->state == SWM_TXN_COMMITTED || sess->state == SWM_TXN_ABORTED)) {
        sess->state = SWM_TXN_NONE;
        if (sess->pinned) { zend_string_release(sess->pinned); sess->pinned = NULL; }
    }
    in_txn = sess && (sess->state == SWM_TXN_STARTING || sess->state == SWM_TXN_IN_PROGRESS);
    if (in_txn || o->txn_control) {
        zval *trp = in_txn ? hfind(&sess->txn_opts, "readPreference") : NULL;
        if (parse_read_pref(trp, &rp, err) == FAILURE) return FAILURE;
        if (in_txn && !write && rp.mode != RP_PRIMARY && !o->txn_control) {
            /* reads in a transaction must use the transaction read preference */
        }
        if (o->txn_control) rp.mode = RP_PRIMARY;
    } else if (write) {
        rp.mode = RP_PRIMARY; rp.tags = NULL; rp.max_staleness = -1;
    } else if (parse_read_pref(o->read_pref ? o->read_pref : &c->read_pref, &rp, err) == FAILURE) {
        return FAILURE;
    }
    retryable = o->retryable && (write ? c->retry_writes : c->retry_reads);
    if (in_txn && !o->txn_control) retryable = 0;
    if (o->txn_control) retryable = c->retry_writes;
    if (write && !in_txn && !o->txn_control && Z_TYPE_P(cmd) == IS_ARRAY) {
        zval *wcv = hfind(cmd, "writeConcern"), *wv, *jv;
        if (wcv && (wv = hfind(wcv, "w")) && Z_TYPE_P(wv) == IS_LONG && Z_LVAL_P(wv) == 0
            && !((jv = hfind(wcv, "j")) && zend_is_true(jv))) {
            if (sess) {
                swm_error_set(err, SWM_ERR_ARG, 0, "Cannot use an explicit session with an unacknowledged write concern");
                return FAILURE;
            }
            unack = 1;
            retryable = 0;
        }
    }
    if (sess && sess->snapshot && write) {
        swm_error_set(err, SWM_ERR_ARG, 0, "Write operations are not supported in snapshot sessions");
        return FAILURE;
    }

    if (!o->no_flush) flush_kill_queue(c);

    for (;;) {
        swm_server *s;
        swm_conn *conn;
        zval final;
        zend_string *pinned = o->server ? o->server : (sess && (in_txn || o->txn_control) ? sess->pinned : NULL);
        int64_t t0, op_id;
        int rc;
        zend_bool pinned_use = 0;
        zend_bool network = 0;
        zval *labels_src = NULL;

        s = NULL;
        if (!pinned && o->prefer && attempts == 0) {
            s = find_server(c, o->prefer);
            if (s && s->type != SRV_UNKNOWN) server_addref(s); else s = NULL;
        }
        if (!s) s = select_server(c, &rp, write || in_txn, pinned, deadline, err);
        if (!s) {
            if (in_txn && !o->txn_control) swm_error_add_label(err, "TransientTransactionError");
            if (o->txn_control && cmd_name && zend_string_equals_literal(cmd_name, "commitTransaction")) {
                swm_error_add_label(err, "UnknownTransactionCommitResult");
            }
            goto fail;
        }
        if (write && retryable && !o->txn_control && (s->type == SRV_STANDALONE || s->session_timeout < 0 || s->max_wire < 6)) {
            retryable = 0;
        }
        if (write && retryable && !o->txn_control && !use_txn) {
            swm_server_session *ss;
            if (!sess) { swm_session_pool_acquire(c, &implicit); have_implicit = 1; ss = &implicit; }
            else ss = &sess->ss;
            txn_number = ++ss->txn_number;
            use_txn = 1;
        }
        if (copy_command(cmd, &final, err) == FAILURE) { server_release(s); goto fail; }
        if (!cmd_name) cmd_name = command_name(&final);

        /* session / transaction fields */
        if ((sess || have_implicit) && s->session_timeout >= 0) {
            zval lsid;
            lsid_doc(sess ? &sess->ss : &implicit, &lsid);
            add_assoc_zval(&final, "lsid", &lsid);
            (sess ? &sess->ss : &implicit)->last_use = swm_now_us();
        }
        if (sess && (in_txn || o->txn_control)) {
            zval tn;
            swm_new_int64(&tn, sess->ss.txn_number);
            add_assoc_zval(&final, "txnNumber", &tn);
            add_assoc_bool(&final, "autocommit", 0);
            if (sess->state == SWM_TXN_STARTING) {
                zval *after = sess->causal && Z_TYPE(sess->operation_time) != IS_UNDEF ? &sess->operation_time : NULL;
                zval *trc = hfind(&sess->txn_opts, "readConcern");
                zend_hash_str_del(Z_ARRVAL(final), ZEND_STRL("readConcern"));
                add_assoc_bool(&final, "startTransaction", 1);
                merge_read_concern(&final, trc, after);
            } else {
                zend_hash_str_del(Z_ARRVAL(final), ZEND_STRL("readConcern"));
            }
            if (!o->txn_control) zend_hash_str_del(Z_ARRVAL(final), ZEND_STRL("writeConcern"));
            if (o->txn_control && attempts > 0 && zend_string_equals_literal(cmd_name, "commitTransaction")) {
                zval wc, *old = hfind(&final, "writeConcern");
                if (old && Z_TYPE_P(old) == IS_ARRAY) ZVAL_ARR(&wc, zend_array_dup(Z_ARRVAL_P(old))); else array_init(&wc);
                add_assoc_string(&wc, "w", "majority");
                if (!hfind(&wc, "wtimeout")) add_assoc_long(&wc, "wtimeout", 10000);
                zend_hash_str_update(Z_ARRVAL(final), ZEND_STRL("writeConcern"), &wc);
            }
        } else {
            if (use_txn) {
                zval tn;
                swm_new_int64(&tn, txn_number);
                add_assoc_zval(&final, "txnNumber", &tn);
            }
            if (sess && sess->snapshot && o->causal_read) {
                zval rcz;
                array_init(&rcz);
                add_assoc_string(&rcz, "level", "snapshot");
                if (Z_TYPE(sess->snapshot_time) != IS_UNDEF) { Z_TRY_ADDREF(sess->snapshot_time); add_assoc_zval(&rcz, "atClusterTime", &sess->snapshot_time); }
                zend_hash_str_update(Z_ARRVAL(final), ZEND_STRL("readConcern"), &rcz);
            } else if (sess && sess->causal && o->causal_read && Z_TYPE(sess->operation_time) != IS_UNDEF) {
                merge_read_concern(&final, hfind(&final, "readConcern"), &sess->operation_time);
            }
        }
        if (s->type != SRV_STANDALONE && s->max_wire >= 6) {
            zval *ct = &c->cluster_time;
            if (sess && Z_TYPE(sess->cluster_time) == IS_ARRAY) {
                zval *a = Z_TYPE(c->cluster_time) == IS_ARRAY ? hfind(&c->cluster_time, "clusterTime") : NULL;
                zval *b = hfind(&sess->cluster_time, "clusterTime");
                if (!a || (b && swm_timestamp_compare(b, a) > 0)) ct = &sess->cluster_time;
            }
            if (Z_TYPE_P(ct) == IS_ARRAY) {
                Z_ADDREF_P(ct);
                add_assoc_zval(&final, "$clusterTime", ct);
            }
        }
        if (!write && !in_txn && !o->txn_control) {
            if (c->topo == TOPO_SINGLE && s->type != SRV_MONGOS) {
                zval d;
                array_init(&d);
                add_assoc_string(&d, "mode", rp.mode == RP_PRIMARY ? "primaryPreferred" : (char *) rp_names[rp.mode]);
                add_assoc_zval(&final, "$readPreference", &d);
            } else if (rp.mode != RP_PRIMARY && s->type != SRV_STANDALONE) {
                zval d;
                read_pref_doc(&rp, &d);
                add_assoc_zval(&final, "$readPreference", &d);
            }
        }
        if (c->api_version && !zend_string_equals_literal(cmd_name, "getMore")) {
            add_assoc_str(&final, "apiVersion", zend_string_copy(c->api_version));
            if (c->api_strict >= 0) add_assoc_bool(&final, "apiStrict", c->api_strict);
            if (c->api_deprecation >= 0) add_assoc_bool(&final, "apiDeprecationErrors", c->api_deprecation);
        }
        if (deadline && !zend_string_equals_literal(cmd_name, "getMore") && !hfind(&final, "maxTimeMS")) {
            int64_t rem = deadline - swm_now_us() - s->rtt;
            if (rem > 1000) add_assoc_long(&final, "maxTimeMS", (zend_long) (rem / 1000));
        }
        if (sess && in_txn && !o->txn_control && sess->state == SWM_TXN_STARTING) {
            sess->state = SWM_TXN_IN_PROGRESS;
            if (s->type == SRV_MONGOS && !sess->pinned) sess->pinned = zend_string_copy(s->address);
        }

        t0 = swm_now_us();
        op_id = ++g_operation_id;
        add_assoc_str(&final, "$db", zend_string_copy(db));
        if (zend_hash_num_elements(Z_ARRVAL(c->subscribers))) {
            zval ev;
            swm_new_command_event(&ev, 0, cmd_name, db, &final, s->address, 0, op_id, op_id, NULL);
            notify(c, "commandstarted", &ev);
            zval_ptr_dtor(&ev);
        }

        {
            swm_conn *use = (swm_conn *) o->use_conn;
            if (!use && sess && sess->pinned_conn && (in_txn || o->txn_control)) use = (swm_conn *) sess->pinned_conn;
            conn = use ? use : pool_checkout(c, s, err);
            pinned_use = use != NULL;
        }
        if (!conn) {
            rc = FAILURE;
            network = err->kind == SWM_ERR_NETWORK || err->kind == SWM_ERR_TIMEOUT;
        } else {
            conn->more_to_come_next = unack;
            conn->deadline = deadline;
            rc = conn_command(c, conn, ZSTR_VAL(db), &final, o->sequences, reply, raw, 1, err);
            if (rc == SUCCESS && !reply_ok(reply) && zv_long(hfind(reply, "code"), 0) == 391
                && c->auth_mechanism && zend_string_equals_literal_ci(c->auth_mechanism, "MONGODB-OIDC")) {
                /* ReauthenticationRequired: refresh the token, re-authenticate this connection, retry once */
                swm_error reauth;
                swm_error_init(&reauth);
                if (c->oidc_token) { zend_string_release(c->oidc_token); c->oidc_token = NULL; }
                zval_ptr_dtor(reply);
                ZVAL_UNDEF(reply);
                if (raw && *raw) { zend_string_release(*raw); *raw = NULL; }
                if (auth_oidc(c, conn, &reauth) == SUCCESS) {
                    rc = conn_command(c, conn, ZSTR_VAL(db), &final, o->sequences, reply, raw, 1, err);
                } else {
                    rc = FAILURE;
                    swm_error_clear(err);
                    *err = reauth;
                }
            }
            if (rc == FAILURE) network = conn->broken;
            if (pinned_use) {
                if (conn->broken) {
                    /* the pinned connection is gone: release it, the owner must not use it again */
                    if (sess && sess->pinned_conn == conn) sess->pinned_conn = NULL;
                    swm_conn_unpin(c, conn, 1);
                    o->pinned_dead = 1;
                } else if (o->txn_control && sess && sess->pinned_conn == conn) {
                    sess->pinned_conn = NULL;
                    swm_conn_unpin(c, conn, 0);
                }
            } else if (c->load_balanced && rc == SUCCESS && reply_ok(reply) && sess && in_txn && !o->txn_control && !sess->pinned_conn) {
                conn->pinned = 1;
                sess->pinned_conn = conn;
            } else if (c->load_balanced && rc == SUCCESS && o->pin_out && reply_ok(reply)
                       && hfind(hfind(reply, "cursor"), "id") && zv_long(hfind(hfind(reply, "cursor"), "id"), 0) != 0) {
                conn->pinned = 1;
                *o->pin_out = conn;
            } else {
                pool_checkin(c, s, conn);
            }
        }
        zval_ptr_dtor(&final);

        if (rc == SUCCESS) {
            gossip_cluster_time(c, sess, reply);
            if (sess && sess->snapshot && Z_TYPE(sess->snapshot_time) == IS_UNDEF) {
                zval *at = hfind(hfind(reply, "cursor"), "atClusterTime");
                if (!at) at = hfind(reply, "atClusterTime");
                if (at && Z_TYPE_P(at) == IS_OBJECT && Z_OBJCE_P(at) == swm_ce_Timestamp) ZVAL_COPY(&sess->snapshot_time, at);
            }
            if (reply_ok(reply)) {
                zval *wce = hfind(reply, "writeConcernError");
                zval *lbl = hfind(reply, "errorLabels");
                zend_bool wce_retry = 0;
                if (wce && retryable && write && attempts == 0) {
                    zend_long code = zv_long(hfind(wce, "code"), 0);
                    zval *v;
                    if (code_in(code, retryable_codes)) wce_retry = 1;
                    if (lbl && Z_TYPE_P(lbl) == IS_ARRAY) {
                        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(lbl), v) {
                            if (Z_TYPE_P(v) == IS_STRING && !strcmp(Z_STRVAL_P(v), "RetryableWriteError")) wce_retry = 1;
                        } ZEND_HASH_FOREACH_END();
                    }
                }
                if (!wce_retry) {
                    if (zend_hash_num_elements(Z_ARRVAL(c->subscribers))) {
                        zval ev;
                        swm_new_command_event(&ev, 1, cmd_name, db, reply, s->address, swm_now_us() - t0, op_id, op_id, NULL);
                        notify(c, "commandsucceeded", &ev);
                        zval_ptr_dtor(&ev);
                    }
                    if (used_server) *used_server = zend_string_copy(s->address);
                    server_release(s);
                    if (have_implicit) swm_session_pool_release(c, &implicit);
                    if (cmd_name) zend_string_release(cmd_name);
                    return SUCCESS;
                }
                swm_error_set(err, SWM_ERR_SERVER, zv_long(hfind(wce, "code"), 0), "Write concern error");
                ZVAL_COPY(&err->reply, reply);
                swm_error_add_label(err, "RetryableWriteError");
            } else {
                error_from_reply(err, reply);
            }
            zval_ptr_dtor(reply);
            ZVAL_UNDEF(reply);
            if (raw && *raw) { zend_string_release(*raw); *raw = NULL; }
        }

        if (zend_hash_num_elements(Z_ARRVAL(c->subscribers))) {
            zval ev;
            swm_new_command_event(&ev, 2, cmd_name, db, Z_TYPE(err->reply) == IS_ARRAY ? &err->reply : NULL, s->address,
                                  swm_now_us() - t0, op_id, op_id, err);
            notify(c, "commandfailed", &ev);
            zval_ptr_dtor(&ev);
        }

        (void) labels_src;
        if (network) {
            if (!c->load_balanced) server_set_unknown(c, s, err->msg ? ZSTR_VAL(err->msg) : "network error", 1);
            if (sess) sess->ss.dirty = 1;
            if (have_implicit) implicit.dirty = 1;
            if (in_txn && !o->txn_control) swm_error_add_label(err, "TransientTransactionError");
            if (retryable && write) swm_error_add_label(err, "RetryableWriteError");
        } else if (!c->load_balanced && err->kind == SWM_ERR_SERVER && code_in(err->code, state_change_codes)) {
            server_set_unknown(c, s, err->msg ? ZSTR_VAL(err->msg) : "state change", err->code == 91 || err->code == 11600);
        }
        if (o->txn_control && zend_string_equals_literal(cmd_name, "commitTransaction")
            && (network || err->kind == SWM_ERR_SELECTION || (err->kind == SWM_ERR_SERVER
                && (code_in(err->code, retryable_codes) || err->code == 50 || swm_error_has_label(err, "RetryableWriteError"))))) {
            swm_error_add_label(err, "UnknownTransactionCommitResult");
        }
        server_release(s);

        if (!retryable || attempts > 0) goto fail;
        if (deadline && swm_now_us() >= deadline) goto fail;
        if (!network && !(err->kind == SWM_ERR_SERVER
                          && (code_in(err->code, retryable_codes) || swm_error_has_label(err, "RetryableWriteError")))) {
            goto fail;
        }
        /* a retryable write against a server that adds labels itself must carry the label */
        if (write && err->kind == SWM_ERR_SERVER && !network && !code_in(err->code, retryable_codes)
            && !swm_error_has_label(err, "RetryableWriteError")) {
            goto fail;
        }
        attempts++;
        swm_error_clear(err);
    }
fail:
    if (have_implicit) swm_session_pool_release(c, &implicit);
    if (cmd_name) zend_string_release(cmd_name);
    return FAILURE;
}
