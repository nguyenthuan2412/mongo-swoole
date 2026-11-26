/*
 * SwooleMongo\Driver: Manager, Server, Command, Query, BulkWrite, WriteResult,
 * WriteError, WriteConcernError, Cursor, Session, ReadPreference, ReadConcern,
 * WriteConcern and Monitoring (mirrors the ext-mongodb API).
 *
 * Compiled with -mgeneral-regs-only on arm64: no floating point here.
 */
#include "php_swmongo.h"

zend_class_entry *swm_ce_Manager;
zend_class_entry *swm_ce_Server;
zend_class_entry *swm_ce_Command;
zend_class_entry *swm_ce_Query;
zend_class_entry *swm_ce_BulkWrite;
zend_class_entry *swm_ce_WriteResult;
zend_class_entry *swm_ce_WriteError;
zend_class_entry *swm_ce_WriteConcernError;
zend_class_entry *swm_ce_Cursor;
zend_class_entry *swm_ce_CursorInterface;
zend_class_entry *swm_ce_Session;
zend_class_entry *swm_ce_ReadPreference;
zend_class_entry *swm_ce_ReadConcern;
zend_class_entry *swm_ce_WriteConcern;
zend_class_entry *swm_ce_ServerApi;
zend_class_entry *swm_ce_Subscriber;
zend_class_entry *swm_ce_CommandSubscriber;
zend_class_entry *swm_ce_CommandStartedEvent;
zend_class_entry *swm_ce_CommandSucceededEvent;
zend_class_entry *swm_ce_CommandFailedEvent;
zend_class_entry *swm_ce_SDAMSubscriber;
zend_class_entry *swm_ce_BulkWriteCommand;
zend_class_entry *swm_ce_BulkWriteCommandResult;
zend_class_entry *swm_ce_BulkWriteCommandException;
zend_class_entry *swm_ce_ServerDescription;
zend_class_entry *swm_ce_TopologyDescription;
static zend_class_entry *sdam_ce[9];

static zend_object_handlers manager_handlers, server_handlers, session_handlers, cursor_handlers;

#define PROP(o, n) OBJ_PROP_NUM((o), (n))
#define THIS_OBJ Z_OBJ_P(ZEND_THIS)

static zval *hfind(zval *arr, const char *key)
{
    if (!arr || Z_TYPE_P(arr) != IS_ARRAY) return NULL;
    return zend_hash_str_find_deref(Z_ARRVAL_P(arr), key, strlen(key));
}

static void prop_set(zend_object *o, int n, zval *v)
{
    zval *p = PROP(o, n);
    zval_ptr_dtor(p);
    ZVAL_COPY(p, v);
}
static void prop_set_long(zend_object *o, int n, zend_long l)
{
    zval *p = PROP(o, n);
    zval_ptr_dtor(p);
    ZVAL_LONG(p, l);
}
static void prop_set_str(zend_object *o, int n, zend_string *s)
{
    zval *p = PROP(o, n);
    zval_ptr_dtor(p);
    ZVAL_STR(p, s);
}

#define THROW_ERR(err) do { if (EG(exception)) swm_error_clear(&(err)); else swm_error_throw(&(err)); RETURN_THROWS(); } while (0)

/* ============================================================ value classes */

/* ---------------------------------------------------------- ReadPreference */
#define RP_PRIMARY 1
#define RP_SECONDARY 2
#define RP_PRIMARY_PREFERRED 5
#define RP_SECONDARY_PREFERRED 6
#define RP_NEAREST 10

static const struct { const char *name; int mode; } rp_modes[] = {
    {"primary", RP_PRIMARY}, {"primaryPreferred", RP_PRIMARY_PREFERRED}, {"secondary", RP_SECONDARY},
    {"secondaryPreferred", RP_SECONDARY_PREFERRED}, {"nearest", RP_NEAREST}, {NULL, 0},
};

static int rp_init(zend_object *o, zval *mode, zval *tags, zval *options, swm_error *err)
{
    const char *name = NULL;
    zval *ms, *hedge;
    if (Z_TYPE_P(mode) == IS_LONG) {
        for (int i = 0; rp_modes[i].name; i++) if (rp_modes[i].mode == Z_LVAL_P(mode)) name = rp_modes[i].name;
    } else if (Z_TYPE_P(mode) == IS_STRING) {
        for (int i = 0; rp_modes[i].name; i++) if (!strcasecmp(rp_modes[i].name, Z_STRVAL_P(mode))) name = rp_modes[i].name;
    }
    if (!name) {
        swm_error_set(err, SWM_ERR_ARG, 0, "Invalid mode: %s", Z_TYPE_P(mode) == IS_STRING ? Z_STRVAL_P(mode) : "(int)");
        return FAILURE;
    }
    prop_set_str(o, 0, zend_string_init(name, strlen(name), 0));
    if (tags && Z_TYPE_P(tags) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(tags))) {
        zval *t, list;
        if (!strcmp(name, "primary")) {
            swm_error_set(err, SWM_ERR_ARG, 0, "tagSets may not be used with primary mode");
            return FAILURE;
        }
        array_init(&list);
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(tags), t) {
            zval a;
            if (Z_TYPE_P(t) == IS_ARRAY) ZVAL_COPY(&a, t);
            else if (Z_TYPE_P(t) == IS_OBJECT) { HashTable *ht = zend_get_properties_for(t, ZEND_PROP_PURPOSE_ARRAY_CAST); ZVAL_ARR(&a, zend_array_dup(ht)); zend_release_properties(ht); }
            else { zval_ptr_dtor(&list); swm_error_set(err, SWM_ERR_ARG, 0, "tagSets must be an array of zero or more documents"); return FAILURE; }
            add_next_index_zval(&list, &a);
        } ZEND_HASH_FOREACH_END();
        prop_set(o, 1, &list);
        zval_ptr_dtor(&list);
    }
    prop_set_long(o, 2, -1);
    if (options && (ms = hfind(options, "maxStalenessSeconds")) && Z_TYPE_P(ms) != IS_NULL) {
        zend_long v = zval_get_long(ms);
        if (v != -1) {
            if (!strcmp(name, "primary")) { swm_error_set(err, SWM_ERR_ARG, 0, "maxStalenessSeconds may not be used with primary mode"); return FAILURE; }
            if (v < 90) { swm_error_set(err, SWM_ERR_ARG, 0, "Expected maxStalenessSeconds to be >= 90, " ZEND_LONG_FMT " given", v); return FAILURE; }
        }
        prop_set_long(o, 2, v);
    }
    if (options && (hedge = hfind(options, "hedge")) && Z_TYPE_P(hedge) != IS_NULL) prop_set(o, 3, hedge);
    return SUCCESS;
}

static void new_read_preference(zval *out, zval *internal)
{
    zval *mode = hfind(internal, "mode"), *tags = hfind(internal, "tags"), opts, *ms;
    swm_error err;
    swm_error_init(&err);
    object_init_ex(out, swm_ce_ReadPreference);
    array_init(&opts);
    if ((ms = hfind(internal, "maxStalenessSeconds"))) { Z_TRY_ADDREF_P(ms); add_assoc_zval(&opts, "maxStalenessSeconds", ms); }
    if (!mode) { zval m; ZVAL_STRING(&m, "primary"); rp_init(Z_OBJ_P(out), &m, NULL, NULL, &err); zval_ptr_dtor(&m); }
    else rp_init(Z_OBJ_P(out), mode, tags, &opts, &err);
    zval_ptr_dtor(&opts);
    swm_error_clear(&err);
}

void swm_rp_to_array(zval *rp, zval *out)
{
    zend_object *o = Z_OBJ_P(rp);
    array_init(out);
    Z_TRY_ADDREF_P(PROP(o, 0));
    add_assoc_zval(out, "mode", PROP(o, 0));
    if (Z_TYPE_P(PROP(o, 1)) == IS_ARRAY) { Z_ADDREF_P(PROP(o, 1)); add_assoc_zval(out, "tags", PROP(o, 1)); }
    if (Z_LVAL_P(PROP(o, 2)) > 0) add_assoc_long(out, "maxStalenessSeconds", Z_LVAL_P(PROP(o, 2)));
}

PHP_METHOD(SwooleMongo_Driver_ReadPreference, __construct)
{
    zval *mode, *tags = NULL, *options = NULL;
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_ZVAL(mode)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(tags)
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    swm_error_init(&err);
    if (rp_init(THIS_OBJ, mode, tags, options, &err) == FAILURE) THROW_ERR(err);
}
PHP_METHOD(SwooleMongo_Driver_ReadPreference, getMode)
{
    ZEND_PARSE_PARAMETERS_NONE();
    for (int i = 0; rp_modes[i].name; i++) if (!strcmp(rp_modes[i].name, Z_STRVAL_P(PROP(THIS_OBJ, 0)))) RETURN_LONG(rp_modes[i].mode);
    RETURN_LONG(RP_PRIMARY);
}
PHP_METHOD(SwooleMongo_Driver_ReadPreference, getModeString)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(THIS_OBJ, 0));
}
PHP_METHOD(SwooleMongo_Driver_ReadPreference, getTagSets)
{
    ZEND_PARSE_PARAMETERS_NONE();
    if (Z_TYPE_P(PROP(THIS_OBJ, 1)) == IS_ARRAY) RETURN_COPY(PROP(THIS_OBJ, 1));
    RETURN_EMPTY_ARRAY();
}
PHP_METHOD(SwooleMongo_Driver_ReadPreference, getMaxStalenessSeconds)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(THIS_OBJ, 2));
}
PHP_METHOD(SwooleMongo_Driver_ReadPreference, getHedge)
{
    ZEND_PARSE_PARAMETERS_NONE();
    if (Z_TYPE_P(PROP(THIS_OBJ, 3)) == IS_NULL) RETURN_NULL();
    swm_array_to_object_doc(PROP(THIS_OBJ, 3), return_value);
}
PHP_METHOD(SwooleMongo_Driver_ReadPreference, bsonSerialize)
{
    zval arr;
    ZEND_PARSE_PARAMETERS_NONE();
    swm_rp_to_array(ZEND_THIS, &arr);
    if (Z_TYPE_P(PROP(THIS_OBJ, 3)) != IS_NULL) { Z_TRY_ADDREF_P(PROP(THIS_OBJ, 3)); add_assoc_zval(&arr, "hedge", PROP(THIS_OBJ, 3)); }
    object_and_properties_init(return_value, zend_standard_class_def, Z_ARRVAL(arr));
}

/* ------------------------------------------------------------- ReadConcern */
static void rc_to_array(zval *rc, zval *out) /* UNDEF if default */
{
    zval *lvl = PROP(Z_OBJ_P(rc), 0);
    if (Z_TYPE_P(lvl) != IS_STRING) { ZVAL_UNDEF(out); return; }
    array_init(out);
    add_assoc_str(out, "level", zend_string_copy(Z_STR_P(lvl)));
}

static void new_read_concern(zval *out, zval *internal)
{
    zval *lvl = hfind(internal, "level");
    object_init_ex(out, swm_ce_ReadConcern);
    if (lvl && Z_TYPE_P(lvl) == IS_STRING) prop_set(Z_OBJ_P(out), 0, lvl);
}

PHP_METHOD(SwooleMongo_Driver_ReadConcern, __construct)
{
    zend_string *level = NULL;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR_OR_NULL(level)
    ZEND_PARSE_PARAMETERS_END();
    if (level) prop_set_str(THIS_OBJ, 0, zend_string_copy(level));
}
PHP_METHOD(SwooleMongo_Driver_ReadConcern, getLevel)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(THIS_OBJ, 0));
}
PHP_METHOD(SwooleMongo_Driver_ReadConcern, isDefault)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(Z_TYPE_P(PROP(THIS_OBJ, 0)) != IS_STRING);
}
PHP_METHOD(SwooleMongo_Driver_ReadConcern, bsonSerialize)
{
    zval arr;
    ZEND_PARSE_PARAMETERS_NONE();
    rc_to_array(ZEND_THIS, &arr);
    if (Z_TYPE(arr) == IS_UNDEF) array_init(&arr);
    object_and_properties_init(return_value, zend_standard_class_def, Z_ARRVAL(arr));
}

/* ------------------------------------------------------------ WriteConcern */
/* props: 0 w, 1 wtimeout, 2 journal, 3 isDefault */
static void wc_to_array(zval *wc, zval *out) /* UNDEF if nothing to send */
{
    zend_object *o = Z_OBJ_P(wc);
    array_init(out);
    if (Z_TYPE_P(PROP(o, 0)) != IS_NULL) { Z_TRY_ADDREF_P(PROP(o, 0)); add_assoc_zval(out, "w", PROP(o, 0)); }
    if (Z_TYPE_P(PROP(o, 2)) == IS_TRUE || Z_TYPE_P(PROP(o, 2)) == IS_FALSE) add_assoc_bool(out, "j", Z_TYPE_P(PROP(o, 2)) == IS_TRUE);
    if (Z_TYPE_P(PROP(o, 1)) == IS_LONG && Z_LVAL_P(PROP(o, 1)) > 0) add_assoc_long(out, "wtimeout", Z_LVAL_P(PROP(o, 1)));
    if (zend_hash_num_elements(Z_ARRVAL_P(out)) == 0) { zval_ptr_dtor(out); ZVAL_UNDEF(out); }
}

static void new_write_concern(zval *out, zval *internal)
{
    zval *v;
    object_init_ex(out, swm_ce_WriteConcern);
    prop_set_long(Z_OBJ_P(out), 1, 0);
    if (!internal || Z_TYPE_P(internal) != IS_ARRAY) {
        zval t;
        ZVAL_TRUE(&t);
        prop_set(Z_OBJ_P(out), 3, &t);
        return;
    }
    if ((v = hfind(internal, "w"))) prop_set(Z_OBJ_P(out), 0, v);
    if ((v = hfind(internal, "wtimeout"))) prop_set_long(Z_OBJ_P(out), 1, zval_get_long(v));
    if ((v = hfind(internal, "j"))) { zval b; ZVAL_BOOL(&b, zend_is_true(v)); prop_set(Z_OBJ_P(out), 2, &b); }
    { zval f; ZVAL_FALSE(&f); prop_set(Z_OBJ_P(out), 3, &f); }
}

PHP_METHOD(SwooleMongo_Driver_WriteConcern, __construct)
{
    zval *w, f;
    zend_long wtimeout = 0;
    zend_bool wtimeout_null = 1, journal = 0, journal_null = 1;
    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_ZVAL(w)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG_OR_NULL(wtimeout, wtimeout_null)
        Z_PARAM_BOOL_OR_NULL(journal, journal_null)
    ZEND_PARSE_PARAMETERS_END();
    if (Z_TYPE_P(w) == IS_LONG) {
        if (Z_LVAL_P(w) < -3) { swm_throw(swm_ce_InvalidArgumentException, "Expected w to be >= -3, " ZEND_LONG_FMT " given", Z_LVAL_P(w)); RETURN_THROWS(); }
    } else if (Z_TYPE_P(w) != IS_STRING) {
        swm_throw(swm_ce_InvalidArgumentException, "Expected w to be integer or string, %s given", zend_zval_type_name(w));
        RETURN_THROWS();
    }
    if (!wtimeout_null && wtimeout < 0) { swm_throw(swm_ce_InvalidArgumentException, "Expected wtimeout to be >= 0"); RETURN_THROWS(); }
    prop_set(THIS_OBJ, 0, w);
    prop_set_long(THIS_OBJ, 1, wtimeout_null ? 0 : wtimeout);
    if (!journal_null) { zval b; ZVAL_BOOL(&b, journal); prop_set(THIS_OBJ, 2, &b); }
    ZVAL_FALSE(&f);
    prop_set(THIS_OBJ, 3, &f);
}
PHP_METHOD(SwooleMongo_Driver_WriteConcern, getW)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(THIS_OBJ, 0));
}
PHP_METHOD(SwooleMongo_Driver_WriteConcern, getWtimeout)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(THIS_OBJ, 1));
}
PHP_METHOD(SwooleMongo_Driver_WriteConcern, getJournal)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_COPY(PROP(THIS_OBJ, 2));
}
PHP_METHOD(SwooleMongo_Driver_WriteConcern, isDefault)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(Z_TYPE_P(PROP(THIS_OBJ, 3)) == IS_TRUE);
}
PHP_METHOD(SwooleMongo_Driver_WriteConcern, bsonSerialize)
{
    zval arr;
    ZEND_PARSE_PARAMETERS_NONE();
    wc_to_array(ZEND_THIS, &arr);
    if (Z_TYPE(arr) == IS_UNDEF) array_init(&arr);
    object_and_properties_init(return_value, zend_standard_class_def, Z_ARRVAL(arr));
}

/* --------------------------------------------------------------- ServerApi */
PHP_METHOD(SwooleMongo_Driver_ServerApi, __construct)
{
    zend_string *version;
    zend_bool strict = 0, strict_null = 1, dep = 0, dep_null = 1;
    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_STR(version)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL_OR_NULL(strict, strict_null)
        Z_PARAM_BOOL_OR_NULL(dep, dep_null)
    ZEND_PARSE_PARAMETERS_END();
    if (!zend_string_equals_literal(version, "1")) {
        swm_throw(swm_ce_InvalidArgumentException, "Server API version \"%s\" is not supported", ZSTR_VAL(version));
        RETURN_THROWS();
    }
    prop_set_str(THIS_OBJ, 0, zend_string_copy(version));
    if (!strict_null) { zval b; ZVAL_BOOL(&b, strict); prop_set(THIS_OBJ, 1, &b); }
    if (!dep_null) { zval b; ZVAL_BOOL(&b, dep); prop_set(THIS_OBJ, 2, &b); }
}
PHP_METHOD(SwooleMongo_Driver_ServerApi, bsonSerialize)
{
    zval arr;
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(&arr);
    Z_TRY_ADDREF_P(PROP(THIS_OBJ, 0));
    add_assoc_zval(&arr, "version", PROP(THIS_OBJ, 0));
    if (Z_TYPE_P(PROP(THIS_OBJ, 1)) != IS_NULL) { Z_TRY_ADDREF_P(PROP(THIS_OBJ, 1)); add_assoc_zval(&arr, "strict", PROP(THIS_OBJ, 1)); }
    if (Z_TYPE_P(PROP(THIS_OBJ, 2)) != IS_NULL) { Z_TRY_ADDREF_P(PROP(THIS_OBJ, 2)); add_assoc_zval(&arr, "deprecationErrors", PROP(THIS_OBJ, 2)); }
    object_and_properties_init(return_value, zend_standard_class_def, Z_ARRVAL(arr));
}

/* ----------------------------------------------------------------- Command */
PHP_METHOD(SwooleMongo_Driver_Command, __construct)
{
    zval *doc, *options = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ZVAL(doc)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    if (Z_TYPE_P(doc) != IS_ARRAY && Z_TYPE_P(doc) != IS_OBJECT) {
        swm_throw(swm_ce_InvalidArgumentException, "Expected document to be array or object, %s given", zend_zval_type_name(doc));
        RETURN_THROWS();
    }
    prop_set(THIS_OBJ, 0, doc);
    if (options) prop_set(THIS_OBJ, 1, options);
}

/* ------------------------------------------------------------------- Query */
PHP_METHOD(SwooleMongo_Driver_Query, __construct)
{
    zval *filter, *options = NULL, *rc;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ZVAL(filter)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    if (Z_TYPE_P(filter) != IS_ARRAY && Z_TYPE_P(filter) != IS_OBJECT) {
        swm_throw(swm_ce_InvalidArgumentException, "Expected filter to be array or object, %s given", zend_zval_type_name(filter));
        RETURN_THROWS();
    }
    if (options && (rc = hfind(options, "readConcern")) && Z_TYPE_P(rc) != IS_NULL
        && !(Z_TYPE_P(rc) == IS_OBJECT && Z_OBJCE_P(rc) == swm_ce_ReadConcern)) {
        swm_throw(swm_ce_InvalidArgumentException, "Expected \"readConcern\" option to be SwooleMongo\\Driver\\ReadConcern, %s given", zend_zval_type_name(rc));
        RETURN_THROWS();
    }
    prop_set(THIS_OBJ, 0, filter);
    if (options) prop_set(THIS_OBJ, 1, options);
}

/* ------------------------------------------------- WriteError / WCError */
#define GETTER(cls, name, idx) \
PHP_METHOD(SwooleMongo_Driver_##cls, name) { ZEND_PARSE_PARAMETERS_NONE(); RETURN_COPY(PROP(THIS_OBJ, idx)); }

GETTER(WriteError, getCode, 0)
GETTER(WriteError, getIndex, 1)
GETTER(WriteError, getMessage, 2)
GETTER(WriteError, getInfo, 3)
GETTER(WriteConcernError, getCode, 0)
GETTER(WriteConcernError, getMessage, 1)
GETTER(WriteConcernError, getInfo, 2)

static void new_write_error(zval *out, zval *e, zend_long index)
{
    zval *v, info;
    object_init_ex(out, swm_ce_WriteError);
    prop_set_long(Z_OBJ_P(out), 0, zval_get_long((v = hfind(e, "code")) ? v : &EG(uninitialized_zval)));
    prop_set_long(Z_OBJ_P(out), 1, index);
    v = hfind(e, "errmsg");
    prop_set_str(Z_OBJ_P(out), 2, v && Z_TYPE_P(v) == IS_STRING ? zend_string_copy(Z_STR_P(v)) : ZSTR_EMPTY_ALLOC());
    if ((v = hfind(e, "errInfo")) && Z_TYPE_P(v) == IS_ARRAY) {
        swm_array_to_object_doc(v, &info);
        prop_set(Z_OBJ_P(out), 3, &info);
        zval_ptr_dtor(&info);
    }
}

static void new_wc_error(zval *out, zval *e)
{
    zval *v, info;
    object_init_ex(out, swm_ce_WriteConcernError);
    prop_set_long(Z_OBJ_P(out), 0, zval_get_long((v = hfind(e, "code")) ? v : &EG(uninitialized_zval)));
    v = hfind(e, "errmsg");
    prop_set_str(Z_OBJ_P(out), 1, v && Z_TYPE_P(v) == IS_STRING ? zend_string_copy(Z_STR_P(v)) : ZSTR_EMPTY_ALLOC());
    if ((v = hfind(e, "errInfo")) && Z_TYPE_P(v) == IS_ARRAY) {
        swm_array_to_object_doc(v, &info);
        prop_set(Z_OBJ_P(out), 2, &info);
        zval_ptr_dtor(&info);
    }
}

/* ------------------------------------------------------------- WriteResult */
/* props: 0 nInserted 1 nMatched 2 nModified 3 nRemoved 4 nUpserted 5 upsertedIds 6 writeErrors 7 wcError 8 acknowledged 9 server */
#define WR_COUNT(name, idx) \
PHP_METHOD(SwooleMongo_Driver_WriteResult, name) { \
    ZEND_PARSE_PARAMETERS_NONE(); \
    if (Z_TYPE_P(PROP(THIS_OBJ, 8)) != IS_TRUE) RETURN_NULL(); \
    RETURN_COPY(PROP(THIS_OBJ, idx)); \
}
WR_COUNT(getInsertedCount, 0)
WR_COUNT(getMatchedCount, 1)
WR_COUNT(getModifiedCount, 2)
WR_COUNT(getDeletedCount, 3)
WR_COUNT(getUpsertedCount, 4)
GETTER(WriteResult, getUpsertedIds, 5)
GETTER(WriteResult, getWriteErrors, 6)
GETTER(WriteResult, getWriteConcernError, 7)
GETTER(WriteResult, isAcknowledged, 8)
GETTER(WriteResult, getServer, 9)

/* --------------------------------------------------------- Monitoring events */
/* props: 0 commandName 1 databaseName 2 operationId 3 requestId 4 host 5 port 6 durationMicros 7 command/reply 8 error 9 reply(failed) */
GETTER(Monitoring_CommandStartedEvent, getCommandName, 0)
GETTER(Monitoring_CommandStartedEvent, getDatabaseName, 1)
GETTER(Monitoring_CommandStartedEvent, getOperationId, 2)
GETTER(Monitoring_CommandStartedEvent, getRequestId, 3)
GETTER(Monitoring_CommandStartedEvent, getHost, 4)
GETTER(Monitoring_CommandStartedEvent, getPort, 5)
GETTER(Monitoring_CommandStartedEvent, getCommand, 7)
GETTER(Monitoring_CommandSucceededEvent, getDurationMicros, 6)
GETTER(Monitoring_CommandSucceededEvent, getReply, 7)
GETTER(Monitoring_CommandFailedEvent, getError, 8)
PHP_METHOD(SwooleMongo_Driver_Monitoring_CommandStartedEvent, getServiceId) { ZEND_PARSE_PARAMETERS_NONE(); RETURN_NULL(); }

void swm_new_command_event(zval *out, int kind, zend_string *name, zend_string *db, zval *doc, zend_string *address,
                           int64_t duration_us, int64_t operation_id, int64_t request_id, swm_error *err)
{
    zend_class_entry *ce = kind == 0 ? swm_ce_CommandStartedEvent : kind == 1 ? swm_ce_CommandSucceededEvent : swm_ce_CommandFailedEvent;
    zend_object *o;
    const char *colon = zend_memrchr(ZSTR_VAL(address), ':', ZSTR_LEN(address));
    object_init_ex(out, ce);
    o = Z_OBJ_P(out);
    prop_set_str(o, 0, zend_string_copy(name));
    prop_set_str(o, 1, zend_string_copy(db));
    prop_set_str(o, 2, zend_strpprintf(0, "%lld", (long long) operation_id));
    prop_set_str(o, 3, zend_strpprintf(0, "%lld", (long long) request_id));
    if (colon) {
        prop_set_str(o, 4, zend_string_init(ZSTR_VAL(address), colon - ZSTR_VAL(address), 0));
        prop_set_long(o, 5, ZEND_STRTOL(colon + 1, NULL, 10));
    } else {
        prop_set_str(o, 4, zend_string_copy(address));
        prop_set_long(o, 5, 0);
    }
    prop_set_long(o, 6, (zend_long) duration_us);
    if (doc) {
        zval d;
        swm_array_to_object_doc(doc, &d);
        prop_set(o, 7, &d);
        zval_ptr_dtor(&d);
    } else {
        zval d;
        object_init(&d);
        prop_set(o, 7, &d);
        zval_ptr_dtor(&d);
    }
    if (kind == 2 && err) {
        zval ex;
        swm_error_to_exception(err, &ex);
        prop_set(o, 8, &ex);
        zval_ptr_dtor(&ex);
    }
}

/* ------------------------------------------------------------- SDAM */
/* ServerDescription props: 0 host 1 port 2 type 3 helloResponse 4 lastUpdateTime 5 roundTripTime */
GETTER(ServerDescription, getHost, 0)
GETTER(ServerDescription, getPort, 1)
GETTER(ServerDescription, getType, 2)
GETTER(ServerDescription, getHelloResponse, 3)
GETTER(ServerDescription, getLastUpdateTime, 4)
GETTER(ServerDescription, getRoundTripTime, 5)
/* TopologyDescription props: 0 type 1 servers */
GETTER(TopologyDescription, getType, 0)
GETTER(TopologyDescription, getServers, 1)

static zend_bool td_has(zval *self, zend_bool writable, zval *rp)
{
    zval *servers = PROP(Z_OBJ_P(self), 1), *sd;
    const char *mode = rp ? Z_STRVAL_P(PROP(Z_OBJ_P(rp), 0)) : "primary";
    if (Z_TYPE_P(servers) != IS_ARRAY) return 0;
    if (!writable && !strcmp(mode, "primary")) writable = 1;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(servers), sd) {
        const char *t = Z_STRVAL_P(PROP(Z_OBJ_P(sd), 2));
        if (!strcmp(t, "RSPrimary") || !strcmp(t, "Standalone") || !strcmp(t, "Mongos") || !strcmp(t, "LoadBalancer")) return 1;
        if (!writable && !strcmp(t, "RSSecondary")) return 1;
    } ZEND_HASH_FOREACH_END();
    return 0;
}
PHP_METHOD(SwooleMongo_Driver_TopologyDescription, hasReadableServer)
{
    zval *rp = NULL;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_OBJECT_OF_CLASS_OR_NULL(rp, swm_ce_ReadPreference)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_BOOL(td_has(ZEND_THIS, 0, rp));
}
PHP_METHOD(SwooleMongo_Driver_TopologyDescription, hasWritableServer)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(td_has(ZEND_THIS, 1, NULL));
}

void swm_new_server_description(zval *out, zend_string *host, zend_long port, const char *type, zval *hello, int64_t last_update, int64_t rtt_ms)
{
    zval t;
    object_init_ex(out, swm_ce_ServerDescription);
    prop_set_str(Z_OBJ_P(out), 0, zend_string_copy(host));
    prop_set_long(Z_OBJ_P(out), 1, port);
    prop_set_str(Z_OBJ_P(out), 2, zend_string_init(type, strlen(type), 0));
    if (hello && Z_TYPE_P(hello) == IS_ARRAY) prop_set(Z_OBJ_P(out), 3, hello);
    else { array_init(&t); prop_set(Z_OBJ_P(out), 3, &t); zval_ptr_dtor(&t); }
    prop_set_long(Z_OBJ_P(out), 4, (zend_long) last_update);
    if (rtt_ms >= 0) prop_set_long(Z_OBJ_P(out), 5, (zend_long) rtt_ms);
}

void swm_new_topology_description(zval *out, const char *type, zval *servers)
{
    object_init_ex(out, swm_ce_TopologyDescription);
    prop_set_str(Z_OBJ_P(out), 0, zend_string_init(type, strlen(type), 0));
    prop_set(Z_OBJ_P(out), 1, servers);
}

/* SDAM event props: 0 host 1 port 2 topologyId 3 newDescription 4 previousDescription 5 durationMicros 6 reply 7 error 8 awaited */
GETTER(Monitoring_ServerChangedEvent, getHost, 0)
GETTER(Monitoring_ServerChangedEvent, getPort, 1)
GETTER(Monitoring_ServerChangedEvent, getTopologyId, 2)
GETTER(Monitoring_ServerChangedEvent, getNewDescription, 3)
GETTER(Monitoring_ServerChangedEvent, getPreviousDescription, 4)
GETTER(Monitoring_ServerHeartbeatSucceededEvent, getDurationMicros, 5)
GETTER(Monitoring_ServerHeartbeatSucceededEvent, getReply, 6)
GETTER(Monitoring_ServerHeartbeatFailedEvent, getError, 7)
GETTER(Monitoring_ServerHeartbeatStartedEvent, isAwaited, 8)

void swm_new_sdam_event(zval *out, int kind, zend_string *host, zend_long port, zval *topology_id, zval *new_desc, zval *prev_desc,
                        int64_t duration_us, zval *reply, swm_error *err)
{
    zend_object *o;
    zval f;
    object_init_ex(out, sdam_ce[kind]);
    o = Z_OBJ_P(out);
    if (host) prop_set_str(o, 0, zend_string_copy(host));
    prop_set_long(o, 1, port);
    if (topology_id) prop_set(o, 2, topology_id);
    if (new_desc) prop_set(o, 3, new_desc);
    if (prev_desc) prop_set(o, 4, prev_desc);
    prop_set_long(o, 5, (zend_long) duration_us);
    if (reply) { zval d; swm_array_to_object_doc(reply, &d); prop_set(o, 6, &d); zval_ptr_dtor(&d); }
    if (err) { zval ex; swm_error_to_exception(err, &ex); prop_set(o, 7, &ex); zval_ptr_dtor(&ex); }
    ZVAL_FALSE(&f);
    prop_set(o, 8, &f);
}

/* ============================================================== Manager */
static zend_object *manager_create(zend_class_entry *ce)
{
    swm_manager_obj *m = zend_object_alloc(sizeof(swm_manager_obj), ce);
    zend_object_std_init(&m->std, ce);
    object_properties_init(&m->std, ce);
    m->std.handlers = &manager_handlers;
    return &m->std;
}

static void manager_free(zend_object *o)
{
    swm_manager_obj *m = swm_manager_from(o);
    if (m->client) swm_client_release(m->client);
    zend_object_std_dtor(o);
}

static zend_object *server_create(zend_class_entry *ce)
{
    swm_server_obj *s = zend_object_alloc(sizeof(swm_server_obj), ce);
    zend_object_std_init(&s->std, ce);
    object_properties_init(&s->std, ce);
    s->std.handlers = &server_handlers;
    return &s->std;
}

static void server_free(zend_object *o)
{
    swm_server_obj *s = swm_server_from(o);
    if (s->client) swm_client_release(s->client);
    if (s->address) zend_string_release(s->address);
    zend_object_std_dtor(o);
}

void swm_new_server(zval *out, swm_client *c, zend_string *address)
{
    swm_server_obj *s;
    object_init_ex(out, swm_ce_Server);
    s = swm_server_from(Z_OBJ_P(out));
    s->client = c;
    swm_client_addref(c);
    s->address = zend_string_copy(address);
}

/* ---- shared option parsing */
static int opt_object(zval *options, const char *key, zend_class_entry *ce, zval **out, swm_error *err)
{
    zval *v = hfind(options, key);
    *out = NULL;
    if (!v || Z_TYPE_P(v) == IS_NULL) return SUCCESS;
    if (Z_TYPE_P(v) != IS_OBJECT || !instanceof_function(Z_OBJCE_P(v), ce)) {
        swm_error_set(err, SWM_ERR_ARG, 0, "Expected \"%s\" option to be %s, %s given", key, ZSTR_VAL(ce->name), zend_zval_type_name(v));
        return FAILURE;
    }
    *out = v;
    return SUCCESS;
}

static int opt_timeout(zval *options, int64_t *out, swm_error *err)
{
    zval *v = hfind(options, "timeoutMS");
    *out = 0;
    if (!v || Z_TYPE_P(v) == IS_NULL) return SUCCESS;
    if (Z_TYPE_P(v) != IS_LONG || Z_LVAL_P(v) < 0) {
        swm_error_set(err, SWM_ERR_ARG, 0, "Expected \"timeoutMS\" option to be a non-negative integer");
        return FAILURE;
    }
    *out = (int64_t) Z_LVAL_P(v) * 1000;
    return SUCCESS;
}

/* Copies an array/object/Document command document into a fresh internal array */
static int doc_to_array(zval *doc, zval *out, swm_error *err)
{
    smart_str s = {0};
    if (Z_TYPE_P(doc) == IS_ARRAY) {
        ZVAL_ARR(out, zend_array_dup(Z_ARRVAL_P(doc)));
        return SUCCESS;
    }
    if (swm_bson_encode(&s, doc, err) == FAILURE) { smart_str_free(&s); return FAILURE; }
    if (swm_bson_decode(ZSTR_VAL(s.s), ZSTR_LEN(s.s), out, err) == FAILURE) { smart_str_free(&s); return FAILURE; }
    smart_str_free(&s);
    return SUCCESS;
}

/* Encodes a document-ish value as a raw Document (so [] becomes {}), lists as PackedArray when allow_list */
static int as_document(zval *v, zval *out, zend_bool allow_list, swm_error *err)
{
    smart_str s = {0};
    ZVAL_DEREF(v);
    if (Z_TYPE_P(v) == IS_OBJECT && (Z_OBJCE_P(v) == swm_ce_Document || Z_OBJCE_P(v) == swm_ce_PackedArray)) {
        ZVAL_COPY(out, v);
        return SUCCESS;
    }
    if (Z_TYPE_P(v) != IS_ARRAY && Z_TYPE_P(v) != IS_OBJECT) {
        ZVAL_COPY(out, v);
        return SUCCESS;
    }
    if (Z_TYPE_P(v) == IS_OBJECT && instanceof_function(Z_OBJCE_P(v), swm_ce_Type) && !instanceof_function(Z_OBJCE_P(v), swm_ce_Serializable)) {
        ZVAL_COPY(out, v);
        return SUCCESS;
    }
    if (swm_bson_encode(&s, v, err) == FAILURE) { smart_str_free(&s); return FAILURE; }
    swm_new_document_raw(out, (allow_list && Z_TYPE_P(v) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(v)) && zend_array_is_list(Z_ARRVAL_P(v)))
                              ? swm_ce_PackedArray : swm_ce_Document, ZSTR_VAL(s.s), ZSTR_LEN(s.s));
    smart_str_free(&s);
    return SUCCESS;
}

static int split_ns(zend_string *ns, zend_string **db, zend_string **coll, swm_error *err)
{
    const char *dot = memchr(ZSTR_VAL(ns), '.', ZSTR_LEN(ns));
    if (!dot || dot == ZSTR_VAL(ns) || dot == ZSTR_VAL(ns) + ZSTR_LEN(ns) - 1) {
        swm_error_set(err, SWM_ERR_ARG, 0, "Invalid namespace provided: %s", ZSTR_VAL(ns));
        return FAILURE;
    }
    *db = zend_string_init(ZSTR_VAL(ns), dot - ZSTR_VAL(ns), 0);
    *coll = zend_string_init(dot + 1, ZSTR_VAL(ns) + ZSTR_LEN(ns) - dot - 1, 0);
    return SUCCESS;
}

/* ============================================================== Cursor */
static zend_object *cursor_create(zend_class_entry *ce)
{
    swm_cursor_obj *c = zend_object_alloc(sizeof(swm_cursor_obj), ce);
    zend_object_std_init(&c->std, ce);
    object_properties_init(&c->std, ce);
    c->std.handlers = &cursor_handlers;
    ZVAL_UNDEF(&c->batch);
    ZVAL_UNDEF(&c->current);
    ZVAL_UNDEF(&c->session);
    ZVAL_UNDEF(&c->comment);
    ZVAL_UNDEF(&c->pbrt);
    swm_typemap_init(&c->tm);
    return &c->std;
}

static void cursor_free(zend_object *o)
{
    swm_cursor_obj *c = swm_cursor_from(o);
    if (c->client) {
        if (c->pinned_conn) {
            /* load balanced: closing the pinned connection makes the server drop the cursor */
            swm_conn_unpin(c->client, c->pinned_conn, c->id != 0);
            c->pinned_conn = NULL;
        } else if (c->id != 0 && c->server) {
            swm_client_queue_kill(c->client, c->server, c->db, c->coll, c->id);
        }
        swm_client_release(c->client);
    }
    if (c->server) zend_string_release(c->server);
    if (c->db) zend_string_release(c->db);
    if (c->coll) zend_string_release(c->coll);
    zval_ptr_dtor(&c->batch);
    zval_ptr_dtor(&c->current);
    zval_ptr_dtor(&c->session);
    zval_ptr_dtor(&c->comment);
    zval_ptr_dtor(&c->pbrt);
    swm_typemap_dtor(&c->tm);
    zend_object_std_dtor(o);
}

/* Parses {cursor: {id, ns, firstBatch|nextBatch, postBatchResumeToken}} from the raw reply */
static void cursor_take_batch(swm_cursor_obj *c, zend_string *raw, zval *reply, const char *batch_key)
{
    const char *cur, *b;
    size_t clen, blen;
    zval *cz = hfind(reply, "cursor"), *v;
    zval_ptr_dtor(&c->batch);
    if (swm_bson_find(ZSTR_VAL(raw), ZSTR_LEN(raw), "cursor", &cur, &clen) == 0x03
        && swm_bson_find(cur, clen, batch_key, &b, &blen) == 0x04) {
        swm_bson_split(b, blen, &c->batch);
    } else {
        array_init(&c->batch);
    }
    c->pos = 0;
    if ((v = hfind(cz, "id"))) {
        zend_bool ok;
        c->id = swm_zval_to_int64(v, &ok);
    } else {
        c->id = 0;
    }
    if ((v = hfind(cz, "postBatchResumeToken"))) {
        zval_ptr_dtor(&c->pbrt);
        ZVAL_COPY(&c->pbrt, v);
    }
}

static void cursor_setup(zval *out, swm_client *client, zend_string *server, zend_string *raw, zval *reply,
                         zend_long batch_size, zend_long max_await, zend_bool tailable, zval *comment, zval *session,
                         zend_string *fallback_db)
{
    swm_cursor_obj *c;
    zval *cz = hfind(reply, "cursor"), *ns;
    object_init_ex(out, swm_ce_Cursor);
    c = swm_cursor_from(Z_OBJ_P(out));
    c->client = client;
    swm_client_addref(client);
    c->server = server; /* takes ownership */
    c->batch_size = batch_size;
    c->max_await = max_await;
    c->tailable = tailable;
    if (comment) ZVAL_COPY(&c->comment, comment);
    if (session) ZVAL_COPY(&c->session, session);
    if (cz && Z_TYPE_P(cz) == IS_ARRAY && hfind(cz, "id")) {
        ns = hfind(cz, "ns");
        if (ns && Z_TYPE_P(ns) == IS_STRING && memchr(Z_STRVAL_P(ns), '.', Z_STRLEN_P(ns))) {
            const char *dot = memchr(Z_STRVAL_P(ns), '.', Z_STRLEN_P(ns));
            c->db = zend_string_init(Z_STRVAL_P(ns), dot - Z_STRVAL_P(ns), 0);
            c->coll = zend_string_init(dot + 1, Z_STRLEN_P(ns) - (dot - Z_STRVAL_P(ns)) - 1, 0);
        } else {
            c->db = zend_string_copy(fallback_db);
            c->coll = ZSTR_EMPTY_ALLOC();
        }
        cursor_take_batch(c, raw, reply, "firstBatch");
    } else {
        /* plain command reply: a single document cursor */
        c->db = zend_string_copy(fallback_db);
        c->coll = ZSTR_EMPTY_ALLOC();
        array_init(&c->batch);
        add_next_index_str(&c->batch, zend_string_copy(raw));
        c->id = 0;
    }
}

static int cursor_getmore(swm_cursor_obj *c, swm_error *err)
{
    zval cmd, idz, reply;
    zend_string *raw = NULL;
    swm_exec_opts o;
    int rc;
    memset(&o, 0, sizeof(o));
    array_init(&cmd);
    swm_new_int64(&idz, c->id);
    add_assoc_zval(&cmd, "getMore", &idz);
    add_assoc_str(&cmd, "collection", zend_string_copy(c->coll));
    if (c->batch_size > 0) add_assoc_long(&cmd, "batchSize", c->batch_size);
    if (c->tailable && c->max_await > 0) add_assoc_long(&cmd, "maxTimeMS", c->max_await);
    if (Z_TYPE(c->comment) != IS_UNDEF) { Z_TRY_ADDREF(c->comment); add_assoc_zval(&cmd, "comment", &c->comment); }
    o.server = c->server;
    o.timeout_us = c->timeout_us;
    o.use_conn = c->pinned_conn;
    if (Z_TYPE(c->session) == IS_OBJECT) o.session = &c->session;
    rc = swm_execute(c->client, c->db, &cmd, &o, &reply, &raw, NULL, err);
    if (o.pinned_dead) c->pinned_conn = NULL;
    zval_ptr_dtor(&cmd);
    if (rc == FAILURE) {
        if (err->kind != SWM_ERR_SERVER || err->code == 43 || err->code == 175 || err->code == 237) c->id = 0;
        return FAILURE;
    }
    cursor_take_batch(c, raw, &reply, "nextBatch");
    zend_string_release(raw);
    zval_ptr_dtor(&reply);
    if (c->id == 0 && c->pinned_conn) {
        swm_conn_unpin(c->client, c->pinned_conn, 0);
        c->pinned_conn = NULL;
    }
    return SUCCESS;
}

static int cursor_load(swm_cursor_obj *c, swm_error *err)
{
    zval_ptr_dtor(&c->current);
    ZVAL_UNDEF(&c->current);
    for (;;) {
        zval *d = Z_TYPE(c->batch) == IS_ARRAY ? zend_hash_index_find(Z_ARRVAL(c->batch), c->pos) : NULL;
        if (d) {
            return swm_bson_decode_tm(Z_STRVAL_P(d), Z_STRLEN_P(d), c->has_tm ? &c->tm : NULL, &c->current, err);
        }
        if (c->id == 0) return SUCCESS;
        if (cursor_getmore(c, err) == FAILURE) return FAILURE;
        if (zend_hash_num_elements(Z_ARRVAL(c->batch)) == 0 && c->tailable) return SUCCESS;
    }
}

#define CURSOR_CALL(expr) do { swm_error err; swm_error_init(&err); if ((expr) == FAILURE) THROW_ERR(err); } while (0)
#define THIS_CURSOR() swm_cursor_from(THIS_OBJ)
#define ENSURE_CURSOR(c) do { if (!(c)->client) { swm_throw(swm_ce_LogicException, "Cursor is not initialized"); RETURN_THROWS(); } } while (0)

PHP_METHOD(SwooleMongo_Driver_Cursor, setTypeMap)
{
    zval *tm;
    swm_cursor_obj *c = THIS_CURSOR();
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(tm)
    ZEND_PARSE_PARAMETERS_END();
    swm_error_init(&err);
    swm_typemap_dtor(&c->tm);
    if (swm_typemap_parse(tm, &c->tm, &err) == FAILURE) THROW_ERR(err);
    c->has_tm = 1;
    /* re-decode an already loaded current document */
    if (Z_TYPE(c->current) != IS_UNDEF) {
        zval *d = zend_hash_index_find(Z_ARRVAL(c->batch), c->pos);
        zval_ptr_dtor(&c->current);
        ZVAL_UNDEF(&c->current);
        if (d && swm_bson_decode_tm(Z_STRVAL_P(d), Z_STRLEN_P(d), &c->tm, &c->current, &err) == FAILURE) THROW_ERR(err);
    }
}

PHP_METHOD(SwooleMongo_Driver_Cursor, rewind)
{
    swm_cursor_obj *c = THIS_CURSOR();
    ZEND_PARSE_PARAMETERS_NONE();
    ENSURE_CURSOR(c);
    if (c->advanced) {
        swm_throw(swm_ce_LogicException, "Cursors cannot rewind after starting iteration");
        RETURN_THROWS();
    }
    if (!c->started) {
        c->started = 1;
        CURSOR_CALL(cursor_load(c, &err));
    }
}

PHP_METHOD(SwooleMongo_Driver_Cursor, next)
{
    swm_cursor_obj *c = THIS_CURSOR();
    ZEND_PARSE_PARAMETERS_NONE();
    ENSURE_CURSOR(c);
    if (!c->started) {
        c->started = 1;
        CURSOR_CALL(cursor_load(c, &err));
        return;
    }
    if (Z_TYPE(c->current) != IS_UNDEF) {
        c->pos++;
        c->key++;
    }
    c->advanced = 1;
    CURSOR_CALL(cursor_load(c, &err));
}

PHP_METHOD(SwooleMongo_Driver_Cursor, valid)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(Z_TYPE(THIS_CURSOR()->current) != IS_UNDEF);
}

PHP_METHOD(SwooleMongo_Driver_Cursor, current)
{
    swm_cursor_obj *c = THIS_CURSOR();
    ZEND_PARSE_PARAMETERS_NONE();
    if (Z_TYPE(c->current) == IS_UNDEF) RETURN_NULL();
    RETURN_COPY(&c->current);
}

PHP_METHOD(SwooleMongo_Driver_Cursor, key)
{
    swm_cursor_obj *c = THIS_CURSOR();
    ZEND_PARSE_PARAMETERS_NONE();
    if (Z_TYPE(c->current) == IS_UNDEF) RETURN_NULL();
    RETURN_LONG(c->key);
}

PHP_METHOD(SwooleMongo_Driver_Cursor, toArray)
{
    swm_cursor_obj *c = THIS_CURSOR();
    swm_error err;
    ZEND_PARSE_PARAMETERS_NONE();
    ENSURE_CURSOR(c);
    if (c->advanced) {
        swm_throw(swm_ce_LogicException, "Cursors cannot yield multiple iterators");
        RETURN_THROWS();
    }
    swm_error_init(&err);
    array_init(return_value);
    if (!c->started) {
        c->started = 1;
        if (cursor_load(c, &err) == FAILURE) { zval_ptr_dtor(return_value); ZVAL_UNDEF(return_value); THROW_ERR(err); }
    }
    while (Z_TYPE(c->current) != IS_UNDEF) {
        Z_TRY_ADDREF(c->current);
        add_next_index_zval(return_value, &c->current);
        c->pos++;
        c->key++;
        c->advanced = 1;
        if (cursor_load(c, &err) == FAILURE) { zval_ptr_dtor(return_value); ZVAL_UNDEF(return_value); THROW_ERR(err); }
        if (c->tailable && Z_TYPE(c->current) == IS_UNDEF) break;
    }
}

PHP_METHOD(SwooleMongo_Driver_Cursor, getId)
{
    ZEND_PARSE_PARAMETERS_NONE();
    swm_new_int64(return_value, THIS_CURSOR()->id);
}

PHP_METHOD(SwooleMongo_Driver_Cursor, getServer)
{
    swm_cursor_obj *c = THIS_CURSOR();
    ZEND_PARSE_PARAMETERS_NONE();
    ENSURE_CURSOR(c);
    swm_new_server(return_value, c->client, c->server);
}

PHP_METHOD(SwooleMongo_Driver_Cursor, isDead)
{
    swm_cursor_obj *c = THIS_CURSOR();
    ZEND_PARSE_PARAMETERS_NONE();
    if (c->id != 0) RETURN_FALSE;
    if (!c->started) RETURN_BOOL(Z_TYPE(c->batch) != IS_ARRAY || zend_hash_num_elements(Z_ARRVAL(c->batch)) == 0);
    RETURN_BOOL(Z_TYPE(c->current) == IS_UNDEF);
}

PHP_METHOD(SwooleMongo_Driver_Cursor, bufferedCount)
{
    swm_cursor_obj *c = THIS_CURSOR();
    uint32_t n;
    ZEND_PARSE_PARAMETERS_NONE();
    if (Z_TYPE(c->batch) != IS_ARRAY) RETURN_LONG(0);
    n = zend_hash_num_elements(Z_ARRVAL(c->batch));
    RETURN_LONG(n > c->pos ? (zend_long) (n - c->pos) : 0);
}

PHP_METHOD(SwooleMongo_Driver_Cursor, getPostBatchResumeToken)
{
    swm_cursor_obj *c = THIS_CURSOR();
    ZEND_PARSE_PARAMETERS_NONE();
    if (Z_TYPE(c->pbrt) == IS_UNDEF) RETURN_NULL();
    if (Z_TYPE(c->pbrt) == IS_ARRAY) { swm_array_to_object_doc(&c->pbrt, return_value); return; }
    RETURN_COPY(&c->pbrt);
}

PHP_METHOD(SwooleMongo_Driver_Cursor, kill)
{
    swm_cursor_obj *c = THIS_CURSOR();
    ZEND_PARSE_PARAMETERS_NONE();
    ENSURE_CURSOR(c);
    if (c->id != 0) {
        zval cmd, ids, idz, reply;
        swm_exec_opts o;
        swm_error err;
        memset(&o, 0, sizeof(o));
        swm_error_init(&err);
        array_init(&cmd);
        add_assoc_str(&cmd, "killCursors", zend_string_copy(c->coll));
        array_init(&ids);
        swm_new_int64(&idz, c->id);
        add_next_index_zval(&ids, &idz);
        add_assoc_zval(&cmd, "cursors", &ids);
        o.server = c->server;
        o.use_conn = c->pinned_conn;
        if (Z_TYPE(c->session) == IS_OBJECT) o.session = &c->session;
        c->id = 0;
        if (swm_execute(c->client, c->db, &cmd, &o, &reply, NULL, NULL, &err) == SUCCESS) zval_ptr_dtor(&reply);
        if (o.pinned_dead) c->pinned_conn = NULL;
        swm_error_clear(&err);
        zval_ptr_dtor(&cmd);
    }
    if (c->pinned_conn) {
        swm_conn_unpin(c->client, c->pinned_conn, 0);
        c->pinned_conn = NULL;
    }
    zval_ptr_dtor(&c->batch);
    array_init(&c->batch);
    zval_ptr_dtor(&c->current);
    ZVAL_UNDEF(&c->current);
    c->started = 1;
}

/* ================================================== command execution core */
enum { EXEC_GENERIC = 0, EXEC_READ, EXEC_WRITE, EXEC_READWRITE };

static zend_bool is_retryable_write_cmd(zval *cmd)
{
    zend_string *k = NULL;
    zend_ulong idx;
    zval *v;
    ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(cmd), idx, k, v) { (void) idx; (void) v; break; } ZEND_HASH_FOREACH_END();
    if (!k) return 0;
    return zend_string_equals_literal_ci(k, "findAndModify") || zend_string_equals_literal_ci(k, "insert")
        || zend_string_equals_literal_ci(k, "update") || zend_string_equals_literal_ci(k, "delete");
}

static zend_bool pipeline_is_changestream(zval *cmd)
{
    zval *p = hfind(cmd, "pipeline"), *first;
    if (!p) return 0;
    if (Z_TYPE_P(p) == IS_ARRAY && (first = zend_hash_index_find(Z_ARRVAL_P(p), 0)) && Z_TYPE_P(first) == IS_ARRAY) {
        return zend_hash_str_exists(Z_ARRVAL_P(first), ZEND_STRL("$changeStream"));
    }
    return 0;
}

static void apply_default_concern(swm_client *c, zval *cmd, const char *key, zval *opt_obj, zend_bool is_rc)
{
    zval arr;
    if (zend_hash_str_exists(Z_ARRVAL_P(cmd), key, strlen(key))) return;
    if (opt_obj) {
        if (is_rc) rc_to_array(opt_obj, &arr); else wc_to_array(opt_obj, &arr);
    } else {
        swm_client_get_option(c, is_rc ? "readConcern" : "writeConcern", &arr);
        if (Z_TYPE(arr) != IS_ARRAY) { zval_ptr_dtor(&arr); ZVAL_UNDEF(&arr); }
    }
    if (Z_TYPE(arr) == IS_ARRAY) zend_hash_str_update(Z_ARRVAL_P(cmd), key, strlen(key), &arr);
}

static void exec_command(swm_client *c, zend_string *prefer, zend_string *db, zval *command, zval *options, int mode, zval *return_value)
{
    void *pinned_slot = NULL;
    zval cmd, reply, *rp = NULL, *sess = NULL, *rc = NULL, *wc = NULL, *v, primary;
    zend_string *raw = NULL, *server = NULL;
    swm_exec_opts o;
    swm_error err;
    zval *copts = PROP(Z_OBJ_P(command), 1);
    zend_long batch = 0, max_await = 0;
    zend_bool tailable;

    swm_error_init(&err);
    memset(&o, 0, sizeof(o));
    ZVAL_UNDEF(&primary);
    if (opt_object(options, "session", swm_ce_Session, &sess, &err) == FAILURE
        || opt_timeout(options, &o.timeout_us, &err) == FAILURE
        || ((mode == EXEC_GENERIC || mode == EXEC_READ) && opt_object(options, "readPreference", swm_ce_ReadPreference, &rp, &err) == FAILURE)
        || ((mode == EXEC_READ || mode == EXEC_READWRITE) && opt_object(options, "readConcern", swm_ce_ReadConcern, &rc, &err) == FAILURE)
        || ((mode == EXEC_WRITE || mode == EXEC_READWRITE) && opt_object(options, "writeConcern", swm_ce_WriteConcern, &wc, &err) == FAILURE)) {
        swm_error_throw(&err);
        return;
    }
    if (doc_to_array(PROP(Z_OBJ_P(command), 0), &cmd, &err) == FAILURE) {
        if (EG(exception)) swm_error_clear(&err); else swm_error_throw(&err);
        return;
    }
    if (mode == EXEC_READ || mode == EXEC_READWRITE) apply_default_concern(c, &cmd, "readConcern", rc, 1);
    if (mode == EXEC_WRITE || mode == EXEC_READWRITE) apply_default_concern(c, &cmd, "writeConcern", wc, 0);

    o.session = sess;
    o.prefer = prefer;
    switch (mode) {
        case EXEC_GENERIC:
            if (rp) o.read_pref = rp; else { ZVAL_STRING(&primary, "primary"); o.read_pref = &primary; }
            break;
        case EXEC_READ:
            o.read_pref = rp; /* NULL: manager default */
            o.retryable = 1;
            o.causal_read = 1;
            break;
        case EXEC_WRITE:
            o.write = 1;
            o.retryable = is_retryable_write_cmd(&cmd);
            break;
        default:
            o.write = 1;
    }
    if ((v = hfind(&cmd, "cursor")) && (v = hfind(v, "batchSize"))) batch = zval_get_long(v);
    if (copts && (v = hfind(copts, "maxAwaitTimeMS"))) max_await = zval_get_long(v);
    tailable = max_await > 0 || ((v = hfind(&cmd, "tailable")) && zend_is_true(v)) || pipeline_is_changestream(&cmd);
    if (swm_client_load_balanced(c)) o.pin_out = &pinned_slot;

    if (swm_execute(c, db, &cmd, &o, &reply, &raw, &server, &err) == FAILURE) {
        zval_ptr_dtor(&cmd);
        zval_ptr_dtor(&primary);
        if (EG(exception)) swm_error_clear(&err); else swm_error_throw(&err);
        return;
    }
    cursor_setup(return_value, c, server, raw, &reply, batch, max_await, tailable, hfind(&cmd, "comment"), sess, db);
    swm_cursor_from(Z_OBJ_P(return_value))->timeout_us = o.timeout_us;
    if (o.pin_out) swm_cursor_from(Z_OBJ_P(return_value))->pinned_conn = *o.pin_out;
    zend_string_release(raw);
    zval_ptr_dtor(&reply);
    zval_ptr_dtor(&cmd);
    zval_ptr_dtor(&primary);
}

static const char *query_passthrough[] = {
    "allowDiskUse", "allowPartialResults", "awaitData", "batchSize", "comment", "let", "max", "maxTimeMS", "min",
    "noCursorTimeout", "oplogReplay", "returnKey", "showRecordId", "singleBatch", "skip", "tailable", NULL,
};
static const char *query_docs[] = {"projection", "sort", "collation", "hint", NULL};

static void exec_query(swm_client *c, zend_string *prefer, zend_string *ns, zval *query, zval *options, zval *return_value)
{
    void *pinned_slot = NULL;
    zval cmd, reply, *rp = NULL, *sess = NULL, *qopts = PROP(Z_OBJ_P(query), 1), *v, f;
    zend_string *db = NULL, *coll = NULL, *raw = NULL, *server = NULL;
    swm_exec_opts o;
    swm_error err;
    zend_long batch = 0, max_await = 0;
    zend_bool tailable = 0;

    swm_error_init(&err);
    memset(&o, 0, sizeof(o));
    if (opt_object(options, "session", swm_ce_Session, &sess, &err) == FAILURE
        || opt_timeout(options, &o.timeout_us, &err) == FAILURE
        || opt_object(options, "readPreference", swm_ce_ReadPreference, &rp, &err) == FAILURE
        || split_ns(ns, &db, &coll, &err) == FAILURE) {
        swm_error_throw(&err);
        return;
    }
    array_init(&cmd);
    add_assoc_str(&cmd, "find", zend_string_copy(coll));
    if (as_document(PROP(Z_OBJ_P(query), 0), &f, 0, &err) == FAILURE) goto fail;
    add_assoc_zval(&cmd, "filter", &f);
    if (qopts && Z_TYPE_P(qopts) == IS_ARRAY) {
        for (int i = 0; query_passthrough[i]; i++) {
            if ((v = hfind(qopts, query_passthrough[i])) && Z_TYPE_P(v) != IS_NULL) {
                Z_TRY_ADDREF_P(v);
                add_assoc_zval(&cmd, query_passthrough[i], v);
            }
        }
        for (int i = 0; query_docs[i]; i++) {
            if ((v = hfind(qopts, query_docs[i])) && Z_TYPE_P(v) != IS_NULL) {
                zval d;
                if (as_document(v, &d, 0, &err) == FAILURE) goto fail;
                add_assoc_zval(&cmd, query_docs[i], &d);
            }
        }
        if ((v = hfind(qopts, "limit")) && zval_get_long(v) != 0) {
            zend_long l = zval_get_long(v);
            if (l < 0) { add_assoc_long(&cmd, "limit", -l); add_assoc_bool(&cmd, "singleBatch", 1); }
            else add_assoc_long(&cmd, "limit", l);
        }
        if ((v = hfind(qopts, "readConcern")) && Z_TYPE_P(v) == IS_OBJECT) {
            zval a;
            rc_to_array(v, &a);
            if (Z_TYPE(a) == IS_ARRAY) add_assoc_zval(&cmd, "readConcern", &a);
        }
        if ((v = hfind(qopts, "batchSize"))) batch = zval_get_long(v);
        if ((v = hfind(qopts, "maxAwaitTimeMS"))) max_await = zval_get_long(v);
        if ((v = hfind(qopts, "tailable"))) tailable = zend_is_true(v);
    }
    apply_default_concern(c, &cmd, "readConcern", NULL, 1);
    o.session = sess;
    o.read_pref = rp;
    o.retryable = 1;
    o.causal_read = 1;
    o.prefer = prefer;
    if (swm_client_load_balanced(c)) o.pin_out = &pinned_slot;
    if (swm_execute(c, db, &cmd, &o, &reply, &raw, &server, &err) == FAILURE) goto fail;
    cursor_setup(return_value, c, server, raw, &reply, batch, max_await, tailable, hfind(&cmd, "comment"), sess, db);
    swm_cursor_from(Z_OBJ_P(return_value))->timeout_us = o.timeout_us;
    if (o.pin_out) swm_cursor_from(Z_OBJ_P(return_value))->pinned_conn = *o.pin_out;
    zend_string_release(raw);
    zval_ptr_dtor(&reply);
    zval_ptr_dtor(&cmd);
    zend_string_release(db);
    zend_string_release(coll);
    return;
fail:
    zval_ptr_dtor(&cmd);
    zend_string_release(db);
    zend_string_release(coll);
    if (EG(exception)) swm_error_clear(&err); else swm_error_throw(&err);
}

/* ================================================================ BulkWrite */
/* props: 0 ops (list of [kind, bson, multi]), 1 options, 2 executed, 3 database, 4 collection */
enum { BW_INSERT = 1, BW_UPDATE = 2, BW_DELETE = 3 };

static void bulk_add(zend_object *o, int kind, zend_string *bson, zend_bool multi)
{
    zval op, *ops = PROP(o, 0);
    if (Z_TYPE_P(ops) != IS_ARRAY) { zval_ptr_dtor(ops); array_init(ops); }
    SEPARATE_ARRAY(ops);
    array_init(&op);
    add_next_index_long(&op, kind);
    add_next_index_str(&op, bson);
    add_next_index_bool(&op, multi);
    add_next_index_zval(ops, &op);
}

static zend_string *encode_root(zval *doc, swm_error *err)
{
    smart_str s = {0};
    if (swm_bson_encode(&s, doc, err) == FAILURE) { smart_str_free(&s); return NULL; }
    smart_str_0(&s);
    return s.s;
}

PHP_METHOD(SwooleMongo_Driver_BulkWrite, __construct)
{
    zval *options = NULL, ops;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    array_init(&ops);
    prop_set(THIS_OBJ, 0, &ops);
    zval_ptr_dtor(&ops);
    if (options) prop_set(THIS_OBJ, 1, options);
}

PHP_METHOD(SwooleMongo_Driver_BulkWrite, insert)
{
    zval *doc;
    zend_string *bson;
    const char *v;
    size_t vl;
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(doc)
    ZEND_PARSE_PARAMETERS_END();
    if (Z_TYPE_P(doc) != IS_ARRAY && Z_TYPE_P(doc) != IS_OBJECT) {
        swm_throw(swm_ce_InvalidArgumentException, "Expected document to be array or object, %s given", zend_zval_type_name(doc));
        RETURN_THROWS();
    }
    swm_error_init(&err);
    if (!(bson = encode_root(doc, &err))) THROW_ERR(err);
    if (swm_bson_find(ZSTR_VAL(bson), ZSTR_LEN(bson), "_id", &v, &vl)) {
        /* return the existing _id decoded like a top level value */
        zval tmp, *idv;
        smart_str s = {0};
        const char *elem = v - 4 - 1; /* type byte + "_id\0" precede the value */
        size_t total = 4 + 1 + 4 + vl + 1;
        smart_str_appendl(&s, "\0\0\0\0", 4);
        smart_str_appendl(&s, elem, 1 + 4 + vl);
        smart_str_appendc(&s, 0);
        ZSTR_VAL(s.s)[0] = (char) total; ZSTR_VAL(s.s)[1] = (char) (total >> 8); ZSTR_VAL(s.s)[2] = (char) (total >> 16); ZSTR_VAL(s.s)[3] = (char) (total >> 24);
        if (swm_bson_decode_tm(ZSTR_VAL(s.s), ZSTR_LEN(s.s), NULL, &tmp, &err) == SUCCESS) {
            idv = zend_hash_str_find(Z_OBJPROP(tmp), ZEND_STRL("_id"));
            if (idv) RETVAL_COPY(idv);
            zval_ptr_dtor(&tmp);
        }
        smart_str_free(&s);
        swm_error_clear(&err);
    } else {
        /* prepend a generated ObjectId */
        zval oid;
        smart_str s = {0};
        size_t total;
        swm_new_objectid(&oid);
        {
            zval one;
            zend_string *ob;
            array_init(&one);
            Z_ADDREF(oid);
            add_assoc_zval(&one, "_id", &oid);
            ob = encode_root(&one, &err);
            zval_ptr_dtor(&one);
            /* ob = len | 07 "_id" 0 | 12 bytes | 00 */
            smart_str_appendl(&s, ZSTR_VAL(ob), ZSTR_LEN(ob) - 1);
            smart_str_appendl(&s, ZSTR_VAL(bson) + 4, ZSTR_LEN(bson) - 4);
            zend_string_release(ob);
        }
        total = ZSTR_LEN(s.s);
        ZSTR_VAL(s.s)[0] = (char) total; ZSTR_VAL(s.s)[1] = (char) (total >> 8); ZSTR_VAL(s.s)[2] = (char) (total >> 16); ZSTR_VAL(s.s)[3] = (char) (total >> 24);
        smart_str_0(&s);
        zend_string_release(bson);
        bson = s.s;
        RETVAL_COPY_VALUE(&oid);
    }
    bulk_add(THIS_OBJ, BW_INSERT, bson, 0);
}

PHP_METHOD(SwooleMongo_Driver_BulkWrite, update)
{
    zval *filter, *newobj, *options = NULL, item, q, u, *v;
    zend_bool multi = 0, is_update;
    zend_string *bson;
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_ZVAL(filter)
        Z_PARAM_ZVAL(newobj)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    if ((Z_TYPE_P(filter) != IS_ARRAY && Z_TYPE_P(filter) != IS_OBJECT) || (Z_TYPE_P(newobj) != IS_ARRAY && Z_TYPE_P(newobj) != IS_OBJECT)) {
        swm_throw(swm_ce_InvalidArgumentException, "Expected filter and newObj to be array or object");
        RETURN_THROWS();
    }
    swm_error_init(&err);
    if ((v = hfind(options, "multi"))) multi = zend_is_true(v);
    if (as_document(filter, &q, 0, &err) == FAILURE) THROW_ERR(err);
    if (as_document(newobj, &u, 1, &err) == FAILURE) { zval_ptr_dtor(&q); THROW_ERR(err); }
    if (Z_OBJCE(u) == swm_ce_PackedArray) {
        is_update = 1;
    } else {
        zend_string *r = Z_STR_P(PROP(Z_OBJ(u), 0));
        is_update = ZSTR_LEN(r) > 6 && ZSTR_VAL(r)[5] == '$';
    }
    if (multi && !is_update) {
        zval_ptr_dtor(&q);
        zval_ptr_dtor(&u);
        swm_throw(swm_ce_InvalidArgumentException, "Replacement document conflicts with true \"multi\" option");
        RETURN_THROWS();
    }
    array_init(&item);
    add_assoc_zval(&item, "q", &q);
    add_assoc_zval(&item, "u", &u);
    add_assoc_bool(&item, "multi", multi);
    add_assoc_bool(&item, "upsert", (v = hfind(options, "upsert")) && zend_is_true(v));
    {
        static const char *pass[] = {"arrayFilters", "collation", "hint", "sort", NULL};
        for (int i = 0; pass[i]; i++) {
            if ((v = hfind(options, pass[i])) && Z_TYPE_P(v) != IS_NULL) {
                zval d;
                if (as_document(v, &d, !strcmp(pass[i], "arrayFilters"), &err) == FAILURE) { zval_ptr_dtor(&item); THROW_ERR(err); }
                add_assoc_zval(&item, pass[i], &d);
            }
        }
    }
    bson = encode_root(&item, &err);
    zval_ptr_dtor(&item);
    if (!bson) THROW_ERR(err);
    bulk_add(THIS_OBJ, BW_UPDATE, bson, multi);
}

PHP_METHOD(SwooleMongo_Driver_BulkWrite, delete)
{
    zval *filter, *options = NULL, item, q, *v;
    zend_bool one;
    zend_string *bson;
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ZVAL(filter)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    if (Z_TYPE_P(filter) != IS_ARRAY && Z_TYPE_P(filter) != IS_OBJECT) {
        swm_throw(swm_ce_InvalidArgumentException, "Expected filter to be array or object");
        RETURN_THROWS();
    }
    swm_error_init(&err);
    one = (v = hfind(options, "limit")) && zend_is_true(v);
    if (as_document(filter, &q, 0, &err) == FAILURE) THROW_ERR(err);
    array_init(&item);
    add_assoc_zval(&item, "q", &q);
    add_assoc_long(&item, "limit", one ? 1 : 0);
    if ((v = hfind(options, "collation")) && Z_TYPE_P(v) != IS_NULL) { zval d; as_document(v, &d, 0, &err); add_assoc_zval(&item, "collation", &d); }
    if ((v = hfind(options, "hint")) && Z_TYPE_P(v) != IS_NULL) { zval d; as_document(v, &d, 0, &err); add_assoc_zval(&item, "hint", &d); }
    bson = encode_root(&item, &err);
    zval_ptr_dtor(&item);
    if (!bson) THROW_ERR(err);
    bulk_add(THIS_OBJ, BW_DELETE, bson, !one);
}

PHP_METHOD(SwooleMongo_Driver_BulkWrite, count)
{
    zval *ops = PROP(THIS_OBJ, 0);
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(Z_TYPE_P(ops) == IS_ARRAY ? zend_hash_num_elements(Z_ARRVAL_P(ops)) : 0);
}

typedef struct {
    zend_long n[5]; /* inserted, matched, modified, removed, upserted */
    zval upserted;  /* index => id */
    zval write_errors;
    zval wc_error;
    zend_string *server;
} bulk_state;

static int64_t g_bulk_timeout = 0;

static int bulk_run_batch(swm_client *c, zend_string *prefer, zend_string *db, zend_string *coll, int kind, zval *batch, zval *index_map,
                          zend_bool has_multi, zval *bopts, zval *wc_arr, zval *sess, bulk_state *st, zend_bool ordered, swm_error *err)
{
    static const char *cmd_names[] = {NULL, "insert", "update", "delete"};
    static const char *seq_names[] = {NULL, "documents", "updates", "deletes"};
    zval cmd, seqs, reply, *v, *e;
    swm_exec_opts o;
    zend_string *server = NULL;
    int rc;
    memset(&o, 0, sizeof(o));
    array_init(&cmd);
    add_assoc_str(&cmd, cmd_names[kind], zend_string_copy(coll));
    add_assoc_bool(&cmd, "ordered", ordered);
    if ((v = hfind(bopts, "bypassDocumentValidation")) && Z_TYPE_P(v) != IS_NULL) add_assoc_bool(&cmd, "bypassDocumentValidation", zend_is_true(v));
    if ((v = hfind(bopts, "comment")) && Z_TYPE_P(v) != IS_NULL) { Z_TRY_ADDREF_P(v); add_assoc_zval(&cmd, "comment", v); }
    if (kind != BW_INSERT && (v = hfind(bopts, "let")) && Z_TYPE_P(v) != IS_NULL) {
        zval d;
        if (as_document(v, &d, 0, err) == FAILURE) { zval_ptr_dtor(&cmd); return FAILURE; }
        add_assoc_zval(&cmd, "let", &d);
    }
    if (wc_arr && Z_TYPE_P(wc_arr) == IS_ARRAY) { Z_ADDREF_P(wc_arr); add_assoc_zval(&cmd, "writeConcern", wc_arr); }
    array_init(&seqs);
    Z_ADDREF_P(batch);
    add_assoc_zval(&seqs, seq_names[kind], batch);
    o.sequences = &seqs;
    o.write = 1;
    o.retryable = !has_multi;
    o.session = sess;
    o.prefer = prefer;
    o.timeout_us = g_bulk_timeout;
    rc = swm_execute(c, db, &cmd, &o, &reply, NULL, &server, err);
    zval_ptr_dtor(&cmd);
    zval_ptr_dtor(&seqs);
    if (rc == FAILURE) return FAILURE;
    if (st->server) zend_string_release(st->server);
    st->server = server;
    {
        zend_long n = zval_get_long((v = hfind(&reply, "n")) ? v : &EG(uninitialized_zval));
        if (kind == BW_INSERT) {
            st->n[0] += n;
        } else if (kind == BW_UPDATE) {
            zval *ups = hfind(&reply, "upserted");
            zend_long nu = ups && Z_TYPE_P(ups) == IS_ARRAY ? zend_hash_num_elements(Z_ARRVAL_P(ups)) : 0;
            st->n[4] += nu;
            st->n[1] += n - nu;
            st->n[2] += zval_get_long((v = hfind(&reply, "nModified")) ? v : &EG(uninitialized_zval));
            if (nu) {
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(ups), e) {
                    zval *idx = hfind(e, "index"), *id = hfind(e, "_id"), *orig, idobj;
                    if (!idx || !id) continue;
                    orig = zend_hash_index_find(Z_ARRVAL_P(index_map), zval_get_long(idx));
                    {
                        zval wrap;
                        array_init(&wrap);
                        Z_TRY_ADDREF_P(id);
                        add_assoc_zval(&wrap, "v", id);
                        swm_array_to_object_doc(&wrap, &idobj);
                        zval_ptr_dtor(&wrap);
                    }
                    {
                        zval *val = Z_TYPE(idobj) == IS_OBJECT ? zend_hash_str_find(Z_OBJPROP(idobj), ZEND_STRL("v")) : NULL;
                        if (val) { Z_TRY_ADDREF_P(val); zend_hash_index_update(Z_ARRVAL(st->upserted), orig ? zval_get_long(orig) : zval_get_long(idx), val); }
                    }
                    zval_ptr_dtor(&idobj);
                } ZEND_HASH_FOREACH_END();
            }
        } else {
            st->n[3] += n;
        }
    }
    if ((v = hfind(&reply, "writeErrors")) && Z_TYPE_P(v) == IS_ARRAY) {
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(v), e) {
            zval we, *idx = hfind(e, "index"), *orig;
            zend_long i = idx ? zval_get_long(idx) : 0;
            orig = zend_hash_index_find(Z_ARRVAL_P(index_map), i);
            new_write_error(&we, e, orig ? zval_get_long(orig) : i);
            add_next_index_zval(&st->write_errors, &we);
        } ZEND_HASH_FOREACH_END();
    }
    if ((v = hfind(&reply, "writeConcernError")) && Z_TYPE_P(v) == IS_ARRAY) {
        zval_ptr_dtor(&st->wc_error);
        new_wc_error(&st->wc_error, v);
    }
    zval_ptr_dtor(&reply);
    return SUCCESS;
}

static void exec_bulk(swm_client *c, zend_string *prefer, zend_string *ns, zval *bulk, zval *options, zval *return_value)
{
    zend_object *bo = Z_OBJ_P(bulk);
    zval *ops = PROP(bo, 0), *bopts = PROP(bo, 1), *sess = NULL, *wc = NULL, wc_arr, *v, *op;
    zend_string *db = NULL, *coll = NULL;
    zend_bool ordered = 1, acknowledged;
    bulk_state st;
    swm_error err;
    int order[3] = {BW_INSERT, BW_UPDATE, BW_DELETE};
    zend_long max_batch = swm_client_max_batch(c);
    size_t max_bytes = (size_t) 47 * 1024 * 1024;

    /* auto encryption: libmongocrypt needs commands below 2 MiB before encryption */
    if (swm_client_crypt(c)) max_bytes = (size_t) 2 * 1024 * 1024;
    swm_error_init(&err);
    memset(&st, 0, sizeof(st));
    if (Z_TYPE_P(PROP(bo, 2)) == IS_TRUE) {
        swm_throw(swm_ce_InvalidArgumentException, "BulkWrite objects may only be executed once and this instance has already been executed");
        return;
    }
    if (Z_TYPE_P(ops) != IS_ARRAY || zend_hash_num_elements(Z_ARRVAL_P(ops)) == 0) {
        swm_throw(swm_ce_InvalidArgumentException, "Cannot do an empty bulk write");
        return;
    }
    if (opt_object(options, "session", swm_ce_Session, &sess, &err) == FAILURE
        || opt_timeout(options, &g_bulk_timeout, &err) == FAILURE
        || opt_object(options, "writeConcern", swm_ce_WriteConcern, &wc, &err) == FAILURE
        || split_ns(ns, &db, &coll, &err) == FAILURE) {
        swm_error_throw(&err);
        return;
    }
    { zval t; ZVAL_TRUE(&t); prop_set(bo, 2, &t); }
    if ((v = hfind(bopts, "ordered"))) ordered = zend_is_true(v);
    if (wc) wc_to_array(wc, &wc_arr);
    else { swm_client_get_option(c, "writeConcern", &wc_arr); if (Z_TYPE(wc_arr) != IS_ARRAY) { zval_ptr_dtor(&wc_arr); ZVAL_UNDEF(&wc_arr); } }
    acknowledged = !(Z_TYPE(wc_arr) == IS_ARRAY && (v = hfind(&wc_arr, "w")) && Z_TYPE_P(v) == IS_LONG && Z_LVAL_P(v) == 0
                     && !((v = hfind(&wc_arr, "j")) && zend_is_true(v)));
    array_init(&st.upserted);
    array_init(&st.write_errors);
    ZVAL_NULL(&st.wc_error);

    /* groups: ordered = consecutive runs of the same kind, unordered = by kind */
    {
        uint32_t nops = zend_hash_num_elements(Z_ARRVAL_P(ops));
        uint32_t i = 0;
        int pass = 0;
        zend_bool stop = 0;
        while (!stop && (ordered ? i < nops : pass < 3)) {
            int kind;
            zval batch, map;
            size_t bytes = 0;
            zend_bool multi = 0;
            uint32_t j;
            if (ordered) {
                op = zend_hash_index_find(Z_ARRVAL_P(ops), i);
                kind = (int) Z_LVAL_P(zend_hash_index_find(Z_ARRVAL_P(op), 0));
            } else {
                kind = order[pass];
            }
            array_init(&batch);
            array_init(&map);
            for (j = ordered ? i : 0; j < nops; j++) {
                zval *b;
                op = zend_hash_index_find(Z_ARRVAL_P(ops), j);
                if ((int) Z_LVAL_P(zend_hash_index_find(Z_ARRVAL_P(op), 0)) != kind) {
                    if (ordered) break;
                    continue;
                }
                b = zend_hash_index_find(Z_ARRVAL_P(op), 1);
                if (zend_hash_num_elements(Z_ARRVAL(batch)) && ((zend_long) zend_hash_num_elements(Z_ARRVAL(batch)) >= max_batch || bytes + Z_STRLEN_P(b) > max_bytes)) {
                    if (bulk_run_batch(c, prefer, db, coll, kind, &batch, &map, multi, bopts, &wc_arr, sess, &st, ordered, &err) == FAILURE) goto fail_batch;
                    zval_ptr_dtor(&batch);
                    zval_ptr_dtor(&map);
                    array_init(&batch);
                    array_init(&map);
                    bytes = 0;
                    multi = 0;
                    if (ordered && zend_hash_num_elements(Z_ARRVAL(st.write_errors))) { stop = 1; break; }
                }
                Z_ADDREF_P(b);
                add_next_index_zval(&batch, b);
                add_next_index_long(&map, (zend_long) j);
                bytes += Z_STRLEN_P(b);
                multi = multi || zend_is_true(zend_hash_index_find(Z_ARRVAL_P(op), 2));
            }
            if (!stop && zend_hash_num_elements(Z_ARRVAL(batch))) {
                if (bulk_run_batch(c, prefer, db, coll, kind, &batch, &map, multi, bopts, &wc_arr, sess, &st, ordered, &err) == FAILURE) goto fail_batch;
                if (ordered && zend_hash_num_elements(Z_ARRVAL(st.write_errors))) stop = 1;
            }
            zval_ptr_dtor(&batch);
            zval_ptr_dtor(&map);
            if (ordered) i = j; else pass++;
            continue;
fail_batch:
            zval_ptr_dtor(&batch);
            zval_ptr_dtor(&map);
            goto fail;
        }
    }

    /* WriteResult */
    object_init_ex(return_value, swm_ce_WriteResult);
    for (int k = 0; k < 5; k++) prop_set_long(Z_OBJ_P(return_value), k, st.n[k]);
    prop_set(Z_OBJ_P(return_value), 5, &st.upserted);
    prop_set(Z_OBJ_P(return_value), 6, &st.write_errors);
    prop_set(Z_OBJ_P(return_value), 7, &st.wc_error);
    { zval b; ZVAL_BOOL(&b, acknowledged); prop_set(Z_OBJ_P(return_value), 8, &b); }
    if (st.server) {
        zval srv;
        swm_new_server(&srv, c, st.server);
        prop_set(Z_OBJ_P(return_value), 9, &srv);
        zval_ptr_dtor(&srv);
    }
    if (zend_hash_num_elements(Z_ARRVAL(st.write_errors)) || Z_TYPE(st.wc_error) == IS_OBJECT) {
        zval ex, *first = zend_hash_index_find(Z_ARRVAL(st.write_errors), 0);
        zend_string *msg;
        zend_long code;
        if (first) {
            msg = zend_string_copy(Z_STR_P(PROP(Z_OBJ_P(first), 2)));
            code = Z_LVAL_P(PROP(Z_OBJ_P(first), 0));
        } else {
            msg = zend_string_copy(Z_STR_P(PROP(Z_OBJ(st.wc_error), 1)));
            code = Z_LVAL_P(PROP(Z_OBJ(st.wc_error), 0));
        }
        object_init_ex(&ex, swm_ce_BulkWriteException);
        zend_update_property_str(zend_ce_exception, Z_OBJ(ex), ZEND_STRL("message"), msg);
        zend_update_property_long(zend_ce_exception, Z_OBJ(ex), ZEND_STRL("code"), code);
        zend_update_property(swm_ce_WriteException, Z_OBJ(ex), ZEND_STRL("writeResult"), return_value);
        zend_string_release(msg);
        zval_ptr_dtor(return_value);
        ZVAL_UNDEF(return_value);
        zend_throw_exception_object(&ex);
    }
    goto cleanup;
fail:
    if (EG(exception)) swm_error_clear(&err); else swm_error_throw(&err);
cleanup:
    zval_ptr_dtor(&st.upserted);
    zval_ptr_dtor(&st.write_errors);
    zval_ptr_dtor(&st.wc_error);
    if (st.server) zend_string_release(st.server);
    zval_ptr_dtor(&wc_arr);
    zend_string_release(db);
    zend_string_release(coll);
}

/* ======================================================== BulkWriteCommand */
/* props: 0 ops (list of [kind, ns, opdoc array, insertedId|null, multi]), 1 options, 2 executed */
enum { BWC_INSERT = 0, BWC_UPDATE = 1, BWC_DELETE = 2 };

static void bwc_add(zend_object *o, int kind, zend_string *ns, zval *doc, zval *id, zend_bool multi)
{
    zval op, *ops = PROP(o, 0);
    if (Z_TYPE_P(ops) != IS_ARRAY) { zval_ptr_dtor(ops); array_init(ops); }
    SEPARATE_ARRAY(ops);
    array_init(&op);
    add_next_index_long(&op, kind);
    add_next_index_str(&op, zend_string_copy(ns));
    add_next_index_zval(&op, doc);
    if (id) { Z_TRY_ADDREF_P(id); add_next_index_zval(&op, id); } else add_next_index_null(&op);
    add_next_index_bool(&op, multi);
    add_next_index_zval(ops, &op);
}

static int bwc_check_ns(zend_string *ns)
{
    const char *dot = memchr(ZSTR_VAL(ns), '.', ZSTR_LEN(ns));
    if (!dot || dot == ZSTR_VAL(ns) || dot == ZSTR_VAL(ns) + ZSTR_LEN(ns) - 1) {
        swm_throw(swm_ce_InvalidArgumentException, "Invalid namespace provided: %s", ZSTR_VAL(ns));
        return FAILURE;
    }
    return SUCCESS;
}

PHP_METHOD(SwooleMongo_Driver_BulkWriteCommand, __construct)
{
    zval *options = NULL, ops;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    array_init(&ops);
    prop_set(THIS_OBJ, 0, &ops);
    zval_ptr_dtor(&ops);
    if (options) prop_set(THIS_OBJ, 1, options);
}

PHP_METHOD(SwooleMongo_Driver_BulkWriteCommand, count)
{
    zval *ops = PROP(THIS_OBJ, 0);
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(Z_TYPE_P(ops) == IS_ARRAY ? zend_hash_num_elements(Z_ARRVAL_P(ops)) : 0);
}

PHP_METHOD(SwooleMongo_Driver_BulkWriteCommand, insertOne)
{
    zend_string *ns;
    zval *doc, d, *idv;
    zval tmp_bulk;
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(ns)
        Z_PARAM_ZVAL(doc)
    ZEND_PARSE_PARAMETERS_END();
    if (bwc_check_ns(ns) == FAILURE) RETURN_THROWS();
    if (Z_TYPE_P(doc) != IS_ARRAY && Z_TYPE_P(doc) != IS_OBJECT) { swm_throw(swm_ce_InvalidArgumentException, "Expected document to be array or object"); RETURN_THROWS(); }
    /* reuse BulkWrite::insert() for _id generation and encoding */
    object_init_ex(&tmp_bulk, swm_ce_BulkWrite);
    zend_call_method_with_1_params(Z_OBJ(tmp_bulk), swm_ce_BulkWrite, NULL, "insert", return_value, doc);
    if (EG(exception)) { zval_ptr_dtor(&tmp_bulk); RETURN_THROWS(); }
    idv = zend_hash_index_find(Z_ARRVAL_P(zend_hash_index_find(Z_ARRVAL_P(PROP(Z_OBJ(tmp_bulk), 0)), 0)), 1);
    swm_error_init(&err);
    array_init(&d);
    {
        zval raw;
        swm_new_document_raw(&raw, swm_ce_Document, Z_STRVAL_P(idv), Z_STRLEN_P(idv));
        add_assoc_zval(&d, "document", &raw);
    }
    zval_ptr_dtor(&tmp_bulk);
    bwc_add(THIS_OBJ, BWC_INSERT, ns, &d, return_value, 0);
}

static void bwc_update(INTERNAL_FUNCTION_PARAMETERS, zend_bool multi, zend_bool replace)
{
    zend_string *ns;
    zval *filter, *u, *options = NULL, d, q, m, *v;
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(3, 4)
        Z_PARAM_STR(ns)
        Z_PARAM_ZVAL(filter)
        Z_PARAM_ZVAL(u)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    if (bwc_check_ns(ns) == FAILURE) RETURN_THROWS();
    if ((Z_TYPE_P(filter) != IS_ARRAY && Z_TYPE_P(filter) != IS_OBJECT) || (Z_TYPE_P(u) != IS_ARRAY && Z_TYPE_P(u) != IS_OBJECT)) {
        swm_throw(swm_ce_InvalidArgumentException, "Expected filter and update to be array or object");
        RETURN_THROWS();
    }
    swm_error_init(&err);
    if (as_document(filter, &q, 0, &err) == FAILURE) THROW_ERR(err);
    if (as_document(u, &m, !replace, &err) == FAILURE) { zval_ptr_dtor(&q); THROW_ERR(err); }
    if (replace && Z_OBJCE(m) == swm_ce_Document && ZSTR_LEN(Z_STR_P(PROP(Z_OBJ(m), 0))) > 6 && ZSTR_VAL(Z_STR_P(PROP(Z_OBJ(m), 0)))[5] == '$') {
        zval_ptr_dtor(&q); zval_ptr_dtor(&m);
        swm_throw(swm_ce_InvalidArgumentException, "Replacement document must not contain update operators");
        RETURN_THROWS();
    }
    array_init(&d);
    add_assoc_zval(&d, "filter", &q);
    add_assoc_zval(&d, "updateMods", &m);
    add_assoc_bool(&d, "multi", multi);
    if ((v = hfind(options, "upsert")) && zend_is_true(v)) add_assoc_bool(&d, "upsert", 1);
    {
        static const char *pass[] = {"arrayFilters", "collation", "hint", "sort", NULL};
        for (int i = 0; pass[i]; i++) {
            zval x;
            if (!(v = hfind(options, pass[i])) || Z_TYPE_P(v) == IS_NULL) continue;
            if (replace && i == 0) continue;
            if (multi && i == 3) continue;
            if (as_document(v, &x, i == 0, &err) == FAILURE) { zval_ptr_dtor(&d); THROW_ERR(err); }
            add_assoc_zval(&d, pass[i], &x);
        }
    }
    bwc_add(Z_OBJ_P(ZEND_THIS), BWC_UPDATE, ns, &d, NULL, multi);
}

PHP_METHOD(SwooleMongo_Driver_BulkWriteCommand, updateOne) { bwc_update(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0, 0); }
PHP_METHOD(SwooleMongo_Driver_BulkWriteCommand, updateMany) { bwc_update(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1, 0); }
PHP_METHOD(SwooleMongo_Driver_BulkWriteCommand, replaceOne) { bwc_update(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0, 1); }

static void bwc_delete(INTERNAL_FUNCTION_PARAMETERS, zend_bool multi)
{
    zend_string *ns;
    zval *filter, *options = NULL, d, q, *v;
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STR(ns)
        Z_PARAM_ZVAL(filter)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    if (bwc_check_ns(ns) == FAILURE) RETURN_THROWS();
    swm_error_init(&err);
    if (as_document(filter, &q, 0, &err) == FAILURE) THROW_ERR(err);
    array_init(&d);
    add_assoc_zval(&d, "filter", &q);
    add_assoc_bool(&d, "multi", multi);
    if ((v = hfind(options, "collation")) && Z_TYPE_P(v) != IS_NULL) { zval x; as_document(v, &x, 0, &err); add_assoc_zval(&d, "collation", &x); }
    if ((v = hfind(options, "hint")) && Z_TYPE_P(v) != IS_NULL) { zval x; as_document(v, &x, 0, &err); add_assoc_zval(&d, "hint", &x); }
    bwc_add(Z_OBJ_P(ZEND_THIS), BWC_DELETE, ns, &d, NULL, multi);
}

PHP_METHOD(SwooleMongo_Driver_BulkWriteCommand, deleteOne) { bwc_delete(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0); }
PHP_METHOD(SwooleMongo_Driver_BulkWriteCommand, deleteMany) { bwc_delete(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1); }

/* BulkWriteCommandResult props: 0 inserted 1 matched 2 modified 3 upserted 4 deleted 5 insertResults 6 updateResults 7 deleteResults 8 acknowledged */
#define BWR_COUNT(name, idx) \
PHP_METHOD(SwooleMongo_Driver_BulkWriteCommandResult, name) { \
    ZEND_PARSE_PARAMETERS_NONE(); \
    if (Z_TYPE_P(PROP(THIS_OBJ, 8)) != IS_TRUE) { swm_throw(swm_ce_LogicException, "Cannot call %s() for an unacknowledged write result", #name); RETURN_THROWS(); } \
    RETURN_COPY(PROP(THIS_OBJ, idx)); \
}
BWR_COUNT(getInsertedCount, 0)
BWR_COUNT(getMatchedCount, 1)
BWR_COUNT(getModifiedCount, 2)
BWR_COUNT(getUpsertedCount, 3)
BWR_COUNT(getDeletedCount, 4)
GETTER(BulkWriteCommandResult, getInsertResults, 5)
GETTER(BulkWriteCommandResult, getUpdateResults, 6)
GETTER(BulkWriteCommandResult, getDeleteResults, 7)
GETTER(BulkWriteCommandResult, isAcknowledged, 8)

PHP_METHOD(SwooleMongo_Driver_BulkWriteCommandException, getPartialResult)
{
    zval rv, *v;
    ZEND_PARSE_PARAMETERS_NONE();
    v = zend_read_property(swm_ce_BulkWriteCommandException, THIS_OBJ, ZEND_STRL("partialResult"), 1, &rv);
    RETURN_COPY(v);
}
PHP_METHOD(SwooleMongo_Driver_BulkWriteCommandException, getWriteErrors)
{
    zval rv, *v;
    ZEND_PARSE_PARAMETERS_NONE();
    v = zend_read_property(swm_ce_BulkWriteCommandException, THIS_OBJ, ZEND_STRL("writeErrors"), 1, &rv);
    if (Z_TYPE_P(v) == IS_ARRAY) RETURN_COPY(v);
    RETURN_EMPTY_ARRAY();
}
PHP_METHOD(SwooleMongo_Driver_BulkWriteCommandException, getWriteConcernErrors)
{
    zval rv, *v;
    ZEND_PARSE_PARAMETERS_NONE();
    v = zend_read_property(swm_ce_BulkWriteCommandException, THIS_OBJ, ZEND_STRL("writeConcernErrors"), 1, &rv);
    if (Z_TYPE_P(v) == IS_ARRAY) RETURN_COPY(v);
    RETURN_EMPTY_ARRAY();
}
PHP_METHOD(SwooleMongo_Driver_BulkWriteCommandException, getErrorReply)
{
    zval rv, *v;
    ZEND_PARSE_PARAMETERS_NONE();
    v = zend_read_property(swm_ce_BulkWriteCommandException, THIS_OBJ, ZEND_STRL("errorReply"), 1, &rv);
    RETURN_COPY(v);
}

static void bwc_set_result(zval *map, zend_long idx, zval *doc)
{
    zend_hash_index_update(Z_ARRVAL_P(map), idx, doc);
}

/* processes result items {ok, idx, n, nModified, upserted, code, errmsg, errInfo} */
static void bwc_items(zval *items, zval *idx_map, zval *ops, zval *ins, zval *upd, zval *del, zval *errors)
{
    zval *it;
    if (!items || Z_TYPE_P(items) != IS_ARRAY) return;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(items), it) {
        zval *okv = hfind(it, "ok"), *iv = hfind(it, "idx"), *g, *op;
        zend_long local = iv ? zval_get_long(iv) : 0, global;
        int kind;
        g = zend_hash_index_find(Z_ARRVAL_P(idx_map), local);
        global = g ? zval_get_long(g) : local;
        op = zend_hash_index_find(Z_ARRVAL_P(ops), global);
        if (!op) continue;
        kind = (int) Z_LVAL_P(zend_hash_index_find(Z_ARRVAL_P(op), 0));
        if (!okv || !zend_is_true(okv)) {
            zval we;
            new_write_error(&we, it, global);
            zend_hash_index_update(Z_ARRVAL_P(errors), global, &we);
            continue;
        }
        {
            zval r, *v;
            array_init(&r);
            if (kind == BWC_INSERT) {
                zval *id = zend_hash_index_find(Z_ARRVAL_P(op), 3);
                Z_TRY_ADDREF_P(id);
                add_assoc_zval(&r, "insertedId", id);
                bwc_set_result(ins, global, &r);
            } else if (kind == BWC_UPDATE) {
                add_assoc_long(&r, "matchedCount", zval_get_long((v = hfind(it, "n")) ? v : &EG(uninitialized_zval)));
                add_assoc_long(&r, "modifiedCount", zval_get_long((v = hfind(it, "nModified")) ? v : &EG(uninitialized_zval)));
                if ((v = hfind(hfind(it, "upserted"), "_id"))) { Z_TRY_ADDREF_P(v); add_assoc_zval(&r, "upsertedId", v); }
                bwc_set_result(upd, global, &r);
            } else {
                add_assoc_long(&r, "deletedCount", zval_get_long((v = hfind(it, "n")) ? v : &EG(uninitialized_zval)));
                bwc_set_result(del, global, &r);
            }
        }
    } ZEND_HASH_FOREACH_END();
}

static void results_doc(zval *map, zval *out)
{
    /* keys are operation indexes; encode as a Document with string keys */
    zval arr;
    zend_ulong k;
    zval *v;
    smart_str s = {0};
    swm_error err;
    swm_error_init(&err);
    array_init(&arr);
    ZEND_HASH_FOREACH_NUM_KEY_VAL(Z_ARRVAL_P(map), k, v) {
        char buf[24];
        int l = snprintf(buf, sizeof(buf), ZEND_ULONG_FMT, k);
        Z_TRY_ADDREF_P(v);
        zend_hash_str_update(Z_ARRVAL(arr), buf, l, v);
    } ZEND_HASH_FOREACH_END();
    if (zend_hash_num_elements(Z_ARRVAL(arr)) == 0) {
        zval_ptr_dtor(&arr);
        object_init(&arr);
    }
    if (swm_bson_encode(&s, &arr, &err) == SUCCESS) swm_new_document_raw(out, swm_ce_Document, ZSTR_VAL(s.s), ZSTR_LEN(s.s));
    else ZVAL_NULL(out);
    smart_str_free(&s);
    swm_error_clear(&err);
    zval_ptr_dtor(&arr);
}

static void exec_bulk_command(swm_client *c, zend_string *prefer, zval *bulk, zval *options, zval *return_value)
{
    zend_object *bo = Z_OBJ_P(bulk);
    zval *ops = PROP(bo, 0), *bopts = PROP(bo, 1), *sess = NULL, *wc = NULL, wc_arr, *v;
    zend_bool ordered = 1, verbose = 0, acknowledged;
    zend_long counts[5] = {0, 0, 0, 0, 0};
    zval ins, upd, del, errors, wces;
    swm_error err;
    uint32_t nops, i = 0;
    zend_long max_batch = swm_client_max_batch(c);
    size_t max_bytes = (size_t) 47 * 1024 * 1024;
    zend_string *admin = ZSTR_INIT_LITERAL("admin", 0);
    int64_t timeout = 0;

    swm_error_init(&err);
    ZVAL_UNDEF(&wc_arr);
    if (Z_TYPE_P(PROP(bo, 2)) == IS_TRUE) { swm_throw(swm_ce_InvalidArgumentException, "BulkWriteCommand objects may only be executed once"); goto done0; }
    if (Z_TYPE_P(ops) != IS_ARRAY || (nops = zend_hash_num_elements(Z_ARRVAL_P(ops))) == 0) { swm_throw(swm_ce_InvalidArgumentException, "Cannot do an empty bulk write"); goto done0; }
    if (opt_object(options, "session", swm_ce_Session, &sess, &err) == FAILURE || opt_timeout(options, &timeout, &err) == FAILURE
        || opt_object(options, "writeConcern", swm_ce_WriteConcern, &wc, &err) == FAILURE) { swm_error_throw(&err); goto done0; }
    { zval t; ZVAL_TRUE(&t); prop_set(bo, 2, &t); }
    if ((v = hfind(bopts, "ordered"))) ordered = zend_is_true(v);
    if ((v = hfind(bopts, "verboseResults"))) verbose = zend_is_true(v);
    if (wc) wc_to_array(wc, &wc_arr);
    else { swm_client_get_option(c, "writeConcern", &wc_arr); if (Z_TYPE(wc_arr) != IS_ARRAY) { zval_ptr_dtor(&wc_arr); ZVAL_UNDEF(&wc_arr); } }
    acknowledged = !(Z_TYPE(wc_arr) == IS_ARRAY && (v = hfind(&wc_arr, "w")) && Z_TYPE_P(v) == IS_LONG && Z_LVAL_P(v) == 0);
    array_init(&ins); array_init(&upd); array_init(&del); array_init(&errors); array_init(&wces);

    while (i < nops) {
        zval cmd, seqs, opsq, nsq, nsmap, idx_map, reply, *op;
        zend_string *raw = NULL, *server = NULL;
        size_t bytes = 0;
        zend_bool multi = 0;
        swm_exec_opts o;
        int rc;
        array_init(&opsq); array_init(&nsq); array_init(&nsmap); array_init(&idx_map);
        for (; i < nops; i++) {
            zval *nsv, *doc, opdoc, nsidx, *found;
            smart_str enc = {0};
            zend_string *k;
            zval *dv;
            int kind;
            op = zend_hash_index_find(Z_ARRVAL_P(ops), i);
            kind = (int) Z_LVAL_P(zend_hash_index_find(Z_ARRVAL_P(op), 0));
            nsv = zend_hash_index_find(Z_ARRVAL_P(op), 1);
            doc = zend_hash_index_find(Z_ARRVAL_P(op), 2);
            if (!(found = zend_hash_find(Z_ARRVAL(nsmap), Z_STR_P(nsv)))) {
                zval nsd;
                ZVAL_LONG(&nsidx, zend_hash_num_elements(Z_ARRVAL(nsmap)));
                found = zend_hash_update(Z_ARRVAL(nsmap), Z_STR_P(nsv), &nsidx);
                array_init(&nsd);
                add_assoc_str(&nsd, "ns", zend_string_copy(Z_STR_P(nsv)));
                add_next_index_zval(&nsq, &nsd);
            }
            array_init(&opdoc);
            add_assoc_long(&opdoc, kind == BWC_INSERT ? "insert" : kind == BWC_UPDATE ? "update" : "delete", Z_LVAL_P(found));
            ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(doc), k, dv) { Z_TRY_ADDREF_P(dv); zend_hash_update(Z_ARRVAL(opdoc), k, dv); } ZEND_HASH_FOREACH_END();
            swm_bson_encode(&enc, &opdoc, &err);
            zval_ptr_dtor(&opdoc);
            smart_str_0(&enc);
            if (zend_hash_num_elements(Z_ARRVAL(opsq)) && ((zend_long) zend_hash_num_elements(Z_ARRVAL(opsq)) >= max_batch || bytes + ZSTR_LEN(enc.s) > max_bytes)) {
                smart_str_free(&enc);
                break;
            }
            bytes += ZSTR_LEN(enc.s);
            add_next_index_str(&opsq, enc.s);
            add_next_index_long(&idx_map, (zend_long) i);
            multi = multi || zend_is_true(zend_hash_index_find(Z_ARRVAL_P(op), 4));
        }
        array_init(&cmd);
        add_assoc_long(&cmd, "bulkWrite", 1);
        add_assoc_bool(&cmd, "errorsOnly", !verbose);
        add_assoc_bool(&cmd, "ordered", ordered);
        if ((v = hfind(bopts, "bypassDocumentValidation")) && Z_TYPE_P(v) != IS_NULL) add_assoc_bool(&cmd, "bypassDocumentValidation", zend_is_true(v));
        if ((v = hfind(bopts, "comment")) && Z_TYPE_P(v) != IS_NULL) { Z_TRY_ADDREF_P(v); add_assoc_zval(&cmd, "comment", v); }
        if ((v = hfind(bopts, "let")) && Z_TYPE_P(v) != IS_NULL) { zval d; as_document(v, &d, 0, &err); add_assoc_zval(&cmd, "let", &d); }
        if (Z_TYPE(wc_arr) == IS_ARRAY) { Z_ADDREF(wc_arr); add_assoc_zval(&cmd, "writeConcern", &wc_arr); }
        array_init(&seqs);
        add_assoc_zval(&seqs, "ops", &opsq);
        add_assoc_zval(&seqs, "nsInfo", &nsq);
        memset(&o, 0, sizeof(o));
        o.sequences = &seqs;
        o.write = 1;
        o.retryable = !multi;
        o.session = sess;
        o.prefer = prefer;
        o.timeout_us = timeout;
        rc = swm_execute(c, admin, &cmd, &o, &reply, &raw, &server, &err);
        zval_ptr_dtor(&cmd);
        zval_ptr_dtor(&seqs);
        zval_ptr_dtor(&nsmap);
        if (rc == FAILURE) {
            zval_ptr_dtor(&idx_map);
            if (EG(exception)) swm_error_clear(&err); else swm_error_throw(&err);
            goto done;
        }
        counts[0] += zval_get_long((v = hfind(&reply, "nInserted")) ? v : &EG(uninitialized_zval));
        counts[1] += zval_get_long((v = hfind(&reply, "nMatched")) ? v : &EG(uninitialized_zval));
        counts[2] += zval_get_long((v = hfind(&reply, "nModified")) ? v : &EG(uninitialized_zval));
        counts[3] += zval_get_long((v = hfind(&reply, "nUpserted")) ? v : &EG(uninitialized_zval));
        counts[4] += zval_get_long((v = hfind(&reply, "nDeleted")) ? v : &EG(uninitialized_zval));
        if ((v = hfind(&reply, "writeConcernError"))) { zval e; new_wc_error(&e, v); add_next_index_zval(&wces, &e); }
        if (acknowledged) {
            zval *cur = hfind(&reply, "cursor");
            zend_bool ok_cursor;
            int64_t cid;
            bwc_items(hfind(cur, "firstBatch"), &idx_map, ops, &ins, &upd, &del, &errors);
            cid = swm_zval_to_int64(hfind(cur, "id") ? hfind(cur, "id") : &EG(uninitialized_zval), &ok_cursor);
            while (cid != 0) {
                zval gm, idz, gr;
                swm_exec_opts go;
                memset(&go, 0, sizeof(go));
                array_init(&gm);
                swm_new_int64(&idz, cid);
                add_assoc_zval(&gm, "getMore", &idz);
                add_assoc_string(&gm, "collection", "$cmd.bulkWrite");
                go.server = server;
                go.session = sess;
                if (swm_execute(c, admin, &gm, &go, &gr, NULL, NULL, &err) == FAILURE) {
                    zval_ptr_dtor(&gm);
                    swm_error_clear(&err);
                    break;
                }
                zval_ptr_dtor(&gm);
                bwc_items(hfind(hfind(&gr, "cursor"), "nextBatch"), &idx_map, ops, &ins, &upd, &del, &errors);
                cid = swm_zval_to_int64(hfind(hfind(&gr, "cursor"), "id") ? hfind(hfind(&gr, "cursor"), "id") : &EG(uninitialized_zval), &ok_cursor);
                zval_ptr_dtor(&gr);
            }
        }
        if (raw) zend_string_release(raw);
        if (server) zend_string_release(server);
        zval_ptr_dtor(&reply);
        zval_ptr_dtor(&idx_map);
        if (ordered && zend_hash_num_elements(Z_ARRVAL(errors))) break;
    }

    object_init_ex(return_value, swm_ce_BulkWriteCommandResult);
    for (int k = 0; k < 5; k++) prop_set_long(Z_OBJ_P(return_value), k, counts[k]);
    if (verbose && acknowledged) {
        zval d;
        results_doc(&ins, &d); prop_set(Z_OBJ_P(return_value), 5, &d); zval_ptr_dtor(&d);
        results_doc(&upd, &d); prop_set(Z_OBJ_P(return_value), 6, &d); zval_ptr_dtor(&d);
        results_doc(&del, &d); prop_set(Z_OBJ_P(return_value), 7, &d); zval_ptr_dtor(&d);
    }
    { zval b; ZVAL_BOOL(&b, acknowledged); prop_set(Z_OBJ_P(return_value), 8, &b); }
    if (zend_hash_num_elements(Z_ARRVAL(errors)) || zend_hash_num_elements(Z_ARRVAL(wces))) {
        zval ex, *first;
        zend_string *msg;
        zend_long code = 0;
        first = zend_hash_get_current_data(Z_ARRVAL(errors));
        if (!first) first = zend_hash_index_find(Z_ARRVAL(wces), 0);
        if (first && Z_OBJCE_P(first) == swm_ce_WriteError) { msg = zend_string_copy(Z_STR_P(PROP(Z_OBJ_P(first), 2))); code = Z_LVAL_P(PROP(Z_OBJ_P(first), 0)); }
        else if (first) { msg = zend_string_copy(Z_STR_P(PROP(Z_OBJ_P(first), 1))); code = Z_LVAL_P(PROP(Z_OBJ_P(first), 0)); }
        else msg = zend_string_init(ZEND_STRL("Bulk write failed"), 0);
        object_init_ex(&ex, swm_ce_BulkWriteCommandException);
        zend_update_property_str(zend_ce_exception, Z_OBJ(ex), ZEND_STRL("message"), msg);
        zend_update_property_long(zend_ce_exception, Z_OBJ(ex), ZEND_STRL("code"), code);
        zend_update_property(swm_ce_BulkWriteCommandException, Z_OBJ(ex), ZEND_STRL("partialResult"), return_value);
        zend_update_property(swm_ce_BulkWriteCommandException, Z_OBJ(ex), ZEND_STRL("writeErrors"), &errors);
        zend_update_property(swm_ce_BulkWriteCommandException, Z_OBJ(ex), ZEND_STRL("writeConcernErrors"), &wces);
        zend_string_release(msg);
        zval_ptr_dtor(return_value);
        ZVAL_UNDEF(return_value);
        zend_throw_exception_object(&ex);
    }
done:
    zval_ptr_dtor(&ins); zval_ptr_dtor(&upd); zval_ptr_dtor(&del); zval_ptr_dtor(&errors); zval_ptr_dtor(&wces);
done0:
    zval_ptr_dtor(&wc_arr);
    zend_string_release(admin);
}

/* ======================================================= Manager methods */
#define THIS_MANAGER() swm_manager_from(THIS_OBJ)
#define ENSURE_MANAGER(m) do { if (!(m)->client) { swm_throw(swm_ce_LogicException, "Manager is not initialized"); RETURN_THROWS(); } } while (0)

PHP_METHOD(SwooleMongo_Driver_Manager, __construct)
{
    zend_string *uri = NULL;
    zval *uri_opts = NULL, *driver_opts = NULL;
    swm_manager_obj *m = THIS_MANAGER();
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(0, 3)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR_OR_NULL(uri)
        Z_PARAM_ARRAY_OR_NULL(uri_opts)
        Z_PARAM_ARRAY_OR_NULL(driver_opts)
    ZEND_PARSE_PARAMETERS_END();
    if (m->client) {
        swm_throw(swm_ce_LogicException, "Manager is already initialized");
        RETURN_THROWS();
    }
    swm_error_init(&err);
    if (!uri) uri = ZSTR_INIT_LITERAL("mongodb://127.0.0.1/", 0); else zend_string_addref(uri);
    m->client = swm_client_create(uri, uri_opts, driver_opts, &err);
    zend_string_release(uri);
    if (!m->client) THROW_ERR(err);
}

#define EXEC_PARAMS_DB \
    zend_string *db; zval *cmd, *options = NULL; \
    ZEND_PARSE_PARAMETERS_START(2, 3) \
        Z_PARAM_STR(db) \
        Z_PARAM_OBJECT_OF_CLASS(cmd, swm_ce_Command) \
        Z_PARAM_OPTIONAL \
        Z_PARAM_ARRAY_OR_NULL(options) \
    ZEND_PARSE_PARAMETERS_END();

PHP_METHOD(SwooleMongo_Driver_Manager, executeCommand)
{
    swm_manager_obj *m = THIS_MANAGER();
    EXEC_PARAMS_DB
    ENSURE_MANAGER(m);
    exec_command(m->client, NULL, db, cmd, options, EXEC_GENERIC, return_value);
}
PHP_METHOD(SwooleMongo_Driver_Manager, executeReadCommand)
{
    swm_manager_obj *m = THIS_MANAGER();
    EXEC_PARAMS_DB
    ENSURE_MANAGER(m);
    exec_command(m->client, NULL, db, cmd, options, EXEC_READ, return_value);
}
PHP_METHOD(SwooleMongo_Driver_Manager, executeWriteCommand)
{
    swm_manager_obj *m = THIS_MANAGER();
    EXEC_PARAMS_DB
    ENSURE_MANAGER(m);
    exec_command(m->client, NULL, db, cmd, options, EXEC_WRITE, return_value);
}
PHP_METHOD(SwooleMongo_Driver_Manager, executeReadWriteCommand)
{
    swm_manager_obj *m = THIS_MANAGER();
    EXEC_PARAMS_DB
    ENSURE_MANAGER(m);
    exec_command(m->client, NULL, db, cmd, options, EXEC_READWRITE, return_value);
}
PHP_METHOD(SwooleMongo_Driver_Manager, executeQuery)
{
    swm_manager_obj *m = THIS_MANAGER();
    zend_string *ns;
    zval *q, *options = NULL;
    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STR(ns)
        Z_PARAM_OBJECT_OF_CLASS(q, swm_ce_Query)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_MANAGER(m);
    exec_query(m->client, NULL, ns, q, options, return_value);
}
PHP_METHOD(SwooleMongo_Driver_Manager, executeBulkWrite)
{
    swm_manager_obj *m = THIS_MANAGER();
    zend_string *ns;
    zval *b, *options = NULL;
    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STR(ns)
        Z_PARAM_OBJECT_OF_CLASS(b, swm_ce_BulkWrite)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_MANAGER(m);
    exec_bulk(m->client, NULL, ns, b, options, return_value);
}

PHP_METHOD(SwooleMongo_Driver_Manager, executeBulkWriteCommand)
{
    swm_manager_obj *m = THIS_MANAGER();
    zval *b, *options = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_OBJECT_OF_CLASS(b, swm_ce_BulkWriteCommand)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_MANAGER(m);
    exec_bulk_command(m->client, NULL, b, options, return_value);
}

PHP_METHOD(SwooleMongo_Driver_Manager, selectServer)
{
    zval *rp = NULL;
    swm_manager_obj *m = THIS_MANAGER();
    swm_error err;
    zend_string *addr;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_OBJECT_OF_CLASS_OR_NULL(rp, swm_ce_ReadPreference)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_MANAGER(m);
    swm_error_init(&err);
    if (!rp) {
        zval def;
        swm_client_get_option(m->client, "readPreference", &def);
        addr = swm_client_select(m->client, &def, 0, &err);
        zval_ptr_dtor(&def);
    } else {
        addr = swm_client_select(m->client, rp, 0, &err);
    }
    if (!addr) THROW_ERR(err);
    swm_new_server(return_value, m->client, addr);
    zend_string_release(addr);
}

PHP_METHOD(SwooleMongo_Driver_Manager, getServers)
{
    swm_manager_obj *m = THIS_MANAGER();
    zend_string **addrs;
    int n;
    ZEND_PARSE_PARAMETERS_NONE();
    ENSURE_MANAGER(m);
    addrs = swm_client_addresses(m->client, &n);
    array_init(return_value);
    for (int i = 0; i < n; i++) {
        zval info, *type, s;
        if (swm_client_server_info(m->client, addrs[i], &info) == SUCCESS) {
            type = hfind(&info, "type");
            if (type && Z_LVAL_P(type) != 0) {
                swm_new_server(&s, m->client, addrs[i]);
                add_next_index_zval(return_value, &s);
            }
            zval_ptr_dtor(&info);
        }
        zend_string_release(addrs[i]);
    }
    efree(addrs);
}

PHP_METHOD(SwooleMongo_Driver_Manager, getReadPreference)
{
    swm_manager_obj *m = THIS_MANAGER();
    zval arr;
    ZEND_PARSE_PARAMETERS_NONE();
    ENSURE_MANAGER(m);
    swm_client_get_option(m->client, "readPreference", &arr);
    new_read_preference(return_value, &arr);
    zval_ptr_dtor(&arr);
}
PHP_METHOD(SwooleMongo_Driver_Manager, getReadConcern)
{
    swm_manager_obj *m = THIS_MANAGER();
    zval arr;
    ZEND_PARSE_PARAMETERS_NONE();
    ENSURE_MANAGER(m);
    swm_client_get_option(m->client, "readConcern", &arr);
    new_read_concern(return_value, &arr);
    zval_ptr_dtor(&arr);
}
PHP_METHOD(SwooleMongo_Driver_Manager, getWriteConcern)
{
    swm_manager_obj *m = THIS_MANAGER();
    zval arr;
    ZEND_PARSE_PARAMETERS_NONE();
    ENSURE_MANAGER(m);
    swm_client_get_option(m->client, "writeConcern", &arr);
    new_write_concern(return_value, &arr);
    zval_ptr_dtor(&arr);
}

PHP_METHOD(SwooleMongo_Driver_Manager, getEncryptedFieldsMap)
{
    swm_manager_obj *m = THIS_MANAGER();
    zval *map;
    ZEND_PARSE_PARAMETERS_NONE();
    ENSURE_MANAGER(m);
    map = swm_client_encrypted_fields_map(m->client);
    if (map) RETURN_COPY(map);
    RETURN_NULL();
}

PHP_METHOD(SwooleMongo_Driver_Manager, addSubscriber)
{
    zval *sub;
    swm_manager_obj *m = THIS_MANAGER();
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(sub, swm_ce_Subscriber)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_MANAGER(m);
    swm_client_add_subscriber(m->client, sub);
}

PHP_METHOD(SwooleMongo_Driver_Manager, removeSubscriber)
{
    zval *sub;
    swm_manager_obj *m = THIS_MANAGER();
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(sub, swm_ce_Subscriber)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_MANAGER(m);
    swm_client_remove_subscriber(m->client, sub);
}

PHP_METHOD(SwooleMongo_Driver_Manager, createClientEncryption)
{
    zval *o;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(o)
    ZEND_PARSE_PARAMETERS_END();
#ifdef SWM_HAVE_MONGOCRYPT
    {
        swm_manager_obj *m = THIS_MANAGER();
        zval *kv = zend_hash_str_find_deref(Z_ARRVAL_P(o), ZEND_STRL("keyVaultClient"));
        swm_client *kvc;
        swm_error err;
        ENSURE_MANAGER(m);
        swm_error_init(&err);
        if (kv && Z_TYPE_P(kv) != IS_NULL && (Z_TYPE_P(kv) != IS_OBJECT || Z_OBJCE_P(kv) != swm_ce_Manager)) {
            swm_throw(swm_ce_InvalidArgumentException, "Expected \"keyVaultClient\" option to be SwooleMongo\\Driver\\Manager");
            RETURN_THROWS();
        }
        kvc = kv && Z_TYPE_P(kv) == IS_OBJECT ? swm_manager_from(Z_OBJ_P(kv))->client : m->client;
        object_init_ex(return_value, swm_ce_ClientEncryption);
        swm_client_encryption_init(return_value, kvc, o, &err);
        if (err.kind != SWM_ERR_NONE) {
            zval_ptr_dtor(return_value);
            ZVAL_UNDEF(return_value);
            swm_error_throw(&err);
            RETURN_THROWS();
        }
    }
#else
    swm_throw(swm_ce_RuntimeException, "Client-side field level encryption was not compiled in; rebuild swmongo with --with-swmongo-mongocrypt");
#endif
}

/* @internal entry point for background coroutines (topology monitoring) */
PHP_METHOD(SwooleMongo_Driver_Manager, __swmTask)
{
    zend_long id;
    zend_string *address;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(id)
        Z_PARAM_STR(address)
    ZEND_PARSE_PARAMETERS_END();
    swm_client_task(id, address);
}

PHP_METHOD(SwooleMongo_Driver_Manager, close)
{
    swm_manager_obj *m = THIS_MANAGER();
    ZEND_PARSE_PARAMETERS_NONE();
    if (m->client) swm_client_close(m->client);
}

/* ======================================================== Server methods */
#define THIS_SERVER() swm_server_from(THIS_OBJ)
#define ENSURE_SERVER(s) do { if (!(s)->client) { swm_throw(swm_ce_LogicException, "Server is not initialized"); RETURN_THROWS(); } } while (0)

PHP_METHOD(SwooleMongo_Driver_Server, executeCommand)
{
    swm_server_obj *s = THIS_SERVER();
    EXEC_PARAMS_DB
    ENSURE_SERVER(s);
    exec_command(s->client, s->address, db, cmd, options, EXEC_GENERIC, return_value);
}
PHP_METHOD(SwooleMongo_Driver_Server, executeReadCommand)
{
    swm_server_obj *s = THIS_SERVER();
    EXEC_PARAMS_DB
    ENSURE_SERVER(s);
    exec_command(s->client, s->address, db, cmd, options, EXEC_READ, return_value);
}
PHP_METHOD(SwooleMongo_Driver_Server, executeWriteCommand)
{
    swm_server_obj *s = THIS_SERVER();
    EXEC_PARAMS_DB
    ENSURE_SERVER(s);
    exec_command(s->client, s->address, db, cmd, options, EXEC_WRITE, return_value);
}
PHP_METHOD(SwooleMongo_Driver_Server, executeReadWriteCommand)
{
    swm_server_obj *s = THIS_SERVER();
    EXEC_PARAMS_DB
    ENSURE_SERVER(s);
    exec_command(s->client, s->address, db, cmd, options, EXEC_READWRITE, return_value);
}
PHP_METHOD(SwooleMongo_Driver_Server, executeQuery)
{
    swm_server_obj *s = THIS_SERVER();
    zend_string *ns;
    zval *q, *options = NULL;
    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STR(ns)
        Z_PARAM_OBJECT_OF_CLASS(q, swm_ce_Query)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_SERVER(s);
    exec_query(s->client, s->address, ns, q, options, return_value);
}
PHP_METHOD(SwooleMongo_Driver_Server, executeBulkWriteCommand)
{
    swm_server_obj *s = THIS_SERVER();
    zval *b, *options = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_OBJECT_OF_CLASS(b, swm_ce_BulkWriteCommand)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_SERVER(s);
    exec_bulk_command(s->client, s->address, b, options, return_value);
}

PHP_METHOD(SwooleMongo_Driver_Server, executeBulkWrite)
{
    swm_server_obj *s = THIS_SERVER();
    zend_string *ns;
    zval *b, *options = NULL;
    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STR(ns)
        Z_PARAM_OBJECT_OF_CLASS(b, swm_ce_BulkWrite)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_SERVER(s);
    exec_bulk(s->client, s->address, ns, b, options, return_value);
}

static int server_info(swm_server_obj *s, zval *info)
{
    if (!s->client || swm_client_server_info(s->client, s->address, info) == FAILURE) {
        swm_throw(swm_ce_RuntimeException, "Failed to get server description");
        return FAILURE;
    }
    return SUCCESS;
}

#define SERVER_INFO_METHOD(name, body) \
PHP_METHOD(SwooleMongo_Driver_Server, name) \
{ \
    zval info, *v; \
    ZEND_PARSE_PARAMETERS_NONE(); \
    if (server_info(THIS_SERVER(), &info) == FAILURE) RETURN_THROWS(); \
    body; \
    zval_ptr_dtor(&info); \
}

SERVER_INFO_METHOD(getHost, v = hfind(&info, "host"); RETVAL_COPY(v))
SERVER_INFO_METHOD(getPort, v = hfind(&info, "port"); RETVAL_COPY(v))
SERVER_INFO_METHOD(getType, v = hfind(&info, "type"); RETVAL_COPY(v))
SERVER_INFO_METHOD(getTags, v = hfind(&info, "tags"); RETVAL_COPY(v))
SERVER_INFO_METHOD(getLatency, v = hfind(&info, "latency"); if (Z_LVAL_P(v) < 0) RETVAL_NULL(); else RETVAL_COPY(v))
SERVER_INFO_METHOD(getInfo, v = hfind(&info, "hello"); if (v && Z_TYPE_P(v) == IS_ARRAY) RETVAL_COPY(v); else array_init(return_value))
SERVER_INFO_METHOD(isPrimary, v = hfind(&info, "type"); RETVAL_BOOL(Z_LVAL_P(v) == 4))
SERVER_INFO_METHOD(isSecondary, v = hfind(&info, "type"); RETVAL_BOOL(Z_LVAL_P(v) == 5))
SERVER_INFO_METHOD(isArbiter, v = hfind(&info, "type"); RETVAL_BOOL(Z_LVAL_P(v) == 6))
SERVER_INFO_METHOD(isHidden, v = hfind(hfind(&info, "hello"), "hidden"); RETVAL_BOOL(v && zend_is_true(v)))
SERVER_INFO_METHOD(isPassive, v = hfind(hfind(&info, "hello"), "passive"); RETVAL_BOOL(v && zend_is_true(v)))

/* ================================================================ Session */
static zend_object *session_create(zend_class_entry *ce)
{
    swm_session_obj *s = zend_object_alloc(sizeof(swm_session_obj), ce);
    zend_object_std_init(&s->std, ce);
    object_properties_init(&s->std, ce);
    s->std.handlers = &session_handlers;
    ZVAL_UNDEF(&s->default_txn_opts);
    ZVAL_UNDEF(&s->txn_opts);
    ZVAL_UNDEF(&s->txn_opts_user);
    ZVAL_UNDEF(&s->operation_time);
    ZVAL_UNDEF(&s->cluster_time);
    ZVAL_UNDEF(&s->snapshot_time);
    s->ended = 1;
    return &s->std;
}

static void session_free(zend_object *o)
{
    swm_session_obj *s = swm_session_from(o);
    if (s->client) {
        if (s->pinned_conn) { swm_conn_unpin(s->client, s->pinned_conn, 1); s->pinned_conn = NULL; }
        if (!s->ended) swm_session_pool_release(s->client, &s->ss);
        swm_client_release(s->client);
    }
    zval_ptr_dtor(&s->default_txn_opts);
    zval_ptr_dtor(&s->txn_opts);
    zval_ptr_dtor(&s->txn_opts_user);
    zval_ptr_dtor(&s->operation_time);
    zval_ptr_dtor(&s->cluster_time);
    zval_ptr_dtor(&s->snapshot_time);
    if (s->pinned) zend_string_release(s->pinned);
    zend_object_std_dtor(o);
}

/* Converts user transaction options (objects) into the internal array form */
static int normalize_txn_opts(zval *src, zval *dst, swm_error *err)
{
    zval *v;
    if (!src || Z_TYPE_P(src) != IS_ARRAY) return SUCCESS;
    if (opt_object(src, "readConcern", swm_ce_ReadConcern, &v, err) == FAILURE) return FAILURE;
    if (v) { zval a; rc_to_array(v, &a); if (Z_TYPE(a) == IS_ARRAY) zend_hash_str_update(Z_ARRVAL_P(dst), ZEND_STRL("readConcern"), &a); }
    if (opt_object(src, "writeConcern", swm_ce_WriteConcern, &v, err) == FAILURE) return FAILURE;
    if (v) {
        zval a, *w;
        wc_to_array(v, &a);
        if (Z_TYPE(a) == IS_ARRAY && (w = hfind(&a, "w")) && Z_TYPE_P(w) == IS_LONG && Z_LVAL_P(w) == 0) {
            zval_ptr_dtor(&a);
            swm_error_set(err, SWM_ERR_ARG, 0, "Transactions do not support unacknowledged write concern");
            return FAILURE;
        }
        if (Z_TYPE(a) == IS_ARRAY) zend_hash_str_update(Z_ARRVAL_P(dst), ZEND_STRL("writeConcern"), &a);
    }
    if (opt_object(src, "readPreference", swm_ce_ReadPreference, &v, err) == FAILURE) return FAILURE;
    if (v) {
        if (strcmp(Z_STRVAL_P(PROP(Z_OBJ_P(v), 0)), "primary") != 0) {
            swm_error_set(err, SWM_ERR_ARG, 0, "Read preference in a transaction must be primary");
            return FAILURE;
        }
        Z_ADDREF_P(v);
        zend_hash_str_update(Z_ARRVAL_P(dst), ZEND_STRL("readPreference"), v);
    }
    if ((v = hfind(src, "maxCommitTimeMS")) && Z_TYPE_P(v) != IS_NULL) {
        zval l;
        if (zval_get_long(v) < 0) { swm_error_set(err, SWM_ERR_ARG, 0, "Expected \"maxCommitTimeMS\" to be >= 0"); return FAILURE; }
        ZVAL_LONG(&l, zval_get_long(v));
        zend_hash_str_update(Z_ARRVAL_P(dst), ZEND_STRL("maxCommitTimeMS"), &l);
    }
    return SUCCESS;
}

PHP_METHOD(SwooleMongo_Driver_Manager, startSession)
{
    zval *options = NULL, *v;
    swm_manager_obj *m = THIS_MANAGER();
    swm_session_obj *s;
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    ENSURE_MANAGER(m);
    swm_error_init(&err);
    if ((v = hfind(options, "snapshot")) && zend_is_true(v) && (v = hfind(options, "causalConsistency")) && zend_is_true(v)) {
        swm_throw(swm_ce_InvalidArgumentException, "Only one of \"causalConsistency\" and \"snapshot\" can be enabled");
        RETURN_THROWS();
    }
    object_init_ex(return_value, swm_ce_Session);
    s = swm_session_from(Z_OBJ_P(return_value));
    array_init(&s->default_txn_opts);
    array_init(&s->txn_opts);
    if ((v = hfind(options, "defaultTransactionOptions")) && Z_TYPE_P(v) == IS_ARRAY
        && normalize_txn_opts(v, &s->default_txn_opts, &err) == FAILURE) {
        zval_ptr_dtor(return_value);
        ZVAL_UNDEF(return_value);
        THROW_ERR(err);
    }
    s->client = m->client;
    swm_client_addref(m->client);
    swm_session_pool_acquire(m->client, &s->ss);
    s->ended = 0;
    s->causal = 1;
    if ((v = hfind(options, "causalConsistency"))) s->causal = zend_is_true(v);
    if ((v = hfind(options, "snapshot")) && zend_is_true(v)) { s->snapshot = 1; s->causal = 0; }
}

static const char *txn_state_name(swm_txn_state st)
{
    switch (st) {
        case SWM_TXN_STARTING: return "starting";
        case SWM_TXN_IN_PROGRESS: return "in_progress";
        case SWM_TXN_COMMITTED: return "committed";
        case SWM_TXN_ABORTED: return "aborted";
        default: return "none";
    }
}

static int session_start_txn(swm_session_obj *s, zval *options, swm_error *err)
{
    if (s->ended) { swm_error_set(err, SWM_ERR_LOGIC, 0, "Cannot use a session that has ended"); return FAILURE; }
    if (s->state == SWM_TXN_STARTING || s->state == SWM_TXN_IN_PROGRESS) {
        swm_error_set(err, SWM_ERR_LOGIC, 0, "Transaction already in progress");
        return FAILURE;
    }
    if (s->snapshot) {
        swm_error_set(err, SWM_ERR_LOGIC, 0, "Transactions are not supported in snapshot sessions");
        return FAILURE;
    }
    zval_ptr_dtor(&s->txn_opts);
    ZVAL_ARR(&s->txn_opts, zend_array_dup(Z_ARRVAL(s->default_txn_opts)));
    if (normalize_txn_opts(options, &s->txn_opts, err) == FAILURE) return FAILURE;
    zval_ptr_dtor(&s->txn_opts_user);
    if (options) ZVAL_COPY(&s->txn_opts_user, options); else array_init(&s->txn_opts_user);
    if (s->pinned_conn) { swm_conn_unpin(s->client, s->pinned_conn, 0); s->pinned_conn = NULL; }
    s->ss.txn_number++;
    s->state = SWM_TXN_STARTING;
    s->committed_empty = 0;
    if (s->pinned) { zend_string_release(s->pinned); s->pinned = NULL; }
    return SUCCESS;
}

static int session_finish(swm_session_obj *s, const char *cmd_name, zend_bool retry_commit, swm_error *err)
{
    zval cmd, reply, zs, *wc, *mct;
    swm_exec_opts o;
    zend_string *admin = ZSTR_INIT_LITERAL("admin", 0);
    int rc;
    memset(&o, 0, sizeof(o));
    array_init(&cmd);
    add_assoc_long(&cmd, cmd_name, 1);
    if ((wc = hfind(&s->txn_opts, "writeConcern")) && Z_TYPE_P(wc) == IS_ARRAY) {
        zval w;
        ZVAL_ARR(&w, zend_array_dup(Z_ARRVAL_P(wc)));
        if (retry_commit) {
            add_assoc_string(&w, "w", "majority");
            if (!hfind(&w, "wtimeout")) add_assoc_long(&w, "wtimeout", 10000);
        }
        add_assoc_zval(&cmd, "writeConcern", &w);
    } else if (retry_commit) {
        zval w;
        array_init(&w);
        add_assoc_string(&w, "w", "majority");
        add_assoc_long(&w, "wtimeout", 10000);
        add_assoc_zval(&cmd, "writeConcern", &w);
    }
    if (!strcmp(cmd_name, "commitTransaction") && (mct = hfind(&s->txn_opts, "maxCommitTimeMS")) && Z_TYPE_P(mct) == IS_LONG) {
        add_assoc_long(&cmd, "maxTimeMS", Z_LVAL_P(mct));
    }
    ZVAL_OBJ(&zs, &s->std);
    o.session = &zs;
    o.write = 1;
    o.retryable = 1;
    o.txn_control = 1;
    rc = swm_execute(s->client, admin, &cmd, &o, &reply, NULL, NULL, err);
    zend_string_release(admin);
    zval_ptr_dtor(&cmd);
    if (rc == SUCCESS) {
        zval *wce = hfind(&reply, "writeConcernError");
        if (wce && !strcmp(cmd_name, "commitTransaction")) {
            zval *msg = hfind(wce, "errmsg");
            zend_long code = zval_get_long(hfind(wce, "code") ? hfind(wce, "code") : &EG(uninitialized_zval));
            swm_error_set(err, SWM_ERR_SERVER, code, "%s", msg && Z_TYPE_P(msg) == IS_STRING ? Z_STRVAL_P(msg) : "Write concern error");
            ZVAL_COPY(&err->reply, &reply);
            if (code != 79 && code != 100) swm_error_add_label(err, "UnknownTransactionCommitResult");
            rc = FAILURE;
        }
        zval_ptr_dtor(&reply);
    }
    return rc;
}

static int session_commit(swm_session_obj *s, swm_error *err)
{
    zend_bool retry = 0;
    if (s->ended) { swm_error_set(err, SWM_ERR_LOGIC, 0, "Cannot use a session that has ended"); return FAILURE; }
    switch (s->state) {
        case SWM_TXN_NONE:
            swm_error_set(err, SWM_ERR_LOGIC, 0, "There is no active transaction");
            return FAILURE;
        case SWM_TXN_ABORTED:
            swm_error_set(err, SWM_ERR_LOGIC, 0, "Cannot call commitTransaction after calling abortTransaction");
            return FAILURE;
        case SWM_TXN_STARTING:
            s->state = SWM_TXN_COMMITTED;
            s->committed_empty = 1;
            return SUCCESS;
        case SWM_TXN_COMMITTED:
            if (s->committed_empty) return SUCCESS;
            retry = 1;
            break;
        default:
            break;
    }
    s->state = SWM_TXN_COMMITTED;
    return session_finish(s, "commitTransaction", retry, err);
}

static int session_abort(swm_session_obj *s, swm_error *err)
{
    swm_error ignored;
    if (s->ended) { swm_error_set(err, SWM_ERR_LOGIC, 0, "Cannot use a session that has ended"); return FAILURE; }
    switch (s->state) {
        case SWM_TXN_NONE:
            swm_error_set(err, SWM_ERR_LOGIC, 0, "There is no active transaction");
            return FAILURE;
        case SWM_TXN_COMMITTED:
            swm_error_set(err, SWM_ERR_LOGIC, 0, "Cannot call abortTransaction after calling commitTransaction");
            return FAILURE;
        case SWM_TXN_ABORTED:
            swm_error_set(err, SWM_ERR_LOGIC, 0, "Cannot call abortTransaction twice");
            return FAILURE;
        case SWM_TXN_STARTING:
            s->state = SWM_TXN_ABORTED;
            return SUCCESS;
        default:
            break;
    }
    swm_error_init(&ignored);
    session_finish(s, "abortTransaction", 0, &ignored);
    swm_error_clear(&ignored);
    if (EG(exception)) zend_clear_exception();
    s->state = SWM_TXN_ABORTED;
    if (s->pinned) { zend_string_release(s->pinned); s->pinned = NULL; }
    return SUCCESS;
}

#define THIS_SESSION() swm_session_from(THIS_OBJ)
#define SESSION_CALL(expr) do { swm_error err; swm_error_init(&err); if ((expr) == FAILURE) THROW_ERR(err); } while (0)

PHP_METHOD(SwooleMongo_Driver_Session, startTransaction)
{
    zval *options = NULL;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();
    SESSION_CALL(session_start_txn(THIS_SESSION(), options, &err));
}

PHP_METHOD(SwooleMongo_Driver_Session, commitTransaction)
{
    ZEND_PARSE_PARAMETERS_NONE();
    SESSION_CALL(session_commit(THIS_SESSION(), &err));
}

PHP_METHOD(SwooleMongo_Driver_Session, abortTransaction)
{
    ZEND_PARSE_PARAMETERS_NONE();
    SESSION_CALL(session_abort(THIS_SESSION(), &err));
}

static zend_bool exception_has_label(zend_object *ex, const char *label)
{
    zval rv, *labels, *v;
    if (!instanceof_function(ex->ce, swm_ce_RuntimeException)) return 0;
    labels = zend_read_property(swm_ce_RuntimeException, ex, ZEND_STRL("errorLabels"), 1, &rv);
    if (!labels || Z_TYPE_P(labels) != IS_ARRAY) return 0;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(labels), v) {
        if (Z_TYPE_P(v) == IS_STRING && !strcmp(Z_STRVAL_P(v), label)) return 1;
    } ZEND_HASH_FOREACH_END();
    return 0;
}

/* Session::withTransaction(callable $callback, ?array $transactionOptions = null): mixed
 * Convenient transaction API: retries TransientTransactionError and
 * UnknownTransactionCommitResult for up to 120 seconds. */
PHP_METHOD(SwooleMongo_Driver_Session, withTransaction)
{
    zend_fcall_info fci;
    zend_fcall_info_cache fcc;
    zval *options = NULL, result, arg;
    swm_session_obj *s = THIS_SESSION();
    int64_t start = swm_now_us();
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_FUNC(fci, fcc)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_OR_NULL(options)
    ZEND_PARSE_PARAMETERS_END();

    ZVAL_COPY(&arg, ZEND_THIS);
    for (;;) {
        swm_error err;
        swm_error_init(&err);
        if (session_start_txn(s, options, &err) == FAILURE) {
            zval_ptr_dtor(&arg);
            THROW_ERR(err);
        }
        ZVAL_UNDEF(&result);
        fci.retval = &result;
        fci.params = &arg;
        fci.param_count = 1;
        zend_call_function(&fci, &fcc);
        if (EG(exception)) {
            zend_object *ex = EG(exception);
            zval exz;
            GC_ADDREF(ex);
            zend_clear_exception();
            if (s->state == SWM_TXN_STARTING || s->state == SWM_TXN_IN_PROGRESS) {
                session_abort(s, &err);
                swm_error_clear(&err);
                if (EG(exception)) zend_clear_exception();
            }
            zval_ptr_dtor(&result);
            if (exception_has_label(ex, "TransientTransactionError") && swm_now_us() - start < 120000000) {
                OBJ_RELEASE(ex);
                continue;
            }
            ZVAL_OBJ(&exz, ex);
            zend_throw_exception_object(&exz);
            zval_ptr_dtor(&arg);
            RETURN_THROWS();
        }
        if (s->state != SWM_TXN_STARTING && s->state != SWM_TXN_IN_PROGRESS) {
            zval_ptr_dtor(&arg);
            RETURN_COPY_VALUE(&result);
        }
        for (;;) {
            if (session_commit(s, &err) == SUCCESS) {
                zval_ptr_dtor(&arg);
                RETURN_COPY_VALUE(&result);
            }
            if (swm_now_us() - start < 120000000) {
                if (swm_error_has_label(&err, "UnknownTransactionCommitResult") && err.code != 50) {
                    swm_error_clear(&err);
                    continue;
                }
                if (swm_error_has_label(&err, "TransientTransactionError")) {
                    swm_error_clear(&err);
                    break;
                }
            }
            zval_ptr_dtor(&result);
            zval_ptr_dtor(&arg);
            THROW_ERR(err);
        }
        zval_ptr_dtor(&result);
    }
}

PHP_METHOD(SwooleMongo_Driver_Session, endSession)
{
    swm_session_obj *s = THIS_SESSION();
    ZEND_PARSE_PARAMETERS_NONE();
    if (s->ended || !s->client) return;
    if (s->state == SWM_TXN_STARTING || s->state == SWM_TXN_IN_PROGRESS) {
        swm_error err;
        swm_error_init(&err);
        session_abort(s, &err);
        swm_error_clear(&err);
        if (EG(exception)) zend_clear_exception();
    }
    if (s->pinned_conn) { swm_conn_unpin(s->client, s->pinned_conn, 0); s->pinned_conn = NULL; }
    swm_session_pool_release(s->client, &s->ss);
    s->ended = 1;
}

PHP_METHOD(SwooleMongo_Driver_Session, getLogicalSessionId)
{
    swm_session_obj *s = THIS_SESSION();
    zval id;
    ZEND_PARSE_PARAMETERS_NONE();
    object_init(return_value);
    swm_new_binary(&id, (char *) s->ss.id, 16, 4);
    zend_update_property(zend_standard_class_def, Z_OBJ_P(return_value), ZEND_STRL("id"), &id);
    zval_ptr_dtor(&id);
}

PHP_METHOD(SwooleMongo_Driver_Session, getTransactionState)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STRING(txn_state_name(THIS_SESSION()->state));
}

PHP_METHOD(SwooleMongo_Driver_Session, getTransactionOptions)
{
    swm_session_obj *s = THIS_SESSION();
    ZEND_PARSE_PARAMETERS_NONE();
    if ((s->state == SWM_TXN_STARTING || s->state == SWM_TXN_IN_PROGRESS) && Z_TYPE(s->txn_opts_user) == IS_ARRAY) RETURN_COPY(&s->txn_opts_user);
    RETURN_NULL();
}

PHP_METHOD(SwooleMongo_Driver_Session, isInTransaction)
{
    swm_session_obj *s = THIS_SESSION();
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(s->state == SWM_TXN_STARTING || s->state == SWM_TXN_IN_PROGRESS);
}

PHP_METHOD(SwooleMongo_Driver_Session, isDirty)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(THIS_SESSION()->ss.dirty);
}

PHP_METHOD(SwooleMongo_Driver_Session, getOperationTime)
{
    swm_session_obj *s = THIS_SESSION();
    ZEND_PARSE_PARAMETERS_NONE();
    if (Z_TYPE(s->operation_time) == IS_UNDEF) RETURN_NULL();
    RETURN_COPY(&s->operation_time);
}

PHP_METHOD(SwooleMongo_Driver_Session, getClusterTime)
{
    swm_session_obj *s = THIS_SESSION();
    ZEND_PARSE_PARAMETERS_NONE();
    if (Z_TYPE(s->cluster_time) != IS_ARRAY) RETURN_NULL();
    swm_array_to_object_doc(&s->cluster_time, return_value);
}

PHP_METHOD(SwooleMongo_Driver_Session, getServer)
{
    swm_session_obj *s = THIS_SESSION();
    ZEND_PARSE_PARAMETERS_NONE();
    if (!s->pinned || !s->client) RETURN_NULL();
    swm_new_server(return_value, s->client, s->pinned);
}

PHP_METHOD(SwooleMongo_Driver_Session, advanceOperationTime)
{
    zval *ts;
    swm_session_obj *s = THIS_SESSION();
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ts, swm_ce_Timestamp)
    ZEND_PARSE_PARAMETERS_END();
    if (Z_TYPE(s->operation_time) == IS_UNDEF || swm_timestamp_compare(ts, &s->operation_time) > 0) {
        zval_ptr_dtor(&s->operation_time);
        ZVAL_COPY(&s->operation_time, ts);
    }
}

PHP_METHOD(SwooleMongo_Driver_Session, advanceClusterTime)
{
    zval *ct, arr, *ts, *cur;
    swm_session_obj *s = THIS_SESSION();
    swm_error err;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(ct)
    ZEND_PARSE_PARAMETERS_END();
    swm_error_init(&err);
    if ((Z_TYPE_P(ct) != IS_ARRAY && Z_TYPE_P(ct) != IS_OBJECT) || doc_to_array(ct, &arr, &err) == FAILURE) {
        swm_error_clear(&err);
        swm_throw(swm_ce_InvalidArgumentException, "Expected clusterTime to be array or object");
        RETURN_THROWS();
    }
    ts = hfind(&arr, "clusterTime");
    if (!ts || Z_TYPE_P(ts) != IS_OBJECT || Z_OBJCE_P(ts) != swm_ce_Timestamp) {
        zval_ptr_dtor(&arr);
        swm_throw(swm_ce_InvalidArgumentException, "clusterTime must contain a \"clusterTime\" Timestamp");
        RETURN_THROWS();
    }
    cur = Z_TYPE(s->cluster_time) == IS_ARRAY ? hfind(&s->cluster_time, "clusterTime") : NULL;
    if (!cur || swm_timestamp_compare(ts, cur) > 0) {
        zval_ptr_dtor(&s->cluster_time);
        ZVAL_COPY(&s->cluster_time, &arr);
    }
    zval_ptr_dtor(&arr);
}

/* ================================================================ arginfo */
SWM_ARGINFO_RET(ai_void, IS_VOID)
SWM_ARGINFO_RET(ai_bool, _IS_BOOL)
SWM_ARGINFO_RET(ai_long, IS_LONG)
SWM_ARGINFO_RET(ai_string, IS_STRING)
SWM_ARGINFO_RET(ai_array, IS_ARRAY)
SWM_ARGINFO_RET(ai_mixed, IS_MIXED)
SWM_ARGINFO_RET(ai_object, IS_OBJECT)
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_long_null, 0, 0, IS_LONG, 1)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_string_null, 0, 0, IS_STRING, 1)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_array_null, 0, 0, IS_ARRAY, 1)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_object_null, 0, 0, IS_OBJECT, 1)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(ai_bool_null, 0, 0, MAY_BE_BOOL | MAY_BE_NULL)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(ai_w, 0, 0, MAY_BE_STRING | MAY_BE_LONG | MAY_BE_NULL)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_server, 0, 0, SwooleMongo\\Driver\\Server, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_server_null, 0, 0, SwooleMongo\\Driver\\Server, 1)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_wce_null, 0, 0, SwooleMongo\\Driver\\WriteConcernError, 1)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_int64, 0, 0, SwooleMongo\\BSON\\Int64, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_ts_null, 0, 0, SwooleMongo\\BSON\\Timestamp, 1)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_exception, 0, 0, Exception, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(ai_bsonserialize, 0, 0, MAY_BE_ARRAY | MAY_BE_OBJECT)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_swm_task, 0, 2, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, address, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(ai_mgr_ctor, 0, 0, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, uri, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, uriOptions, IS_ARRAY, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, driverOptions, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_exec_cmd, 0, 2, SwooleMongo\\Driver\\Cursor, 0)
    ZEND_ARG_TYPE_INFO(0, db, IS_STRING, 0)
    ZEND_ARG_OBJ_INFO(0, command, SwooleMongo\\Driver\\Command, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_exec_query, 0, 2, SwooleMongo\\Driver\\Cursor, 0)
    ZEND_ARG_TYPE_INFO(0, namespace, IS_STRING, 0)
    ZEND_ARG_OBJ_INFO(0, query, SwooleMongo\\Driver\\Query, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_exec_bulk, 0, 2, SwooleMongo\\Driver\\WriteResult, 0)
    ZEND_ARG_TYPE_INFO(0, namespace, IS_STRING, 0)
    ZEND_ARG_OBJ_INFO(0, bulk, SwooleMongo\\Driver\\BulkWrite, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_select, 0, 0, SwooleMongo\\Driver\\Server, 0)
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, readPreference, SwooleMongo\\Driver\\ReadPreference, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_get_rp, 0, 0, SwooleMongo\\Driver\\ReadPreference, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_get_rc, 0, 0, SwooleMongo\\Driver\\ReadConcern, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_get_wc, 0, 0, SwooleMongo\\Driver\\WriteConcern, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_start_session, 0, 0, SwooleMongo\\Driver\\Session, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_subscriber, 0, 1, IS_VOID, 0)
    ZEND_ARG_OBJ_INFO(0, subscriber, SwooleMongo\\Driver\\Monitoring\\Subscriber, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(ai_get_efm, 0, 0, MAY_BE_ARRAY | MAY_BE_OBJECT | MAY_BE_NULL)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_cce, 0, 1, SwooleMongo\\Driver\\ClientEncryption, 0)
    ZEND_ARG_TYPE_INFO(0, options, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_rp_ctor, 0, 0, 1)
    ZEND_ARG_TYPE_MASK(0, mode, MAY_BE_STRING | MAY_BE_LONG, NULL)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, tagSets, IS_ARRAY, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(ai_rc_ctor, 0, 0, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, level, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(ai_wc_ctor, 0, 0, 1)
    ZEND_ARG_TYPE_MASK(0, w, MAY_BE_STRING | MAY_BE_LONG, NULL)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, wtimeout, IS_LONG, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, journal, _IS_BOOL, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(ai_cmd_ctor, 0, 0, 1)
    ZEND_ARG_TYPE_MASK(0, document, MAY_BE_ARRAY | MAY_BE_OBJECT, NULL)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, commandOptions, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(ai_query_ctor, 0, 0, 1)
    ZEND_ARG_TYPE_MASK(0, filter, MAY_BE_ARRAY | MAY_BE_OBJECT, NULL)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, queryOptions, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(ai_bulk_ctor, 0, 0, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_bulk_insert, 0, 1, IS_MIXED, 0)
    ZEND_ARG_TYPE_MASK(0, document, MAY_BE_ARRAY | MAY_BE_OBJECT, NULL)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_bulk_update, 0, 2, IS_VOID, 0)
    ZEND_ARG_TYPE_MASK(0, filter, MAY_BE_ARRAY | MAY_BE_OBJECT, NULL)
    ZEND_ARG_TYPE_MASK(0, newObj, MAY_BE_ARRAY | MAY_BE_OBJECT, NULL)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, updateOptions, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_bulk_delete, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_MASK(0, filter, MAY_BE_ARRAY | MAY_BE_OBJECT, NULL)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, deleteOptions, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_set_tm, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, typemap, IS_ARRAY, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_start_txn, 0, 0, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_with_txn, 0, 1, IS_MIXED, 0)
    ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, transactionOptions, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_adv_op, 0, 1, IS_VOID, 0)
    ZEND_ARG_OBJ_INFO(0, operationTime, SwooleMongo\\BSON\\Timestamp, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_adv_ct, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_MASK(0, clusterTime, MAY_BE_ARRAY | MAY_BE_OBJECT, NULL)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_ev_started, 0, 1, IS_VOID, 0)
    ZEND_ARG_OBJ_INFO(0, event, SwooleMongo\\Driver\\Monitoring\\CommandStartedEvent, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_ev_succeeded, 0, 1, IS_VOID, 0)
    ZEND_ARG_OBJ_INFO(0, event, SwooleMongo\\Driver\\Monitoring\\CommandSucceededEvent, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_ev_failed, 0, 1, IS_VOID, 0)
    ZEND_ARG_OBJ_INFO(0, event, SwooleMongo\\Driver\\Monitoring\\CommandFailedEvent, 0)
ZEND_END_ARG_INFO()

#define ME(cls, name, ai) PHP_ME(SwooleMongo_Driver_##cls, name, ai, ZEND_ACC_PUBLIC)
#define ME_FINAL(cls, name, ai) PHP_ME(SwooleMongo_Driver_##cls, name, ai, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_exec_bwc, 0, 1, SwooleMongo\\Driver\\BulkWriteCommandResult, 0)
    ZEND_ARG_OBJ_INFO(0, bulkWrite, SwooleMongo\\Driver\\BulkWriteCommand, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_bwc_insert, 0, 2, IS_MIXED, 0)
    ZEND_ARG_TYPE_INFO(0, namespace, IS_STRING, 0)
    ZEND_ARG_TYPE_MASK(0, document, MAY_BE_ARRAY | MAY_BE_OBJECT, NULL)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_bwc_update, 0, 3, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, namespace, IS_STRING, 0)
    ZEND_ARG_TYPE_MASK(0, filter, MAY_BE_ARRAY | MAY_BE_OBJECT, NULL)
    ZEND_ARG_TYPE_MASK(0, update, MAY_BE_ARRAY | MAY_BE_OBJECT, NULL)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_bwc_delete, 0, 2, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, namespace, IS_STRING, 0)
    ZEND_ARG_TYPE_MASK(0, filter, MAY_BE_ARRAY | MAY_BE_OBJECT, NULL)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_doc_null, 0, 0, SwooleMongo\\BSON\\Document, 1)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_bwcr_null, 0, 0, SwooleMongo\\Driver\\BulkWriteCommandResult, 1)
ZEND_END_ARG_INFO()
static const zend_function_entry bwc_methods[] = {
    ME(BulkWriteCommand, __construct, ai_bulk_ctor)
    ME(BulkWriteCommand, count, ai_long)
    ME(BulkWriteCommand, insertOne, ai_bwc_insert)
    ME(BulkWriteCommand, updateOne, ai_bwc_update)
    ME(BulkWriteCommand, updateMany, ai_bwc_update)
    ME(BulkWriteCommand, replaceOne, ai_bwc_update)
    ME(BulkWriteCommand, deleteOne, ai_bwc_delete)
    ME(BulkWriteCommand, deleteMany, ai_bwc_delete)
    PHP_FE_END
};
static const zend_function_entry bwcr_methods[] = {
    ME(BulkWriteCommandResult, getInsertedCount, ai_long)
    ME(BulkWriteCommandResult, getMatchedCount, ai_long)
    ME(BulkWriteCommandResult, getModifiedCount, ai_long)
    ME(BulkWriteCommandResult, getUpsertedCount, ai_long)
    ME(BulkWriteCommandResult, getDeletedCount, ai_long)
    ME(BulkWriteCommandResult, getInsertResults, ai_doc_null)
    ME(BulkWriteCommandResult, getUpdateResults, ai_doc_null)
    ME(BulkWriteCommandResult, getDeleteResults, ai_doc_null)
    ME(BulkWriteCommandResult, isAcknowledged, ai_bool)
    PHP_FE_END
};
static const zend_function_entry bwce_methods[] = {
    ME_FINAL(BulkWriteCommandException, getPartialResult, ai_bwcr_null)
    ME_FINAL(BulkWriteCommandException, getWriteErrors, ai_array)
    ME_FINAL(BulkWriteCommandException, getWriteConcernErrors, ai_array)
    ME_FINAL(BulkWriteCommandException, getErrorReply, ai_doc_null)
    PHP_FE_END
};

static const zend_function_entry manager_methods[] = {
    ME(Manager, __construct, ai_mgr_ctor)
    ME(Manager, executeCommand, ai_exec_cmd)
    ME(Manager, executeReadCommand, ai_exec_cmd)
    ME(Manager, executeWriteCommand, ai_exec_cmd)
    ME(Manager, executeReadWriteCommand, ai_exec_cmd)
    ME(Manager, executeQuery, ai_exec_query)
    ME(Manager, executeBulkWrite, ai_exec_bulk)
    ME(Manager, executeBulkWriteCommand, ai_exec_bwc)
    ME(Manager, selectServer, ai_select)
    ME(Manager, getServers, ai_array)
    ME(Manager, getReadPreference, ai_get_rp)
    ME(Manager, getReadConcern, ai_get_rc)
    ME(Manager, getWriteConcern, ai_get_wc)
    ME(Manager, getEncryptedFieldsMap, ai_get_efm)
    ME(Manager, startSession, ai_start_session)
    ME(Manager, addSubscriber, ai_subscriber)
    ME(Manager, removeSubscriber, ai_subscriber)
    ME(Manager, createClientEncryption, ai_cce)
    ME(Manager, close, ai_void)
    PHP_ME(SwooleMongo_Driver_Manager, __swmTask, ai_swm_task, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};
static const zend_function_entry server_methods[] = {
    ME(Server, executeCommand, ai_exec_cmd)
    ME(Server, executeReadCommand, ai_exec_cmd)
    ME(Server, executeWriteCommand, ai_exec_cmd)
    ME(Server, executeReadWriteCommand, ai_exec_cmd)
    ME(Server, executeQuery, ai_exec_query)
    ME(Server, executeBulkWrite, ai_exec_bulk)
    ME(Server, executeBulkWriteCommand, ai_exec_bwc)
    ME(Server, getHost, ai_string)
    ME(Server, getPort, ai_long)
    ME(Server, getType, ai_long)
    ME(Server, getTags, ai_array)
    ME(Server, getLatency, ai_long_null)
    ME(Server, getInfo, ai_array)
    ME(Server, isPrimary, ai_bool)
    ME(Server, isSecondary, ai_bool)
    ME(Server, isArbiter, ai_bool)
    ME(Server, isHidden, ai_bool)
    ME(Server, isPassive, ai_bool)
    PHP_FE_END
};
static const zend_function_entry rp_methods[] = {
    ME(ReadPreference, __construct, ai_rp_ctor)
    ME(ReadPreference, getMode, ai_long)
    ME(ReadPreference, getModeString, ai_string)
    ME(ReadPreference, getTagSets, ai_array)
    ME(ReadPreference, getMaxStalenessSeconds, ai_long)
    ME(ReadPreference, getHedge, ai_object_null)
    ME(ReadPreference, bsonSerialize, ai_bsonserialize)
    PHP_FE_END
};
static const zend_function_entry rc_methods[] = {
    ME(ReadConcern, __construct, ai_rc_ctor)
    ME(ReadConcern, getLevel, ai_string_null)
    ME(ReadConcern, isDefault, ai_bool)
    ME(ReadConcern, bsonSerialize, ai_bsonserialize)
    PHP_FE_END
};
static const zend_function_entry wc_methods[] = {
    ME(WriteConcern, __construct, ai_wc_ctor)
    ME(WriteConcern, getW, ai_w)
    ME(WriteConcern, getWtimeout, ai_long)
    ME(WriteConcern, getJournal, ai_bool_null)
    ME(WriteConcern, isDefault, ai_bool)
    ME(WriteConcern, bsonSerialize, ai_bsonserialize)
    PHP_FE_END
};
ZEND_BEGIN_ARG_INFO_EX(ai_api_ctor, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, version, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, strict, _IS_BOOL, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, deprecationErrors, _IS_BOOL, 1, "null")
ZEND_END_ARG_INFO()
static const zend_function_entry api_methods[] = {
    ME(ServerApi, __construct, ai_api_ctor)
    ME(ServerApi, bsonSerialize, ai_bsonserialize)
    PHP_FE_END
};
static const zend_function_entry command_methods[] = {
    ME(Command, __construct, ai_cmd_ctor)
    PHP_FE_END
};
static const zend_function_entry query_methods[] = {
    ME(Query, __construct, ai_query_ctor)
    PHP_FE_END
};
static const zend_function_entry bulk_methods[] = {
    ME(BulkWrite, __construct, ai_bulk_ctor)
    ME(BulkWrite, insert, ai_bulk_insert)
    ME(BulkWrite, update, ai_bulk_update)
    ME(BulkWrite, delete, ai_bulk_delete)
    ME(BulkWrite, count, ai_long)
    PHP_FE_END
};
static const zend_function_entry wr_methods[] = {
    ME(WriteResult, getInsertedCount, ai_long_null)
    ME(WriteResult, getMatchedCount, ai_long_null)
    ME(WriteResult, getModifiedCount, ai_long_null)
    ME(WriteResult, getDeletedCount, ai_long_null)
    ME(WriteResult, getUpsertedCount, ai_long_null)
    ME(WriteResult, getUpsertedIds, ai_array)
    ME(WriteResult, getWriteErrors, ai_array)
    ME(WriteResult, getWriteConcernError, ai_wce_null)
    ME(WriteResult, isAcknowledged, ai_bool)
    ME(WriteResult, getServer, ai_server)
    PHP_FE_END
};
static const zend_function_entry we_methods[] = {
    ME(WriteError, getCode, ai_long)
    ME(WriteError, getIndex, ai_long)
    ME(WriteError, getMessage, ai_string)
    ME(WriteError, getInfo, ai_object_null)
    PHP_FE_END
};
static const zend_function_entry wce_methods[] = {
    ME(WriteConcernError, getCode, ai_long)
    ME(WriteConcernError, getMessage, ai_string)
    ME(WriteConcernError, getInfo, ai_object_null)
    PHP_FE_END
};
static const zend_function_entry cursor_methods[] = {
    ME(Cursor, setTypeMap, ai_set_tm)
    ME(Cursor, rewind, ai_void)
    ME(Cursor, next, ai_void)
    ME(Cursor, valid, ai_bool)
    ME(Cursor, current, ai_mixed)
    ME(Cursor, key, ai_long_null)
    ME(Cursor, toArray, ai_array)
    ME(Cursor, getId, ai_int64)
    ME(Cursor, getServer, ai_server)
    ME(Cursor, isDead, ai_bool)
    ME(Cursor, bufferedCount, ai_long)
    ME(Cursor, getPostBatchResumeToken, ai_object_null)
    ME(Cursor, kill, ai_void)
    PHP_FE_END
};
static const zend_function_entry cursor_iface_methods[] = {
    ZEND_ABSTRACT_ME_WITH_FLAGS(SwooleMongo_Driver_CursorInterface, getId, ai_int64, ZEND_ACC_PUBLIC | ZEND_ACC_ABSTRACT)
    ZEND_ABSTRACT_ME_WITH_FLAGS(SwooleMongo_Driver_CursorInterface, getServer, ai_server, ZEND_ACC_PUBLIC | ZEND_ACC_ABSTRACT)
    ZEND_ABSTRACT_ME_WITH_FLAGS(SwooleMongo_Driver_CursorInterface, isDead, ai_bool, ZEND_ACC_PUBLIC | ZEND_ACC_ABSTRACT)
    ZEND_ABSTRACT_ME_WITH_FLAGS(SwooleMongo_Driver_CursorInterface, setTypeMap, ai_set_tm, ZEND_ACC_PUBLIC | ZEND_ACC_ABSTRACT)
    ZEND_ABSTRACT_ME_WITH_FLAGS(SwooleMongo_Driver_CursorInterface, toArray, ai_array, ZEND_ACC_PUBLIC | ZEND_ACC_ABSTRACT)
    PHP_FE_END
};
static const zend_function_entry session_methods[] = {
    ME_FINAL(Session, startTransaction, ai_start_txn)
    ME_FINAL(Session, commitTransaction, ai_void)
    ME_FINAL(Session, abortTransaction, ai_void)
    ME_FINAL(Session, withTransaction, ai_with_txn)
    ME_FINAL(Session, endSession, ai_void)
    ME_FINAL(Session, getLogicalSessionId, ai_object)
    ME_FINAL(Session, getTransactionState, ai_string)
    ME_FINAL(Session, getTransactionOptions, ai_array_null)
    ME_FINAL(Session, isInTransaction, ai_bool)
    ME_FINAL(Session, isDirty, ai_bool)
    ME_FINAL(Session, getOperationTime, ai_ts_null)
    ME_FINAL(Session, getClusterTime, ai_object_null)
    ME_FINAL(Session, getServer, ai_server_null)
    ME_FINAL(Session, advanceOperationTime, ai_adv_op)
    ME_FINAL(Session, advanceClusterTime, ai_adv_ct)
    PHP_FE_END
};
#define EV(cls, name, ai) PHP_ME(SwooleMongo_Driver_Monitoring_##cls, name, ai, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
#define EV_ALIAS(name, target, ai) PHP_MALIAS(SwooleMongo_Driver_Monitoring_CommandStartedEvent, name, target, ai, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
static const zend_function_entry started_methods[] = {
    EV(CommandStartedEvent, getCommandName, ai_string)
    EV(CommandStartedEvent, getDatabaseName, ai_string)
    EV(CommandStartedEvent, getOperationId, ai_string)
    EV(CommandStartedEvent, getRequestId, ai_string)
    EV(CommandStartedEvent, getHost, ai_string)
    EV(CommandStartedEvent, getPort, ai_long)
    EV(CommandStartedEvent, getCommand, ai_object)
    EV(CommandStartedEvent, getServiceId, ai_mixed)
    PHP_FE_END
};
static const zend_function_entry succeeded_methods[] = {
    EV_ALIAS(getCommandName, getCommandName, ai_string)
    EV_ALIAS(getDatabaseName, getDatabaseName, ai_string)
    EV_ALIAS(getOperationId, getOperationId, ai_string)
    EV_ALIAS(getRequestId, getRequestId, ai_string)
    EV_ALIAS(getHost, getHost, ai_string)
    EV_ALIAS(getPort, getPort, ai_long)
    EV_ALIAS(getServiceId, getServiceId, ai_mixed)
    EV(CommandSucceededEvent, getDurationMicros, ai_long)
    EV(CommandSucceededEvent, getReply, ai_object)
    PHP_FE_END
};
static const zend_function_entry failed_methods[] = {
    EV_ALIAS(getCommandName, getCommandName, ai_string)
    EV_ALIAS(getDatabaseName, getDatabaseName, ai_string)
    EV_ALIAS(getOperationId, getOperationId, ai_string)
    EV_ALIAS(getRequestId, getRequestId, ai_string)
    EV_ALIAS(getHost, getHost, ai_string)
    EV_ALIAS(getPort, getPort, ai_long)
    EV_ALIAS(getServiceId, getServiceId, ai_mixed)
    PHP_MALIAS(SwooleMongo_Driver_Monitoring_CommandSucceededEvent, getDurationMicros, getDurationMicros, ai_long, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
    PHP_MALIAS(SwooleMongo_Driver_Monitoring_CommandSucceededEvent, getReply, getReply, ai_object, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
    EV(CommandFailedEvent, getError, ai_exception)
    PHP_FE_END
};
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_sd, 0, 0, SwooleMongo\\Driver\\ServerDescription, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_td, 0, 0, SwooleMongo\\Driver\\TopologyDescription, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(ai_oid, 0, 0, SwooleMongo\\BSON\\ObjectId, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_has_readable, 0, 0, _IS_BOOL, 0)
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, readPreference, SwooleMongo\\Driver\\ReadPreference, 1, "null")
ZEND_END_ARG_INFO()
static const zend_function_entry sd_methods[] = {
    ME(ServerDescription, getHost, ai_string)
    ME(ServerDescription, getPort, ai_long)
    ME(ServerDescription, getType, ai_string)
    ME(ServerDescription, getHelloResponse, ai_array)
    ME(ServerDescription, getLastUpdateTime, ai_long)
    ME(ServerDescription, getRoundTripTime, ai_long_null)
    PHP_FE_END
};
static const zend_function_entry td_methods[] = {
    ME(TopologyDescription, getType, ai_string)
    ME(TopologyDescription, getServers, ai_array)
    ME(TopologyDescription, hasReadableServer, ai_has_readable)
    ME(TopologyDescription, hasWritableServer, ai_bool)
    PHP_FE_END
};
#define SDE(name, ai) PHP_MALIAS(SwooleMongo_Driver_Monitoring_ServerChangedEvent, name, name, ai, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
#define HB_AWAIT PHP_MALIAS(SwooleMongo_Driver_Monitoring_ServerHeartbeatStartedEvent, isAwaited, isAwaited, ai_bool, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
static const zend_function_entry ev_server_changed[] = { SDE(getHost, ai_string) SDE(getPort, ai_long) SDE(getTopologyId, ai_oid) SDE(getNewDescription, ai_sd) SDE(getPreviousDescription, ai_sd) PHP_FE_END };
static const zend_function_entry ev_server_oc[] = { SDE(getHost, ai_string) SDE(getPort, ai_long) SDE(getTopologyId, ai_oid) PHP_FE_END };
static const zend_function_entry ev_hb_started[] = { SDE(getHost, ai_string) SDE(getPort, ai_long) HB_AWAIT PHP_FE_END };
static const zend_function_entry ev_hb_succeeded[] = { SDE(getHost, ai_string) SDE(getPort, ai_long) HB_AWAIT
    PHP_MALIAS(SwooleMongo_Driver_Monitoring_ServerHeartbeatSucceededEvent, getDurationMicros, getDurationMicros, ai_long, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
    PHP_MALIAS(SwooleMongo_Driver_Monitoring_ServerHeartbeatSucceededEvent, getReply, getReply, ai_object, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL) PHP_FE_END };
static const zend_function_entry ev_hb_failed[] = { SDE(getHost, ai_string) SDE(getPort, ai_long) HB_AWAIT
    PHP_MALIAS(SwooleMongo_Driver_Monitoring_ServerHeartbeatSucceededEvent, getDurationMicros, getDurationMicros, ai_long, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
    PHP_MALIAS(SwooleMongo_Driver_Monitoring_ServerHeartbeatFailedEvent, getError, getError, ai_exception, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL) PHP_FE_END };
static const zend_function_entry ev_topo_changed[] = {
    PHP_MALIAS(SwooleMongo_Driver_Monitoring_ServerChangedEvent, getNewDescription, getNewDescription, ai_td, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
    PHP_MALIAS(SwooleMongo_Driver_Monitoring_ServerChangedEvent, getPreviousDescription, getPreviousDescription, ai_td, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
    SDE(getTopologyId, ai_oid) PHP_FE_END };
static const zend_function_entry ev_topo_oc[] = { SDE(getTopologyId, ai_oid) PHP_FE_END };

#define SDAM_ARG(n, cls) ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(n, 0, 1, IS_VOID, 0) ZEND_ARG_OBJ_INFO(0, event, cls, 0) ZEND_END_ARG_INFO()
SDAM_ARG(ai_s_changed, SwooleMongo\\Driver\\Monitoring\\ServerChangedEvent)
SDAM_ARG(ai_s_opening, SwooleMongo\\Driver\\Monitoring\\ServerOpeningEvent)
SDAM_ARG(ai_s_closed, SwooleMongo\\Driver\\Monitoring\\ServerClosedEvent)
SDAM_ARG(ai_hb_started, SwooleMongo\\Driver\\Monitoring\\ServerHeartbeatStartedEvent)
SDAM_ARG(ai_hb_succeeded, SwooleMongo\\Driver\\Monitoring\\ServerHeartbeatSucceededEvent)
SDAM_ARG(ai_hb_failed, SwooleMongo\\Driver\\Monitoring\\ServerHeartbeatFailedEvent)
SDAM_ARG(ai_t_changed, SwooleMongo\\Driver\\Monitoring\\TopologyChangedEvent)
SDAM_ARG(ai_t_opening, SwooleMongo\\Driver\\Monitoring\\TopologyOpeningEvent)
SDAM_ARG(ai_t_closed, SwooleMongo\\Driver\\Monitoring\\TopologyClosedEvent)
#define SDAM_ABS(name, ai) ZEND_ABSTRACT_ME_WITH_FLAGS(SwooleMongo_Driver_Monitoring_SDAMSubscriber, name, ai, ZEND_ACC_PUBLIC | ZEND_ACC_ABSTRACT)
static const zend_function_entry sdam_subscriber_methods[] = {
    SDAM_ABS(serverChanged, ai_s_changed) SDAM_ABS(serverClosed, ai_s_closed) SDAM_ABS(serverOpening, ai_s_opening)
    SDAM_ABS(serverHeartbeatFailed, ai_hb_failed) SDAM_ABS(serverHeartbeatStarted, ai_hb_started) SDAM_ABS(serverHeartbeatSucceeded, ai_hb_succeeded)
    SDAM_ABS(topologyChanged, ai_t_changed) SDAM_ABS(topologyClosed, ai_t_closed) SDAM_ABS(topologyOpening, ai_t_opening)
    PHP_FE_END
};

static const zend_function_entry command_subscriber_methods[] = {
    ZEND_ABSTRACT_ME_WITH_FLAGS(SwooleMongo_Driver_Monitoring_CommandSubscriber, commandStarted, ai_ev_started, ZEND_ACC_PUBLIC | ZEND_ACC_ABSTRACT)
    ZEND_ABSTRACT_ME_WITH_FLAGS(SwooleMongo_Driver_Monitoring_CommandSubscriber, commandSucceeded, ai_ev_succeeded, ZEND_ACC_PUBLIC | ZEND_ACC_ABSTRACT)
    ZEND_ABSTRACT_ME_WITH_FLAGS(SwooleMongo_Driver_Monitoring_CommandSubscriber, commandFailed, ai_ev_failed, ZEND_ACC_PUBLIC | ZEND_ACC_ABSTRACT)
    PHP_FE_END
};

static zend_class_entry *reg_value(const char *name, const zend_function_entry *m, const char **props)
{
    zend_class_entry ce, *r;
    INIT_CLASS_ENTRY_EX(ce, name, strlen(name), m);
    r = zend_register_internal_class(&ce);
    r->ce_flags |= ZEND_ACC_FINAL;
    for (int i = 0; props && props[i]; i++) zend_declare_property_null(r, props[i], strlen(props[i]), ZEND_ACC_PRIVATE);
    return r;
}

static zend_class_entry *reg_custom(const char *name, const zend_function_entry *m, zend_object *(*create)(zend_class_entry *),
                                    zend_object_handlers *h, size_t offset, void (*free_obj)(zend_object *))
{
    zend_class_entry ce, *r;
    INIT_CLASS_ENTRY_EX(ce, name, strlen(name), m);
    r = zend_register_internal_class(&ce);
    r->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;
    r->create_object = create;
    memcpy(h, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    h->offset = (int) offset;
    h->free_obj = free_obj;
    h->clone_obj = NULL;
    return r;
}

void swm_driver_minit(void)
{
    zend_class_entry ce;
    static const char *p_rp[] = {"mode", "tags", "maxStalenessSeconds", "hedge", NULL};
    static const char *p_rc[] = {"level", NULL};
    static const char *p_wc[] = {"w", "wtimeout", "j", "isDefault", NULL};
    static const char *p_cmd[] = {"document", "options", NULL};
    static const char *p_query[] = {"filter", "options", NULL};
    static const char *p_bulk[] = {"ops", "options", "executed", NULL};
    static const char *p_wr[] = {"nInserted", "nMatched", "nModified", "nRemoved", "nUpserted", "upsertedIds", "writeErrors",
                                 "writeConcernError", "acknowledged", "server", NULL};
    static const char *p_we[] = {"code", "index", "message", "info", NULL};
    static const char *p_wce[] = {"code", "message", "info", NULL};
    static const char *p_ev[] = {"commandName", "databaseName", "operationId", "requestId", "host", "port", "durationMicros",
                                 "document", "error", NULL};

    swm_ce_Manager = reg_custom("SwooleMongo\\Driver\\Manager", manager_methods, manager_create, &manager_handlers,
                                XtOffsetOf(swm_manager_obj, std), manager_free);
    swm_ce_Server = reg_custom("SwooleMongo\\Driver\\Server", server_methods, server_create, &server_handlers,
                               XtOffsetOf(swm_server_obj, std), server_free);
    swm_ce_Session = reg_custom("SwooleMongo\\Driver\\Session", session_methods, session_create, &session_handlers,
                                XtOffsetOf(swm_session_obj, std), session_free);
    swm_ce_Cursor = reg_custom("SwooleMongo\\Driver\\Cursor", cursor_methods, cursor_create, &cursor_handlers,
                               XtOffsetOf(swm_cursor_obj, std), cursor_free);
    INIT_CLASS_ENTRY(ce, "SwooleMongo\\Driver\\CursorInterface", cursor_iface_methods);
    swm_ce_CursorInterface = zend_register_internal_interface(&ce);
    zend_class_implements(swm_ce_CursorInterface, 1, zend_ce_traversable);
    zend_class_implements(swm_ce_Cursor, 2, zend_ce_iterator, swm_ce_CursorInterface);

    swm_ce_ReadPreference = reg_value("SwooleMongo\\Driver\\ReadPreference", rp_methods, p_rp);
    swm_ce_ReadConcern = reg_value("SwooleMongo\\Driver\\ReadConcern", rc_methods, p_rc);
    swm_ce_WriteConcern = reg_value("SwooleMongo\\Driver\\WriteConcern", wc_methods, p_wc);
    zend_class_implements(swm_ce_ReadPreference, 1, swm_ce_Serializable);
    zend_class_implements(swm_ce_ReadConcern, 1, swm_ce_Serializable);
    zend_class_implements(swm_ce_WriteConcern, 1, swm_ce_Serializable);
    {
        static const char *p_api[] = {"version", "strict", "deprecationErrors", NULL};
        swm_ce_ServerApi = reg_value("SwooleMongo\\Driver\\ServerApi", api_methods, p_api);
        zend_class_implements(swm_ce_ServerApi, 1, swm_ce_Serializable);
        zend_declare_class_constant_string(swm_ce_ServerApi, ZEND_STRL("V1"), "1");
    }
    swm_ce_Command = reg_value("SwooleMongo\\Driver\\Command", command_methods, p_cmd);
    swm_ce_Query = reg_value("SwooleMongo\\Driver\\Query", query_methods, p_query);
    swm_ce_BulkWrite = reg_value("SwooleMongo\\Driver\\BulkWrite", bulk_methods, p_bulk);
    zend_class_implements(swm_ce_BulkWrite, 1, zend_ce_countable);
    swm_ce_WriteResult = reg_value("SwooleMongo\\Driver\\WriteResult", wr_methods, p_wr);
    {
        static const char *p_bwc[] = {"ops", "options", "executed", NULL};
        static const char *p_bwcr[] = {"insertedCount", "matchedCount", "modifiedCount", "upsertedCount", "deletedCount",
                                       "insertResults", "updateResults", "deleteResults", "acknowledged", NULL};
        zend_class_entry ce2;
        swm_ce_BulkWriteCommand = reg_value("SwooleMongo\\Driver\\BulkWriteCommand", bwc_methods, p_bwc);
        zend_class_implements(swm_ce_BulkWriteCommand, 1, zend_ce_countable);
        swm_ce_BulkWriteCommandResult = reg_value("SwooleMongo\\Driver\\BulkWriteCommandResult", bwcr_methods, p_bwcr);
        INIT_CLASS_ENTRY(ce2, "SwooleMongo\\Driver\\Exception\\BulkWriteCommandException", bwce_methods);
        swm_ce_BulkWriteCommandException = zend_register_internal_class_ex(&ce2, swm_ce_ServerException);
        swm_ce_BulkWriteCommandException->ce_flags |= ZEND_ACC_FINAL;
        zend_declare_property_null(swm_ce_BulkWriteCommandException, ZEND_STRL("partialResult"), ZEND_ACC_PRIVATE);
        zend_declare_property_null(swm_ce_BulkWriteCommandException, ZEND_STRL("writeErrors"), ZEND_ACC_PRIVATE);
        zend_declare_property_null(swm_ce_BulkWriteCommandException, ZEND_STRL("writeConcernErrors"), ZEND_ACC_PRIVATE);
        zend_declare_property_null(swm_ce_BulkWriteCommandException, ZEND_STRL("errorReply"), ZEND_ACC_PRIVATE);
    }
    swm_ce_WriteError = reg_value("SwooleMongo\\Driver\\WriteError", we_methods, p_we);
    swm_ce_WriteConcernError = reg_value("SwooleMongo\\Driver\\WriteConcernError", wce_methods, p_wce);

    INIT_CLASS_ENTRY(ce, "SwooleMongo\\Driver\\Monitoring\\Subscriber", NULL);
    swm_ce_Subscriber = zend_register_internal_interface(&ce);
    INIT_CLASS_ENTRY(ce, "SwooleMongo\\Driver\\Monitoring\\CommandSubscriber", command_subscriber_methods);
    swm_ce_CommandSubscriber = zend_register_internal_interface(&ce);
    zend_class_implements(swm_ce_CommandSubscriber, 1, swm_ce_Subscriber);
    swm_ce_CommandStartedEvent = reg_value("SwooleMongo\\Driver\\Monitoring\\CommandStartedEvent", started_methods, p_ev);
    swm_ce_CommandSucceededEvent = reg_value("SwooleMongo\\Driver\\Monitoring\\CommandSucceededEvent", succeeded_methods, p_ev);
    swm_ce_CommandFailedEvent = reg_value("SwooleMongo\\Driver\\Monitoring\\CommandFailedEvent", failed_methods, p_ev);
    {
        static const char *p_sd[] = {"host", "port", "type", "helloResponse", "lastUpdateTime", "roundTripTime", NULL};
        static const char *p_td[] = {"type", "servers", NULL};
        static const char *p_sev[] = {"host", "port", "topologyId", "newDescription", "previousDescription", "durationMicros", "reply", "error", "awaited", NULL};
        static const char *names[9] = {"ServerChangedEvent", "ServerOpeningEvent", "ServerClosedEvent", "ServerHeartbeatStartedEvent",
            "ServerHeartbeatSucceededEvent", "ServerHeartbeatFailedEvent", "TopologyChangedEvent", "TopologyOpeningEvent", "TopologyClosedEvent"};
        const zend_function_entry *tables[9] = {ev_server_changed, ev_server_oc, ev_server_oc, ev_hb_started, ev_hb_succeeded, ev_hb_failed,
            ev_topo_changed, ev_topo_oc, ev_topo_oc};
        static const char *sd_types[][2] = {{"TYPE_UNKNOWN", "Unknown"}, {"TYPE_STANDALONE", "Standalone"}, {"TYPE_MONGOS", "Mongos"},
            {"TYPE_POSSIBLE_PRIMARY", "PossiblePrimary"}, {"TYPE_RS_PRIMARY", "RSPrimary"}, {"TYPE_RS_SECONDARY", "RSSecondary"},
            {"TYPE_RS_ARBITER", "RSArbiter"}, {"TYPE_RS_OTHER", "RSOther"}, {"TYPE_RS_GHOST", "RSGhost"}, {"TYPE_LOAD_BALANCER", "LoadBalancer"}, {NULL, NULL}};
        static const char *td_types[][2] = {{"TYPE_UNKNOWN", "Unknown"}, {"TYPE_SINGLE", "Single"}, {"TYPE_SHARDED", "Sharded"},
            {"TYPE_REPLICA_SET_NO_PRIMARY", "ReplicaSetNoPrimary"}, {"TYPE_REPLICA_SET_WITH_PRIMARY", "ReplicaSetWithPrimary"},
            {"TYPE_LOAD_BALANCED", "LoadBalanced"}, {NULL, NULL}};
        char buf[128];
        swm_ce_ServerDescription = reg_value("SwooleMongo\\Driver\\ServerDescription", sd_methods, p_sd);
        swm_ce_TopologyDescription = reg_value("SwooleMongo\\Driver\\TopologyDescription", td_methods, p_td);
        for (int i = 0; sd_types[i][0]; i++) zend_declare_class_constant_string(swm_ce_ServerDescription, sd_types[i][0], strlen(sd_types[i][0]), sd_types[i][1]);
        for (int i = 0; td_types[i][0]; i++) zend_declare_class_constant_string(swm_ce_TopologyDescription, td_types[i][0], strlen(td_types[i][0]), td_types[i][1]);
        for (int i = 0; i < 9; i++) {
            snprintf(buf, sizeof(buf), "SwooleMongo\\Driver\\Monitoring\\%s", names[i]);
            sdam_ce[i] = reg_value(buf, tables[i], p_sev);
        }
        INIT_CLASS_ENTRY(ce, "SwooleMongo\\Driver\\Monitoring\\SDAMSubscriber", sdam_subscriber_methods);
        swm_ce_SDAMSubscriber = zend_register_internal_interface(&ce);
        zend_class_implements(swm_ce_SDAMSubscriber, 1, swm_ce_Subscriber);
    }

#define RP_CONST_S(n, v) zend_declare_class_constant_string(swm_ce_ReadPreference, ZEND_STRL(n), v)
#define RP_CONST_L(n, v) zend_declare_class_constant_long(swm_ce_ReadPreference, ZEND_STRL(n), v)
    RP_CONST_L("RP_PRIMARY", RP_PRIMARY);
    RP_CONST_L("RP_PRIMARY_PREFERRED", RP_PRIMARY_PREFERRED);
    RP_CONST_L("RP_SECONDARY", RP_SECONDARY);
    RP_CONST_L("RP_SECONDARY_PREFERRED", RP_SECONDARY_PREFERRED);
    RP_CONST_L("RP_NEAREST", RP_NEAREST);
    RP_CONST_S("PRIMARY", "primary");
    RP_CONST_S("PRIMARY_PREFERRED", "primaryPreferred");
    RP_CONST_S("SECONDARY", "secondary");
    RP_CONST_S("SECONDARY_PREFERRED", "secondaryPreferred");
    RP_CONST_S("NEAREST", "nearest");
    RP_CONST_L("NO_MAX_STALENESS", -1);
    RP_CONST_L("SMALLEST_MAX_STALENESS_SECONDS", 90);
    zend_declare_class_constant_string(swm_ce_ReadConcern, ZEND_STRL("LOCAL"), "local");
    zend_declare_class_constant_string(swm_ce_ReadConcern, ZEND_STRL("MAJORITY"), "majority");
    zend_declare_class_constant_string(swm_ce_ReadConcern, ZEND_STRL("LINEARIZABLE"), "linearizable");
    zend_declare_class_constant_string(swm_ce_ReadConcern, ZEND_STRL("AVAILABLE"), "available");
    zend_declare_class_constant_string(swm_ce_ReadConcern, ZEND_STRL("SNAPSHOT"), "snapshot");
    zend_declare_class_constant_string(swm_ce_WriteConcern, ZEND_STRL("MAJORITY"), "majority");
    zend_declare_class_constant_string(swm_ce_Session, ZEND_STRL("TRANSACTION_NONE"), "none");
    zend_declare_class_constant_string(swm_ce_Session, ZEND_STRL("TRANSACTION_STARTING"), "starting");
    zend_declare_class_constant_string(swm_ce_Session, ZEND_STRL("TRANSACTION_IN_PROGRESS"), "in_progress");
    zend_declare_class_constant_string(swm_ce_Session, ZEND_STRL("TRANSACTION_COMMITTED"), "committed");
    zend_declare_class_constant_string(swm_ce_Session, ZEND_STRL("TRANSACTION_ABORTED"), "aborted");
#define SRV_CONST(n, v) zend_declare_class_constant_long(swm_ce_Server, ZEND_STRL(n), v)
    SRV_CONST("TYPE_UNKNOWN", 0);
    SRV_CONST("TYPE_STANDALONE", 1);
    SRV_CONST("TYPE_MONGOS", 2);
    SRV_CONST("TYPE_POSSIBLE_PRIMARY", 3);
    SRV_CONST("TYPE_RS_PRIMARY", 4);
    SRV_CONST("TYPE_RS_SECONDARY", 5);
    SRV_CONST("TYPE_RS_ARBITER", 6);
    SRV_CONST("TYPE_RS_OTHER", 7);
    SRV_CONST("TYPE_RS_GHOST", 8);
    SRV_CONST("TYPE_LOAD_BALANCER", 9);
}
