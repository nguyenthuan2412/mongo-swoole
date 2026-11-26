PHP_ARG_ENABLE([swoole_mongo],
  [whether to enable swoole-mongo support],
  [AS_HELP_STRING([--enable-swoole-mongo], [Enable swoole-mongo (coroutine MongoDB driver for Swoole)])],
  [yes])

PHP_ARG_WITH([swoole-mongo-gssapi],
  [for Kerberos (GSSAPI) authentication],
  [AS_HELP_STRING([[--with-swoole-mongo-gssapi[=DIR]]], [swoole-mongo: enable MONGODB GSSAPI auth (krb5-config prefix, "auto" to detect)])],
  [auto], [no])

PHP_ARG_WITH([swoole-mongo-mongocrypt],
  [for libmongocrypt (client-side field level encryption)],
  [AS_HELP_STRING([[--with-swoole-mongo-mongocrypt[=DIR]]], [swoole-mongo: enable CSFLE / Queryable Encryption with libmongocrypt installed in DIR ("auto" to use pkg-config)])],
  [auto], [no])

if test "$PHP_SWOOLE_MONGO" != "no"; then
  dnl The PHP layer (library/*.php) is embedded into the .so through swm_library.h.
  dnl build.sh generates it; PIE / PECL builds run configure directly, so do it here too.
  SWM_SRCDIR="$ext_srcdir"
  test -z "$SWM_SRCDIR" && SWM_SRCDIR="."
  if test -f "$SWM_SRCDIR/tools/gen_library.php"; then
    SWM_PHP="$PHP_EXECUTABLE"
    if test -z "$SWM_PHP" && test -n "$PHP_CONFIG"; then SWM_PHP=`$PHP_CONFIG --php-binary 2>/dev/null`; fi
    test -z "$SWM_PHP" && SWM_PHP=php
    AC_MSG_CHECKING([for the embedded PHP library])
    if "$SWM_PHP" "$SWM_SRCDIR/tools/gen_library.php" >/dev/null 2>&1; then
      AC_MSG_RESULT([generated swm_library.h])
    elif test -f "$SWM_SRCDIR/swm_library.h"; then
      AC_MSG_RESULT([using the bundled swm_library.h])
    else
      AC_MSG_ERROR([cannot generate swm_library.h: run "php tools/gen_library.php" first])
    fi
  elif test ! -f "$SWM_SRCDIR/swm_library.h"; then
    AC_MSG_ERROR([swm_library.h is missing])
  fi

  PHP_CHECK_LIBRARY(z, compress2, [
    PHP_ADD_LIBRARY(z, 1, SWOOLE_MONGO_SHARED_LIBADD)
  ], [
    AC_MSG_ERROR([zlib is required by swoole_mongo])
  ])

  dnl ---------------------------------------------------------------- GSSAPI
  SWM_EXTRA_CFLAGS=""
  if test "$PHP_SWOOLE_MONGO_GSSAPI" != "no"; then
    SWM_KRB5_CONFIG=""
    if test "$PHP_SWOOLE_MONGO_GSSAPI" != "auto" && test "$PHP_SWOOLE_MONGO_GSSAPI" != "yes"; then
      SWM_KRB5_CONFIG="$PHP_SWOOLE_MONGO_GSSAPI/bin/krb5-config"
    else
      for p in /opt/homebrew/opt/krb5/bin/krb5-config /usr/local/opt/krb5/bin/krb5-config /usr/bin/krb5-config /usr/local/bin/krb5-config; do
        if test -x "$p"; then SWM_KRB5_CONFIG="$p"; break; fi
      done
    fi
    AC_MSG_CHECKING([for krb5-config])
    if test -n "$SWM_KRB5_CONFIG" && test -x "$SWM_KRB5_CONFIG"; then
      AC_MSG_RESULT([$SWM_KRB5_CONFIG])
      SWM_GSS_CFLAGS=`$SWM_KRB5_CONFIG --cflags gssapi`
      SWM_GSS_LIBS=`$SWM_KRB5_CONFIG --libs gssapi`
      SWM_EXTRA_CFLAGS="$SWM_EXTRA_CFLAGS $SWM_GSS_CFLAGS -DSWM_HAVE_GSSAPI=1"
      if $SWM_KRB5_CONFIG --vendor 2>/dev/null | grep -qi "massachusetts"; then
        SWM_EXTRA_CFLAGS="$SWM_EXTRA_CFLAGS -DSWM_HAVE_GSS_PASSWORD=1"
      fi
      PHP_EVAL_LIBLINE($SWM_GSS_LIBS, SWOOLE_MONGO_SHARED_LIBADD)
    else
      AC_MSG_RESULT([not found])
      if test "$PHP_SWOOLE_MONGO_GSSAPI" != "auto"; then
        AC_MSG_ERROR([krb5-config not found; install MIT Kerberos (libkrb5-dev / brew krb5)])
      fi
    fi
  fi

  dnl ----------------------------------------------------------- libmongocrypt
  if test "$PHP_SWOOLE_MONGO_MONGOCRYPT" != "no"; then
    SWM_MC_FOUND=no
    if test "$PHP_SWOOLE_MONGO_MONGOCRYPT" != "auto" && test "$PHP_SWOOLE_MONGO_MONGOCRYPT" != "yes"; then
      export PKG_CONFIG_PATH="$PHP_SWOOLE_MONGO_MONGOCRYPT/lib/pkgconfig:$PHP_SWOOLE_MONGO_MONGOCRYPT/lib64/pkgconfig:$PKG_CONFIG_PATH"
    fi
    AC_MSG_CHECKING([for libmongocrypt])
    if pkg-config --exists libmongocrypt 2>/dev/null; then
      SWM_MC_CFLAGS=`pkg-config --cflags libmongocrypt`
      SWM_MC_LIBS=`pkg-config --libs libmongocrypt`
      SWM_MC_LIBDIR=`pkg-config --variable=libdir libmongocrypt`
      AC_MSG_RESULT([yes ($SWM_MC_LIBDIR)])
      SWM_EXTRA_CFLAGS="$SWM_EXTRA_CFLAGS $SWM_MC_CFLAGS -DSWM_HAVE_MONGOCRYPT=1"
      PHP_EVAL_LIBLINE($SWM_MC_LIBS, SWOOLE_MONGO_SHARED_LIBADD)
      PHP_ADD_LIBPATH($SWM_MC_LIBDIR, SWOOLE_MONGO_SHARED_LIBADD)
      SWM_MC_FOUND=yes
    else
      AC_MSG_RESULT([no])
      if test "$PHP_SWOOLE_MONGO_MONGOCRYPT" != "auto"; then
        AC_MSG_ERROR([libmongocrypt not found (pkg-config libmongocrypt)])
      fi
    fi
  fi
  PHP_SUBST(SWOOLE_MONGO_SHARED_LIBADD)

  dnl Files that perform I/O (and can therefore yield to another coroutine) must not
  dnl use floating point registers: Swoole's arm64 context switch does not preserve
  dnl the callee-saved FP registers d8-d15. -mgeneral-regs-only turns any FP use in
  dnl those files into a compile error.
  SWM_IO_CFLAGS=""
  case $host_cpu in
    aarch64*|arm64*)
      dnl zend_types.h unconditionally includes arm_neon.h (unused by the headers); skip it.
      SWM_IO_CFLAGS="-mgeneral-regs-only -D__ARM_NEON_H=1"
      ;;
  esac

  PHP_NEW_EXTENSION(swoole_mongo, swm_util.c, $ext_shared,, -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1 -Wall -Wno-unused-parameter $SWM_EXTRA_CFLAGS)
  PHP_ADD_SOURCES_X(PHP_EXT_DIR(swoole_mongo), swoole_mongo.c swm_bson.c swm_client.c swm_driver.c swm_crypt.c, -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1 -Wall -Wno-unused-parameter $SWM_IO_CFLAGS $SWM_EXTRA_CFLAGS, shared_objects_swoole_mongo, yes)
  PHP_ADD_EXTENSION_DEP(swoole_mongo, json)
  PHP_ADD_EXTENSION_DEP(swoole_mongo, date)
  PHP_ADD_EXTENSION_DEP(swoole_mongo, spl)
  PHP_ADD_EXTENSION_DEP(swoole_mongo, hash)
fi
