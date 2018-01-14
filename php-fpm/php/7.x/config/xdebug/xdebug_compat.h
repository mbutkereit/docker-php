/*
   +----------------------------------------------------------------------+
   | Xdebug                                                               |
   +----------------------------------------------------------------------+
   | Copyright (c) 2002-2017 Derick Rethans                               |
   +----------------------------------------------------------------------+
   | This source file is subject to version 1.0 of the Xdebug license,    |
   | that is bundled with this package in the file LICENSE, and is        |
   | available at through the world-wide-web at                           |
   | http://xdebug.derickrethans.nl/license.php                           |
   | If you did not receive a copy of the Xdebug license and are unable   |
   | to obtain it through the world-wide-web, please send a note to       |
   | xdebug@derickrethans.nl so we can mail you a copy immediately.       |
   +----------------------------------------------------------------------+
   | Authors:  Derick Rethans <derick@xdebug.org>                         |
   +----------------------------------------------------------------------+
 */

#ifndef __HAVE_XDEBUG_COMPAT_H__
#define __HAVE_XDEBUG_COMPAT_H__

#include "php.h"

#include "ext/standard/head.h"
#include "ext/standard/php_var.h"
#define xdebug_php_var_dump php_var_dump

zval *xdebug_zval_ptr(int op_type, const znode_op *node, zend_execute_data *zdata TSRMLS_DC);

char *xdebug_str_to_str(char *haystack, size_t length, char *needle, size_t needle_len, char *str, size_t str_len, size_t *new_len);
char *xdebug_base64_encode(unsigned char *data, int data_len, int *new_len);
unsigned char *xdebug_base64_decode(unsigned char *data, int data_len, int *new_len);
void xdebug_stripcslashes(char *string, int *new_len);
zend_class_entry *xdebug_fetch_class(char *classname, int classname_len, int flags TSRMLS_DC);
int xdebug_get_constant(char *val, int len, zval *const_val TSRMLS_DC);
void xdebug_setcookie(char *name, int name_len, char *value, int value_len, time_t expires, char *path, int path_len, char *domain, int domain_len, int secure, int url_encode, int httponly TSRMLS_DC);
char *xdebug_get_compiled_variable_name(zend_op_array *op_array, uint32_t var, int *cv_len);
zval *xdebug_read_property(zend_class_entry *ce, zval *exception, char *name, int length, int flags TSRMLS_DC);

# define XDEBUG_MAKE_STD_ZVAL(zv) \
	zv = ecalloc(sizeof(zval), 1);

# define HASH_KEY_SIZEOF(k) (sizeof(k) - 1)
# define HASH_KEY_STRLEN(k) (strlen(k))
# define HASH_KEY_IS_NUMERIC(k) ((k) == NULL)
# define HASH_APPLY_KEY_VAL(k) (k)->val
# define HASH_APPLY_KEY_LEN(k) (k)->len + 1

# define STR_NAME_VAL(k) (k)->val
# define STR_NAME_LEN(k) (k)->len

#endif
