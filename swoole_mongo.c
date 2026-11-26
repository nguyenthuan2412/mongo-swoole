/*
 * swoole-mongo module entry: INI, exception hierarchy, embedded PHP library.
 */
#include "php_swoole_mongo.h"
#include "swm_library.h"

ZEND_DECLARE_MODULE_GLOBALS(swoole_mongo)

zend_class_entry *swm_ce_Exception;
zend_class_entry *swm_ce_RuntimeException;
zend_class_entry *swm_ce_InvalidArgumentException;
zend_class_entry *swm_ce_LogicException;
zend_class_entry *swm_ce_UnexpectedValueException;
zend_class_entry *swm_ce_ConnectionException;
zend_class_entry *swm_ce_ConnectionTimeoutException;
zend_class_entry *swm_ce_AuthenticationException;
zend_class_entry *swm_ce_ServerException;
zend_class_entry *swm_ce_CommandException;
zend_class_entry *swm_ce_ExecutionTimeoutException;
zend_class_entry *swm_ce_WriteException;
zend_class_entry *swm_ce_BulkWriteException;
zend_class_entry *swm_ce_EncryptionException;

PHP_INI_BEGIN()
    STD_PHP_INI_BOOLEAN("swoole_mongo.enable_library", "1", PHP_INI_SYSTEM, OnUpdateBool, enable_library, zend_swoole_mongo_globals, swoole_mongo_globals)
    STD_PHP_INI_BOOLEAN("swoole_mongo.warn_blocking", "1", PHP_INI_ALL, OnUpdateBool, warn_blocking, zend_swoole_mongo_globals, swoole_mongo_globals)
PHP_INI_END()

/* --------------------------------------------------------- exceptions */
PHP_METHOD(SwooleMongo_Driver_Exception_RuntimeException, hasErrorLabel)
{
    zend_string *label;
    zval rv, *labels, *v;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(label)
    ZEND_PARSE_PARAMETERS_END();
    labels = zend_read_property(swm_ce_RuntimeException, Z_OBJ_P(ZEND_THIS), ZEND_STRL("errorLabels"), 1, &rv);
    if (labels && Z_TYPE_P(labels) == IS_ARRAY) {
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(labels), v) {
            if (Z_TYPE_P(v) == IS_STRING && zend_string_equals(Z_STR_P(v), label)) RETURN_TRUE;
        } ZEND_HASH_FOREACH_END();
    }
    RETURN_FALSE;
}

PHP_METHOD(SwooleMongo_Driver_Exception_CommandException, getResultDocument)
{
    zval rv, *v;
    ZEND_PARSE_PARAMETERS_NONE();
    v = zend_read_property(swm_ce_CommandException, Z_OBJ_P(ZEND_THIS), ZEND_STRL("resultDocument"), 1, &rv);
    if (v && Z_TYPE_P(v) != IS_NULL) RETURN_COPY(v);
    object_init(return_value);
}

PHP_METHOD(SwooleMongo_Driver_Exception_WriteException, getWriteResult)
{
    zval rv, *v;
    ZEND_PARSE_PARAMETERS_NONE();
    v = zend_read_property(swm_ce_WriteException, Z_OBJ_P(ZEND_THIS), ZEND_STRL("writeResult"), 1, &rv);
    RETURN_COPY(v);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_has_label, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, errorLabel, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_result_doc, 0, 0, IS_OBJECT, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_write_result, 0, 0, SwooleMongo\\Driver\\WriteResult, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry runtime_exception_methods[] = {
    PHP_ME(SwooleMongo_Driver_Exception_RuntimeException, hasErrorLabel, arginfo_has_label, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
    PHP_FE_END
};
static const zend_function_entry command_exception_methods[] = {
    PHP_ME(SwooleMongo_Driver_Exception_CommandException, getResultDocument, arginfo_result_doc, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
    PHP_FE_END
};
static const zend_function_entry write_exception_methods[] = {
    PHP_ME(SwooleMongo_Driver_Exception_WriteException, getWriteResult, arginfo_write_result, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
    PHP_FE_END
};

static zend_class_entry *reg_exception(const char *name, zend_class_entry *parent, const zend_function_entry *methods)
{
    zend_class_entry ce, *r;
    INIT_CLASS_ENTRY_EX(ce, name, strlen(name), methods);
    r = zend_register_internal_class_ex(&ce, parent);
    if (!instanceof_function(r, swm_ce_Exception)) zend_class_implements(r, 1, swm_ce_Exception);
    return r;
}

#define NS "SwooleMongo\\Driver\\Exception\\"

static void exceptions_minit(void)
{
    zend_class_entry ce;
    INIT_CLASS_ENTRY(ce, NS "Exception", NULL);
    swm_ce_Exception = zend_register_internal_interface(&ce);
    zend_class_implements(swm_ce_Exception, 1, zend_ce_throwable);

    swm_ce_RuntimeException = reg_exception(NS "RuntimeException", spl_ce_RuntimeException, runtime_exception_methods);
    zend_declare_property_null(swm_ce_RuntimeException, ZEND_STRL("errorLabels"), ZEND_ACC_PROTECTED);
    swm_ce_InvalidArgumentException = reg_exception(NS "InvalidArgumentException", spl_ce_InvalidArgumentException, NULL);
    swm_ce_LogicException = reg_exception(NS "LogicException", spl_ce_LogicException, NULL);
    swm_ce_UnexpectedValueException = reg_exception(NS "UnexpectedValueException", spl_ce_UnexpectedValueException, NULL);
    swm_ce_ConnectionException = reg_exception(NS "ConnectionException", swm_ce_RuntimeException, NULL);
    swm_ce_ConnectionTimeoutException = reg_exception(NS "ConnectionTimeoutException", swm_ce_ConnectionException, NULL);
    swm_ce_ConnectionTimeoutException->ce_flags |= ZEND_ACC_FINAL;
    swm_ce_AuthenticationException = reg_exception(NS "AuthenticationException", swm_ce_ConnectionException, NULL);
    swm_ce_ServerException = reg_exception(NS "ServerException", swm_ce_RuntimeException, NULL);
    swm_ce_CommandException = reg_exception(NS "CommandException", swm_ce_ServerException, command_exception_methods);
    zend_declare_property_null(swm_ce_CommandException, ZEND_STRL("resultDocument"), ZEND_ACC_PROTECTED);
    swm_ce_ExecutionTimeoutException = reg_exception(NS "ExecutionTimeoutException", swm_ce_ServerException, NULL);
    swm_ce_WriteException = reg_exception(NS "WriteException", swm_ce_ServerException, write_exception_methods);
    swm_ce_WriteException->ce_flags |= ZEND_ACC_EXPLICIT_ABSTRACT_CLASS;
    zend_declare_property_null(swm_ce_WriteException, ZEND_STRL("writeResult"), ZEND_ACC_PROTECTED);
    swm_ce_BulkWriteException = reg_exception(NS "BulkWriteException", swm_ce_WriteException, NULL);
    swm_ce_EncryptionException = reg_exception(NS "EncryptionException", swm_ce_RuntimeException, NULL);
}

/* ------------------------------------------------------------ module */
static PHP_GINIT_FUNCTION(swoole_mongo)
{
#if defined(COMPILE_DL_SWOOLE_MONGO) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    swoole_mongo_globals->enable_library = 1;
    swoole_mongo_globals->warn_blocking = 1;
}

PHP_MINIT_FUNCTION(swoole_mongo)
{
    REGISTER_INI_ENTRIES();
    exceptions_minit();
    swm_bson_minit();
    swm_driver_minit();
    swm_crypt_minit();
    REGISTER_STRING_CONSTANT("SWOOLE_MONGO_VERSION", PHP_SWOOLE_MONGO_VERSION, CONST_CS | CONST_PERSISTENT);
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(swoole_mongo)
{
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

/* Compiles the bundled high level PHP library (SwooleMongo\MongoClient, ...). */
static void load_library(void)
{
    if (zend_hash_str_exists(EG(class_table), ZEND_STRL("swoolemongo\\client"))) return;
    for (size_t i = 0; i < sizeof(swm_library) / sizeof(swm_library[0]); i++) {
        if (zend_eval_stringl(swm_library[i].code, swm_library[i].len, NULL, swm_library[i].name) != SUCCESS) {
            php_error_docref(NULL, E_WARNING, "swoole-mongo: failed to load bundled library file %s", swm_library[i].name);
            break;
        }
        if (EG(exception)) {
            zend_exception_error(EG(exception), E_WARNING);
            zend_clear_exception();
            break;
        }
    }
}

PHP_RINIT_FUNCTION(swoole_mongo)
{
#if defined(COMPILE_DL_SWOOLE_MONGO) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    if (SWMG(enable_library)) load_library();
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(swoole_mongo)
{
    swm_client_close_all();
    return SUCCESS;
}

PHP_MINFO_FUNCTION(swoole_mongo)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "swoole-mongo support", "enabled");
    php_info_print_table_row(2, "Version", PHP_SWOOLE_MONGO_VERSION);
    php_info_print_table_row(2, "Wire protocol", "OP_MSG, OP_COMPRESSED (zlib)");
    php_info_print_table_row(2, "Authentication", "SCRAM-SHA-1, SCRAM-SHA-256, PLAIN, MONGODB-X509");
    php_info_print_table_row(2, "Bundled library", SWMG(enable_library) ? "enabled" : "disabled");
    php_info_print_table_end();
    DISPLAY_INI_ENTRIES();
}

static const zend_module_dep swoole_mongo_deps[] = {
    ZEND_MOD_REQUIRED("json")
    ZEND_MOD_REQUIRED("date")
    ZEND_MOD_REQUIRED("spl")
    ZEND_MOD_REQUIRED("hash")
    ZEND_MOD_OPTIONAL("swoole")
    ZEND_MOD_END
};

zend_module_entry swoole_mongo_module_entry = {
    STANDARD_MODULE_HEADER_EX, NULL,
    swoole_mongo_deps,
    "swoole_mongo",
    NULL,
    PHP_MINIT(swoole_mongo),
    PHP_MSHUTDOWN(swoole_mongo),
    PHP_RINIT(swoole_mongo),
    PHP_RSHUTDOWN(swoole_mongo),
    PHP_MINFO(swoole_mongo),
    PHP_SWOOLE_MONGO_VERSION,
    PHP_MODULE_GLOBALS(swoole_mongo),
    PHP_GINIT(swoole_mongo),
    NULL,
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_SWOOLE_MONGO
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(swoole_mongo)
#endif
