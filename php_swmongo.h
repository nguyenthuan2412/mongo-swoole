/*
 * swmongo - coroutine friendly MongoDB driver for PHP / Swoole.
 *
 * Class layout mirrors the official stack (ext-mongodb + mongodb/mongodb)
 * under the SwooleMongo namespace:
 *   SwooleMongo\BSON\*                      BSON types, Document, PackedArray
 *   SwooleMongo\Driver\*                    Manager, Server, Command, Query, BulkWrite, Cursor, Session, ...
 *   SwooleMongo\Driver\Exception\*          exception hierarchy
 *   SwooleMongo\Driver\Monitoring\*         command monitoring
 *   SwooleMongo\{Client,Database,...}       high level library (embedded PHP, see library/)
 *
 * All network I/O goes through php_stream_xport_create(), so when Swoole's
 * runtime hook (SWOOLE_HOOK_TCP / UNIX / SSL) is enabled every socket
 * operation transparently yields to the Swoole scheduler.
 */
#ifndef PHP_SWMONGO_H
#define PHP_SWMONGO_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "php_network.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"
#include "zend_smart_str.h"
#include "zend_enum.h"
#include "ext/spl/spl_exceptions.h"
#include "ext/spl/spl_array.h"
#include "ext/json/php_json.h"
#include "ext/date/php_date.h"

#define PHP_SWMONGO_VERSION "0.2.0"
#define SWM_DRIVER_NAME "swmongo"

extern zend_module_entry swmongo_module_entry;
#define phpext_swmongo_ptr &swmongo_module_entry

ZEND_BEGIN_MODULE_GLOBALS(swmongo)
    zend_bool enable_library;
    zend_bool warn_blocking;
ZEND_END_MODULE_GLOBALS(swmongo)

ZEND_EXTERN_MODULE_GLOBALS(swmongo)
#define SWMG(v) ZEND_MODULE_GLOBALS_ACCESSOR(swmongo, v)

/* --------------------------------------------- SwooleMongo\Driver\Exception */
extern zend_class_entry *swm_ce_Exception;                 /* interface */
extern zend_class_entry *swm_ce_RuntimeException;
extern zend_class_entry *swm_ce_InvalidArgumentException;
extern zend_class_entry *swm_ce_LogicException;
extern zend_class_entry *swm_ce_UnexpectedValueException;
extern zend_class_entry *swm_ce_ConnectionException;
extern zend_class_entry *swm_ce_ConnectionTimeoutException;
extern zend_class_entry *swm_ce_AuthenticationException;
extern zend_class_entry *swm_ce_ServerException;
extern zend_class_entry *swm_ce_CommandException;
extern zend_class_entry *swm_ce_ExecutionTimeoutException;
extern zend_class_entry *swm_ce_WriteException;
extern zend_class_entry *swm_ce_BulkWriteException;
extern zend_class_entry *swm_ce_EncryptionException;

/* ------------------------------------------------------- SwooleMongo\BSON */
extern zend_class_entry *swm_ce_Type;
extern zend_class_entry *swm_ce_Serializable;
extern zend_class_entry *swm_ce_Unserializable;
extern zend_class_entry *swm_ce_Persistable;
extern zend_class_entry *swm_ce_ObjectId;
extern zend_class_entry *swm_ce_UTCDateTime;
extern zend_class_entry *swm_ce_Binary;
extern zend_class_entry *swm_ce_Regex;
extern zend_class_entry *swm_ce_Timestamp;
extern zend_class_entry *swm_ce_Int64;
extern zend_class_entry *swm_ce_Decimal128;
extern zend_class_entry *swm_ce_MinKey;
extern zend_class_entry *swm_ce_MaxKey;
extern zend_class_entry *swm_ce_Javascript;
extern zend_class_entry *swm_ce_Document;
extern zend_class_entry *swm_ce_PackedArray;

/* ----------------------------------------------------- SwooleMongo\Driver */
extern zend_class_entry *swm_ce_Manager;
extern zend_class_entry *swm_ce_Server;
extern zend_class_entry *swm_ce_Command;
extern zend_class_entry *swm_ce_Query;
extern zend_class_entry *swm_ce_BulkWrite;
extern zend_class_entry *swm_ce_WriteResult;
extern zend_class_entry *swm_ce_WriteError;
extern zend_class_entry *swm_ce_WriteConcernError;
extern zend_class_entry *swm_ce_Cursor;
extern zend_class_entry *swm_ce_CursorInterface;
extern zend_class_entry *swm_ce_Session;
extern zend_class_entry *swm_ce_ReadPreference;
extern zend_class_entry *swm_ce_ReadConcern;
extern zend_class_entry *swm_ce_WriteConcern;
extern zend_class_entry *swm_ce_ServerApi;
extern zend_class_entry *swm_ce_Subscriber;
extern zend_class_entry *swm_ce_CommandSubscriber;
extern zend_class_entry *swm_ce_CommandStartedEvent;
extern zend_class_entry *swm_ce_CommandSucceededEvent;
extern zend_class_entry *swm_ce_CommandFailedEvent;

/* ------------------------------------------------------------------ errors */
typedef enum {
    SWM_ERR_NONE = 0,
    SWM_ERR_ARG,
    SWM_ERR_LOGIC,
    SWM_ERR_BSON,
    SWM_ERR_NETWORK,
    SWM_ERR_TIMEOUT,
    SWM_ERR_SELECTION,
    SWM_ERR_AUTH,
    SWM_ERR_SERVER,
    SWM_ERR_PROTOCOL,
    SWM_ERR_RUNTIME_CRYPT,
} swm_err_kind;

typedef struct {
    swm_err_kind kind;
    zend_long code;
    zend_string *msg;
    zval reply;  /* server reply (array) or UNDEF */
    zval labels; /* array of strings or UNDEF */
} swm_error;

void swm_error_init(swm_error *e);
void swm_error_clear(swm_error *e);
void swm_error_set(swm_error *e, swm_err_kind kind, zend_long code, const char *fmt, ...) ZEND_ATTRIBUTE_FORMAT(printf, 4, 5);
void swm_error_add_label(swm_error *e, const char *label);
zend_bool swm_error_has_label(swm_error *e, const char *label);
void swm_error_to_exception(swm_error *e, zval *out); /* builds (does not throw) */
void swm_error_throw(swm_error *e);                  /* throws and clears */
void swm_throw(zend_class_entry *ce, const char *fmt, ...) ZEND_ATTRIBUTE_FORMAT(printf, 2, 3);

/* ----------------------------------------------------------------- typeMap */
typedef enum {
    SWM_TM_DEFAULT = 0, /* documents -> stdClass (honours __pclass), arrays -> array */
    SWM_TM_ARRAY,
    SWM_TM_OBJECT,
    SWM_TM_BSON,        /* Document / PackedArray */
    SWM_TM_CLASS,       /* class implementing Unserializable */
} swm_tm_kind;

typedef struct {
    swm_tm_kind kind;
    zend_class_entry *ce;
} swm_tm_entry;

typedef struct {
    zend_string **segs;
    int nsegs;
    swm_tm_entry entry;
} swm_tm_path;

typedef struct {
    swm_tm_entry root, document, array;
    swm_tm_path *paths;
    int npaths;
} swm_typemap;

void swm_typemap_init(swm_typemap *tm);
int swm_typemap_parse(zval *arr, swm_typemap *tm, swm_error *err);
void swm_typemap_dtor(swm_typemap *tm);

/* -------------------------------------------------------------------- BSON */
int swm_bson_encode(smart_str *out, zval *doc, swm_error *err);
/* internal decoding: every document / array becomes a PHP array */
int swm_bson_decode(const char *data, size_t len, zval *out, swm_error *err);
int swm_bson_decode_one(const char *data, size_t len, size_t *used, zval *out, swm_error *err);
/* user facing decoding honouring a typeMap (NULL = default typeMap) */
int swm_bson_decode_tm(const char *data, size_t len, swm_typemap *tm, zval *out, swm_error *err);
/* finds a top level element; returns its type (0 if missing) and value slice */
uint8_t swm_bson_find(const char *data, size_t len, const char *key, const char **val, size_t *vlen);
/* splits a BSON array/document (validated) into a PHP list of raw document strings */
void swm_bson_split(const char *data, size_t len, zval *out);
/* re-encodes an internal array as a default-typemap value (stdClass) */
void swm_array_to_object_doc(zval *arr, zval *out);

void swm_bson_minit(void);
void swm_new_binary(zval *out, const char *data, size_t len, zend_long subtype);
void swm_new_int64(zval *out, int64_t v);
void swm_new_timestamp(zval *out, uint32_t inc, uint32_t ts);
void swm_new_objectid(zval *out);
void swm_new_document_raw(zval *out, zend_class_entry *ce, const char *data, size_t len);
zend_string *swm_binary_data(zval *bin); /* borrowed, NULL if not a Binary */
int64_t swm_zval_to_int64(zval *v, zend_bool *ok); /* int / Int64 / float */
void swm_uuid4(uint8_t out[16]);
int swm_timestamp_compare(zval *a, zval *b);

/* ------------------------------------------------------------------ client */
typedef struct _swm_client swm_client;
typedef struct _swm_server swm_server;

typedef struct {
    uint8_t id[16];
    int64_t txn_number;
    int64_t last_use; /* microseconds */
    zend_bool dirty;
} swm_server_session;

typedef enum {
    SWM_TXN_NONE = 0,
    SWM_TXN_STARTING,
    SWM_TXN_IN_PROGRESS,
    SWM_TXN_COMMITTED,
    SWM_TXN_ABORTED,
} swm_txn_state;

typedef struct {
    swm_client *client;
    swm_server_session ss;
    zend_bool ended;
    zend_bool causal;
    swm_txn_state state;
    zend_bool committed_empty;
    zval default_txn_opts; /* internal array form */
    zval txn_opts;         /* internal array form (readConcern/writeConcern arrays, readPreference) */
    zval txn_opts_user;    /* as passed by the user */
    zval operation_time;   /* Timestamp or UNDEF */
    zval cluster_time;     /* array or UNDEF */
    zend_string *pinned;   /* pinned mongos address */
    zend_bool snapshot;
    zval snapshot_time;    /* atClusterTime of the first snapshot read */
    void *pinned_conn;     /* load balanced mode: connection pinned to the transaction */
    zend_object std;
} swm_session_obj;

typedef struct {
    swm_client *client;
    zend_string *server;
    zend_string *db;
    zend_string *coll;
    int64_t id;
    zval batch;   /* list of raw BSON document strings */
    uint32_t pos;
    zval current; /* decoded current document */
    zend_long key;
    zend_long batch_size;
    zend_long max_await;
    zend_bool tailable;
    zend_bool advanced;
    zend_bool started;
    zend_bool has_tm;
    swm_typemap tm;
    zval session; /* Session object or UNDEF */
    zval comment;
    zval pbrt;    /* postBatchResumeToken (internal array) */
    int64_t timeout_us;
    void *pinned_conn;   /* load balanced mode */
    zend_object std;
} swm_cursor_obj;

typedef struct {
    swm_client *client;
    zend_object std;
} swm_manager_obj;

typedef struct {
    swm_client *client;
    zend_string *address;
    zend_object std;
} swm_server_obj;

static inline swm_session_obj *swm_session_from(zend_object *o) {
    return (swm_session_obj *) ((char *) o - XtOffsetOf(swm_session_obj, std));
}
static inline swm_cursor_obj *swm_cursor_from(zend_object *o) {
    return (swm_cursor_obj *) ((char *) o - XtOffsetOf(swm_cursor_obj, std));
}
static inline swm_manager_obj *swm_manager_from(zend_object *o) {
    return (swm_manager_obj *) ((char *) o - XtOffsetOf(swm_manager_obj, std));
}
static inline swm_server_obj *swm_server_from(zend_object *o) {
    return (swm_server_obj *) ((char *) o - XtOffsetOf(swm_server_obj, std));
}

typedef struct {
    zval *read_pref;     /* ReadPreference object / array / string or NULL */
    zval *session;       /* Session object or NULL */
    zval *sequences;     /* array identifier => list of docs (array/object/encoded string) */
    zend_string *server; /* force this server (getMore etc.) */
    zend_string *prefer; /* try this server first, reselect on retry (Server::execute*) */
    zend_bool write;
    zend_bool retryable;
    zend_bool causal_read;
    zend_bool txn_control; /* commitTransaction / abortTransaction */
    zend_bool no_flush;
    int64_t timeout_us;  /* timeoutMS (CSOT) for this operation, 0 = client default */
    void *use_conn;      /* load balanced mode: run on this pinned connection */
    void **pin_out;      /* load balanced mode: keep the connection when a cursor stays open */
    zend_bool pinned_dead; /* out: the pinned connection was closed because of an error */
    zend_bool no_crypt;  /* skip automatic encryption / decryption */
} swm_exec_opts;

swm_client *swm_client_create(zend_string *uri, zval *uri_options, zval *driver_options, swm_error *err);
void swm_client_addref(swm_client *c);
void swm_client_release(swm_client *c);
void swm_client_close(swm_client *c);
void swm_client_close_all(void);

/* reply: decoded internal array. raw (optional): the reply document bytes */
int swm_execute(swm_client *c, zend_string *db, zval *cmd, swm_exec_opts *o, zval *reply, zend_string **raw,
                zend_string **used_server, swm_error *err);
zend_string *swm_client_select(swm_client *c, zval *read_pref, zend_bool write, swm_error *err);
zend_string **swm_client_addresses(swm_client *c, int *n);
/* fills a description of the server (type, hello, rtt, tags...); returns FAILURE if unknown address */
int swm_client_server_info(swm_client *c, zend_string *address, zval *out);
void swm_client_get_option(swm_client *c, const char *what, zval *out);
void swm_client_add_subscriber(swm_client *c, zval *sub);
void swm_client_remove_subscriber(swm_client *c, zval *sub);
void swm_client_task(zend_long id, zend_string *address);
void swm_conn_unpin(swm_client *c, void *conn, zend_bool close);
zend_bool swm_client_load_balanced(swm_client *c);
void swm_client_queue_kill(swm_client *c, zend_string *server, zend_string *db, zend_string *coll, int64_t id);
zend_long swm_client_max_batch(swm_client *c);
zend_long swm_client_max_bson(swm_client *c);

/* CSFLE / Queryable Encryption (swm_crypt.c) */
typedef struct _swm_crypt swm_crypt;
extern zend_class_entry *swm_ce_ClientEncryption;
void swm_crypt_minit(void);
#ifdef SWM_HAVE_MONGOCRYPT
swm_crypt *swm_crypt_new(swm_client *owner, zval *opts, zend_bool auto_mode, swm_client *keyvault, swm_error *err);
void swm_crypt_free(swm_crypt *cr);
zend_bool swm_crypt_bypass_auto(swm_crypt *cr);
zval *swm_crypt_encrypted_fields_map(swm_crypt *cr);
int swm_crypt_auto_encrypt(swm_crypt *cr, zend_string *db, zval *cmd, swm_error *err);
int swm_crypt_auto_decrypt(swm_crypt *cr, zval *reply, zend_string **raw, swm_error *err);
void swm_client_encryption_init(zval *obj, swm_client *keyvault, zval *options, swm_error *err);
#endif
swm_crypt *swm_client_crypt(swm_client *c);
zval *swm_client_encrypted_fields_map(swm_client *c);

void swm_session_pool_acquire(swm_client *c, swm_server_session *out);
void swm_session_pool_release(swm_client *c, swm_server_session *ss);

/* monitoring events: kind 0 = started, 1 = succeeded, 2 = failed */
void swm_new_command_event(zval *out, int kind, zend_string *name, zend_string *db, zval *doc, zend_string *address,
                           int64_t duration_us, int64_t operation_id, int64_t request_id, swm_error *err);
void swm_new_server(zval *out, swm_client *c, zend_string *address);
void swm_rp_to_array(zval *rp, zval *out); /* ReadPreference object -> internal array */
extern zend_class_entry *swm_ce_SDAMSubscriber;
void swm_new_server_description(zval *out, zend_string *host, zend_long port, const char *type, zval *hello, int64_t last_update, int64_t rtt_ms);
void swm_new_topology_description(zval *out, const char *type, zval *servers);
/* kind: 0 serverChanged 1 serverOpening 2 serverClosed 3 heartbeatStarted 4 heartbeatSucceeded 5 heartbeatFailed
 *       6 topologyChanged 7 topologyOpening 8 topologyClosed */
void swm_new_sdam_event(zval *out, int kind, zend_string *host, zend_long port, zval *topology_id, zval *new_desc, zval *prev_desc,
                        int64_t duration_us, zval *reply, swm_error *err);
void swm_driver_minit(void);

/* ----------------------------------------------------------------- helpers */
int64_t swm_now_us(void);
void swm_sleep_us(int64_t us);
zend_class_entry *swm_swoole_co_ce(void);
void swm_add_assoc_us_as_ms(zval *arr, const char *key, int64_t us);
zend_bool swm_in_coroutine(void);
int swm_call_function(const char *name, zval *retval, uint32_t argc, zval *argv);
/* FPU helpers (implemented in swm_util.c, the only file allowed to use FP registers) */
int64_t swm_dval_to_int64(zval *v);
uint64_t swm_strtod_bits(const char *s, int *ok);
void swm_append_double(smart_str *s, uint64_t bits, zend_bool relaxed);
void swm_zval_set_double_bits(zval *z, uint64_t bits);

/* arginfo shared by all compilation units */
#define SWM_ARGINFO_RET(name, type) \
    ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(name, 0, 0, type, 0) \
    ZEND_END_ARG_INFO()

#endif /* PHP_SWMONGO_H */
