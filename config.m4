PHP_ARG_ENABLE([swmongo],
  [whether to enable swmongo support],
  [AS_HELP_STRING([--enable-swmongo], [Enable swmongo (coroutine MongoDB driver for Swoole)])],
  [no])

PHP_ARG_WITH([swmongo-gssapi],
  [for Kerberos (GSSAPI) authentication],
  [AS_HELP_STRING([[--with-swmongo-gssapi[=DIR]]], [swmongo: enable MONGODB GSSAPI auth (krb5-config prefix, "auto" to detect)])],
  [auto], [no])

PHP_ARG_WITH([swmongo-mongocrypt],
  [for libmongocrypt (client-side field level encryption)],
  [AS_HELP_STRING([[--with-swmongo-mongocrypt[=DIR]]], [swmongo: enable CSFLE / Queryable Encryption with libmongocrypt installed in DIR ("auto" to use pkg-config)])],
  [auto], [no])

if test "$PHP_SWMONGO" != "no"; then
  PHP_CHECK_LIBRARY(z, compress2, [
    PHP_ADD_LIBRARY(z, 1, SWMONGO_SHARED_LIBADD)
  ], [
    AC_MSG_ERROR([zlib is required by swmongo])
  ])

  dnl ---------------------------------------------------------------- GSSAPI
  SWM_EXTRA_CFLAGS=""
  if test "$PHP_SWMONGO_GSSAPI" != "no"; then
    SWM_KRB5_CONFIG=""
    if test "$PHP_SWMONGO_GSSAPI" != "auto" && test "$PHP_SWMONGO_GSSAPI" != "yes"; then
      SWM_KRB5_CONFIG="$PHP_SWMONGO_GSSAPI/bin/krb5-config"
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
      PHP_EVAL_LIBLINE($SWM_GSS_LIBS, SWMONGO_SHARED_LIBADD)
    else
      AC_MSG_RESULT([not found])
      if test "$PHP_SWMONGO_GSSAPI" != "auto"; then
        AC_MSG_ERROR([krb5-config not found; install MIT Kerberos (libkrb5-dev / brew krb5)])
      fi
    fi
  fi

  dnl ----------------------------------------------------------- libmongocrypt
  if test "$PHP_SWMONGO_MONGOCRYPT" != "no"; then
    SWM_MC_FOUND=no
    if test "$PHP_SWMONGO_MONGOCRYPT" != "auto" && test "$PHP_SWMONGO_MONGOCRYPT" != "yes"; then
      export PKG_CONFIG_PATH="$PHP_SWMONGO_MONGOCRYPT/lib/pkgconfig:$PHP_SWMONGO_MONGOCRYPT/lib64/pkgconfig:$PKG_CONFIG_PATH"
    fi
    AC_MSG_CHECKING([for libmongocrypt])
    if pkg-config --exists libmongocrypt 2>/dev/null; then
      SWM_MC_CFLAGS=`pkg-config --cflags libmongocrypt`
      SWM_MC_LIBS=`pkg-config --libs libmongocrypt`
      SWM_MC_LIBDIR=`pkg-config --variable=libdir libmongocrypt`
      AC_MSG_RESULT([yes ($SWM_MC_LIBDIR)])
      SWM_EXTRA_CFLAGS="$SWM_EXTRA_CFLAGS $SWM_MC_CFLAGS -DSWM_HAVE_MONGOCRYPT=1"
      PHP_EVAL_LIBLINE($SWM_MC_LIBS, SWMONGO_SHARED_LIBADD)
      PHP_ADD_LIBPATH($SWM_MC_LIBDIR, SWMONGO_SHARED_LIBADD)
      SWM_MC_FOUND=yes
    else
      AC_MSG_RESULT([no])
      if test "$PHP_SWMONGO_MONGOCRYPT" != "auto"; then
        AC_MSG_ERROR([libmongocrypt not found (pkg-config libmongocrypt)])
      fi
    fi
  fi
  PHP_SUBST(SWMONGO_SHARED_LIBADD)

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

  PHP_NEW_EXTENSION(swmongo, swm_util.c, $ext_shared,, -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1 -Wall -Wno-unused-parameter $SWM_EXTRA_CFLAGS)
  PHP_ADD_SOURCES_X(PHP_EXT_DIR(swmongo), swmongo.c swm_bson.c swm_client.c swm_driver.c swm_crypt.c, -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1 -Wall -Wno-unused-parameter $SWM_IO_CFLAGS $SWM_EXTRA_CFLAGS, shared_objects_swmongo, yes)
  PHP_ADD_EXTENSION_DEP(swmongo, json)
  PHP_ADD_EXTENSION_DEP(swmongo, date)
  PHP_ADD_EXTENSION_DEP(swmongo, spl)
  PHP_ADD_EXTENSION_DEP(swmongo, hash)
fi
