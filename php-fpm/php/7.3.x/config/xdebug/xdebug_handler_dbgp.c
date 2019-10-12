/*
   +----------------------------------------------------------------------+
   | Xdebug                                                               |
   +----------------------------------------------------------------------+
   | Copyright (c) 2002-2019 Derick Rethans                               |
   +----------------------------------------------------------------------+
   | This source file is subject to version 1.01 of the Xdebug license,   |
   | that is bundled with this package in the file LICENSE, and is        |
   | available at through the world-wide-web at                           |
   | https://xdebug.org/license.php                                       |
   | If you did not receive a copy of the Xdebug license and are unable   |
   | to obtain it through the world-wide-web, please send a note to       |
   | derick@xdebug.org so we can mail you a copy immediately.             |
   +----------------------------------------------------------------------+
   | Authors: Derick Rethans <derick@xdebug.org>                          |
   |          Shane Caraveo <shanec@ActiveState.com>                      |
   +----------------------------------------------------------------------+
 */

#include <sys/types.h>

#ifndef PHP_WIN32
#include <unistd.h>
#endif

#include "php.h"
#include "SAPI.h"

#include "ext/standard/php_string.h"
#include "ext/standard/url.h"
#include "main/php_version.h"
#include "main/php_network.h"
#include "ext/standard/base64.h"
#include "TSRM.h"
#include "php_globals.h"
#include "php_xdebug.h"
#include "xdebug_private.h"
#include "xdebug_code_coverage.h"
#include "xdebug_com.h"
#include "xdebug_compat.h"
#include "xdebug_handler_dbgp.h"
#include "xdebug_hash.h"
#include "xdebug_llist.h"
#include "xdebug_mm.h"
#include "xdebug_private.h"
#include "xdebug_stack.h"
#include "xdebug_var.h"
#include "xdebug_xml.h"

#include "xdebug_compat.h"

#ifdef PHP_WIN32
#include "win32/time.h"
#include <process.h>
#endif
#include <fcntl.h>

ZEND_EXTERN_MODULE_GLOBALS(xdebug)
static char *create_eval_key_file(char *filename, int lineno);
static char *create_eval_key_id(int id);
static void line_breakpoint_resolve_helper(xdebug_con *context, function_stack_entry *fse, xdebug_brk_info *brk_info);

/*****************************************************************************
** Constants and strings for statii and reasons
*/

/* Status structure */
#define DBGP_STATUS_STARTING  1
#define DBGP_STATUS_STOPPING  2
#define DBGP_STATUS_STOPPED   3
#define DBGP_STATUS_RUNNING   4
#define DBGP_STATUS_BREAK     5
#define DBGP_STATUS_DETACHED  6

const char *xdebug_dbgp_status_strings[6] =
	{"", "starting", "stopping", "stopped", "running", "break"};

#define DBGP_REASON_OK        0
#define DBGP_REASON_ERROR     1
#define DBGP_REASON_ABORTED   2
#define DBGP_REASON_EXCEPTION 3

const char *xdebug_dbgp_reason_strings[4] =
	{"ok", "error", "aborted", "exception"};

typedef struct {
	int         code;
	const char *message;
} xdebug_error_entry;

xdebug_error_entry xdebug_error_codes[24] = {
	{   0, "no error" },
	{   1, "parse error in command" },
	{   2, "duplicate arguments in command" },
	{   3, "invalid or missing options" },
	{   4, "unimplemented command" },
	{   5, "command is not available" },
	{ 100, "can not open file" },
	{ 101, "stream redirect failed" },
	{ 200, "breakpoint could not be set" },
	{ 201, "breakpoint type is not supported" },
	{ 202, "invalid breakpoint line" },
	{ 203, "no code on breakpoint line" },
	{ 204, "invalid breakpoint state" },
	{ 205, "no such breakpoint" },
	{ 206, "error evaluating code" },
	{ 207, "invalid expression" },
	{ 300, "can not get property" },
	{ 301, "stack depth invalid" },
	{ 302, "context invalid" },
	{ 800, "profiler not started" },
	{ 900, "encoding not supported" },
	{ 998, "an internal exception in the debugger" },
	{ 999, "unknown error" },
	{  -1, NULL }
};

#define XDEBUG_STR_SWITCH_DECL       char *__switch_variable
#define XDEBUG_STR_SWITCH(s)         __switch_variable = (s);
#define XDEBUG_STR_CASE(s)           if (strcmp(__switch_variable, s) == 0) {
#define XDEBUG_STR_CASE_END          } else
#define XDEBUG_STR_CASE_DEFAULT      {
#define XDEBUG_STR_CASE_DEFAULT_END  }

#define XDEBUG_TYPES_COUNT 8
const char *xdebug_dbgp_typemap[XDEBUG_TYPES_COUNT][3] = {
	/* common, lang, schema */
	{"bool",     "bool",     "xsd:boolean"},
	{"int",      "int",      "xsd:decimal"},
	{"float",    "float",    "xsd:double"},
	{"string",   "string",   "xsd:string"},
	{"null",     "null",     NULL},
	{"hash",     "array",    NULL},
	{"object",   "object",   NULL},
	{"resource", "resource", NULL}
};


typedef struct {
	int         value;
	const char *name;
} xdebug_breakpoint_entry;

#define XDEBUG_BREAKPOINT_TYPES_COUNT 6
xdebug_breakpoint_entry xdebug_breakpoint_types[XDEBUG_BREAKPOINT_TYPES_COUNT] = {
	{ XDEBUG_BREAKPOINT_TYPE_LINE,        "line" },
	{ XDEBUG_BREAKPOINT_TYPE_CONDITIONAL, "conditional" },
	{ XDEBUG_BREAKPOINT_TYPE_CALL,        "call" },
	{ XDEBUG_BREAKPOINT_TYPE_RETURN,      "return" },
	{ XDEBUG_BREAKPOINT_TYPE_EXCEPTION,   "exception" },
	{ XDEBUG_BREAKPOINT_TYPE_WATCH,       "watch" }
};

#define XDEBUG_DBGP_SCAN_RANGE 5

/*****************************************************************************
** Prototypes for debug command handlers
*/

/* DBGP_FUNC(break); */
DBGP_FUNC(breakpoint_get);
DBGP_FUNC(breakpoint_list);
DBGP_FUNC(breakpoint_remove);
DBGP_FUNC(breakpoint_set);
DBGP_FUNC(breakpoint_update);

DBGP_FUNC(context_get);
DBGP_FUNC(context_names);

DBGP_FUNC(eval);
DBGP_FUNC(feature_get);
DBGP_FUNC(feature_set);

DBGP_FUNC(typemap_get);
DBGP_FUNC(property_get);
DBGP_FUNC(property_set);
DBGP_FUNC(property_value);

DBGP_FUNC(source);
DBGP_FUNC(stack_depth);
DBGP_FUNC(stack_get);
DBGP_FUNC(status);

DBGP_FUNC(stderr);
DBGP_FUNC(stdout);

DBGP_FUNC(stop);
DBGP_FUNC(run);
DBGP_FUNC(step_into);
DBGP_FUNC(step_out);
DBGP_FUNC(step_over);
DBGP_FUNC(detach);

/* Non standard comments */
DBGP_FUNC(xcmd_profiler_name_get);
DBGP_FUNC(xcmd_get_executable_lines);

/*****************************************************************************
** Dispatcher tables for supported debug commands
*/

static xdebug_dbgp_cmd dbgp_commands[] = {
	/* DBGP_FUNC_ENTRY(break) */
	DBGP_FUNC_ENTRY(breakpoint_get,    XDEBUG_DBGP_NONE)
	DBGP_FUNC_ENTRY(breakpoint_list,   XDEBUG_DBGP_POST_MORTEM)
	DBGP_FUNC_ENTRY(breakpoint_remove, XDEBUG_DBGP_NONE)
	DBGP_FUNC_ENTRY(breakpoint_set,    XDEBUG_DBGP_NONE)
	DBGP_FUNC_ENTRY(breakpoint_update, XDEBUG_DBGP_NONE)

	DBGP_FUNC_ENTRY(context_get,       XDEBUG_DBGP_NONE)
	DBGP_FUNC_ENTRY(context_names,     XDEBUG_DBGP_POST_MORTEM)

	DBGP_FUNC_ENTRY(eval,              XDEBUG_DBGP_NONE)
	DBGP_FUNC_ENTRY(feature_get,       XDEBUG_DBGP_POST_MORTEM)
	DBGP_FUNC_ENTRY(feature_set,       XDEBUG_DBGP_NONE)

	DBGP_FUNC_ENTRY(typemap_get,       XDEBUG_DBGP_POST_MORTEM)
	DBGP_FUNC_ENTRY(property_get,      XDEBUG_DBGP_NONE)
	DBGP_FUNC_ENTRY(property_set,      XDEBUG_DBGP_NONE)
	DBGP_FUNC_ENTRY(property_value,    XDEBUG_DBGP_NONE)

	DBGP_FUNC_ENTRY(source,            XDEBUG_DBGP_NONE)
	DBGP_FUNC_ENTRY(stack_depth,       XDEBUG_DBGP_NONE)
	DBGP_FUNC_ENTRY(stack_get,         XDEBUG_DBGP_NONE)
	DBGP_FUNC_ENTRY(status,            XDEBUG_DBGP_POST_MORTEM)

	DBGP_FUNC_ENTRY(stderr,            XDEBUG_DBGP_NONE)
	DBGP_FUNC_ENTRY(stdout,            XDEBUG_DBGP_NONE)

	DBGP_CONT_FUNC_ENTRY(run,          XDEBUG_DBGP_NONE)
	DBGP_CONT_FUNC_ENTRY(step_into,    XDEBUG_DBGP_NONE)
	DBGP_CONT_FUNC_ENTRY(step_out,     XDEBUG_DBGP_NONE)
	DBGP_CONT_FUNC_ENTRY(step_over,    XDEBUG_DBGP_NONE)

	DBGP_STOP_FUNC_ENTRY(stop,         XDEBUG_DBGP_POST_MORTEM)
	DBGP_STOP_FUNC_ENTRY(detach,       XDEBUG_DBGP_NONE)

	/* Non standard functions */
	DBGP_FUNC_ENTRY(xcmd_profiler_name_get,    XDEBUG_DBGP_POST_MORTEM)
	DBGP_FUNC_ENTRY(xcmd_get_executable_lines, XDEBUG_DBGP_NONE)
	{ NULL, NULL, 0, 0 }
};

/*****************************************************************************
** Utility functions
*/

static xdebug_dbgp_cmd* lookup_cmd(char *cmd)
{
	xdebug_dbgp_cmd *ptr = dbgp_commands;

	while (ptr->name) {
		if (strcmp(ptr->name, cmd) == 0) {
			return ptr;
		}
		ptr++;
	}
	return NULL;
}

void XDEBUG_ATTRIBUTE_FORMAT(printf, 2, 3) xdebug_dbgp_log(int log_level, const char *fmt, ...)
{
	if (XG(remote_log_file) && XG(remote_log_level) >= log_level) {
		va_list    argv;
		zend_ulong pid = xdebug_get_pid();

		fprintf(XG(remote_log_file), "[" ZEND_ULONG_FMT "] %s", pid, xdebug_log_prefix[log_level]);
		va_start(argv, fmt);
		vfprintf(XG(remote_log_file), fmt, argv);
		va_end(argv);

		fflush(XG(remote_log_file));
	}
}

static xdebug_str *make_message(xdebug_con *context, xdebug_xml_node *message TSRMLS_DC)
{
	xdebug_str  xml_message = XDEBUG_STR_INITIALIZER;
	xdebug_str *ret = xdebug_str_new();

	xdebug_xml_return_node(message, &xml_message);
	context->handler->log(XDEBUG_LOG_COM, "-> %s\n\n", xml_message.d);

	xdebug_str_add(ret, xdebug_sprintf("%d", xml_message.l + sizeof("<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>\n") - 1), 1);
	xdebug_str_addl(ret, "\0", 1, 0);
	xdebug_str_add(ret, "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>\n", 0);
	xdebug_str_add(ret, xml_message.d, 0);
	xdebug_str_addl(ret, "\0", 1, 0);
	xdebug_str_destroy(&xml_message);

	return ret;
}

static void send_message_ex(xdebug_con *context, xdebug_xml_node *message, int stage TSRMLS_DC)
{
	xdebug_str *tmp;

	/* Sometimes we end up in 'send_message' although the debugging connection
	 * is already closed. In that case, we early return. */
	if (XG(status) != DBGP_STATUS_STARTING && !xdebug_is_debug_connection_active()) {
		return;
	}

	tmp = make_message(context, message TSRMLS_CC);
	if ((size_t) SSENDL(context->socket, tmp->d, tmp->l) != tmp->l) {
		char *sock_error = php_socket_strerror(php_socket_errno(), NULL, 0);
		char *utime_str = xdebug_sprintf("%F", xdebug_get_utime());

		fprintf(stderr, "%s: There was a problem sending %zd bytes on socket %d: %s\n", utime_str, tmp->l, context->socket, sock_error);

		efree(sock_error);
		xdfree(utime_str);
	}
	xdebug_str_free(tmp);
}

static void send_message(xdebug_con *context, xdebug_xml_node *message TSRMLS_DC)
{
	send_message_ex(context, message, 0);
}


static xdebug_xml_node* get_symbol(xdebug_str *name, xdebug_var_export_options *options)
{
	zval                   retval;
	xdebug_xml_node       *tmp_node;

	xdebug_get_php_symbol(&retval, name TSRMLS_CC);
	if (Z_TYPE(retval) != IS_UNDEF) {
		if (strcmp(name->d, "this") == 0 && Z_TYPE(retval) == IS_NULL) {
			return NULL;
		}
		tmp_node = xdebug_get_zval_value_xml_node(name, &retval, options TSRMLS_CC);
		zval_ptr_dtor_nogc(&retval);
		return tmp_node;
	}

	return NULL;
}

static int get_symbol_contents(xdebug_str *name, xdebug_xml_node *node, xdebug_var_export_options *options)
{
	zval retval;

	xdebug_get_php_symbol(&retval, name TSRMLS_CC);
	if (Z_TYPE(retval) != IS_UNDEF) {
		// TODO WTF???
		zval *retval_ptr = &retval;
		xdebug_var_export_xml_node(&retval_ptr, name, node, options, 1 TSRMLS_CC);
		zval_ptr_dtor_nogc(&retval);
		return 1;
	}

	return 0;
}

static xdebug_str* return_file_source(char *filename, int begin, int end TSRMLS_DC)
{
	php_stream *stream;
	int    i = begin;
	char  *line = NULL;
	xdebug_str *source = xdebug_str_new();

	if (i < 0) {
		begin = 0;
		i = 0;
	}

	filename = xdebug_path_from_url(filename TSRMLS_CC);
	stream = php_stream_open_wrapper(filename, "rb",
			USE_PATH | REPORT_ERRORS,
			NULL);
	xdfree(filename);

	/* Read until the "begin" line has been read */
	if (!stream) {
		return NULL;
	}

	/* skip to the first requested line */
	while (i > 0 && !php_stream_eof(stream)) {
		if (line) {
			efree(line);
			line = NULL;
		}
		line = php_stream_gets(stream, NULL, 1024);
		i--;
	}
	/* Read until the "end" line has been read */
	do {
		if (line) {
			xdebug_str_add(source, line, 0);
			efree(line);
			line = NULL;
			if (php_stream_eof(stream)) break;
		}
		line = php_stream_gets(stream, NULL, 1024);
		i++;
	} while (i < end + 1 - begin);

	/* Print last line */
	if (line) {
		efree(line);
		line = NULL;
	}
	php_stream_close(stream);
	return source;
}

static xdebug_str* return_eval_source(char *id, int begin, int end TSRMLS_DC)
{
	char             *key;
	xdebug_str       *joined;
	xdebug_eval_info *ei;
	xdebug_arg       *parts = (xdebug_arg*) xdmalloc(sizeof(xdebug_arg));

	if (begin < 0) {
		begin = 0;
	}
	key = create_eval_key_id(atoi(id));
	if (xdebug_hash_find(XG(context).eval_id_lookup, key, strlen(key), (void *) &ei)) {
		xdebug_arg_init(parts);
		xdebug_explode("\n", ei->contents, parts, end + 2);
		joined = xdebug_join("\n", parts, begin, end);
		xdebug_arg_dtor(parts);
		return joined;
	}
	return NULL;
}

static int is_dbgp_url(const char *filename)
{
	return (strncmp(filename, "dbgp://", 7) == 0);
}

static xdebug_str* return_source(char *filename, int begin, int end TSRMLS_DC)
{
	if (is_dbgp_url(filename)) {
		return return_eval_source(filename + 7, begin, end TSRMLS_CC);
	} else {
		return return_file_source(filename, begin, end TSRMLS_CC);
	}
}


static int check_evaled_code(function_stack_entry *fse, char **filename, int use_fse)
{
	char *end_marker;
	xdebug_eval_info *ei;
	char *filename_to_use;

	filename_to_use = use_fse ? fse->filename : *filename;

	end_marker = filename_to_use + strlen(filename_to_use) - strlen("eval()'d code");
	if (end_marker >= filename_to_use && strcmp("eval()'d code", end_marker) == 0) {
		if (xdebug_hash_find(XG(context).eval_id_lookup, filename_to_use, strlen(filename_to_use), (void *) &ei)) {
			*filename = xdebug_sprintf("dbgp://%lu", ei->id);
		}
		return 1;
	}
	return 0;
}

static xdebug_xml_node* return_stackframe(int nr TSRMLS_DC)
{
	function_stack_entry *fse, *fse_prev;
	char                 *tmp_fname;
	char                 *tmp_filename;
	xdebug_xml_node      *tmp;

	fse = xdebug_get_stack_frame(nr TSRMLS_CC);
	fse_prev = xdebug_get_stack_frame(nr - 1 TSRMLS_CC);

	tmp_fname = xdebug_show_fname(fse->function, 0, 0 TSRMLS_CC);

	tmp = xdebug_xml_node_init("stack");
	xdebug_xml_add_attribute_ex(tmp, "where", xdstrdup(tmp_fname), 0, 1);
	xdebug_xml_add_attribute_ex(tmp, "level", xdebug_sprintf("%ld", nr), 0, 1);
	if (fse_prev) {
		if (check_evaled_code(fse_prev, &tmp_filename, 1)) {
			xdebug_xml_add_attribute_ex(tmp, "type",     xdstrdup("eval"), 0, 1);
			xdebug_xml_add_attribute_ex(tmp, "filename", tmp_filename, 0, 0);
		} else {
			xdebug_xml_add_attribute_ex(tmp, "type",     xdstrdup("file"), 0, 1);
			xdebug_xml_add_attribute_ex(tmp, "filename", xdebug_path_to_url(fse_prev->filename TSRMLS_CC), 0, 1);
		}
		xdebug_xml_add_attribute_ex(tmp, "lineno",   xdebug_sprintf("%lu", fse_prev->lineno TSRMLS_CC), 0, 1);
	} else {
		int tmp_lineno;

		tmp_lineno = zend_get_executed_lineno(TSRMLS_C);
		tmp_filename = (char *) zend_get_executed_filename(TSRMLS_C);
		if (check_evaled_code(fse, &tmp_filename, 0)) {
			xdebug_xml_add_attribute_ex(tmp, "type", xdstrdup("eval"), 0, 1);
			xdebug_xml_add_attribute_ex(tmp, "filename", tmp_filename, 0, 0);
		} else {
			xdebug_xml_add_attribute_ex(tmp, "type", xdstrdup("file"), 0, 1);
			xdebug_xml_add_attribute_ex(tmp, "filename", xdebug_path_to_url(tmp_filename TSRMLS_CC), 0, 1);
		}
		xdebug_xml_add_attribute_ex(tmp, "lineno",   xdebug_sprintf("%lu", tmp_lineno), 0, 1);
	}

	xdfree(tmp_fname);
	return tmp;
}

/*****************************************************************************
** Client command handlers - Breakpoints
*/

/* Helper functions */
void xdebug_hash_admin_dtor(xdebug_brk_admin *admin)
{
	xdfree(admin->key);
	xdfree(admin);
}

static int breakpoint_admin_add(xdebug_con *context, int type, char *key)
{
	xdebug_brk_admin *admin = xdmalloc(sizeof(xdebug_brk_admin));
	char             *hkey;
	TSRMLS_FETCH();

	XG(breakpoint_count)++;
	admin->id   = ((xdebug_get_pid() & 0x1ffff) * 10000) + XG(breakpoint_count);
	admin->type = type;
	admin->key  = xdstrdup(key);

	hkey = xdebug_sprintf("%lu", admin->id);
	xdebug_hash_add(context->breakpoint_list, hkey, strlen(hkey), (void*) admin);
	xdfree(hkey);

	return admin->id;
}

static int breakpoint_admin_fetch(xdebug_con *context, char *hkey, int *type, char **key)
{
	xdebug_brk_admin *admin;

	if (xdebug_hash_find(context->breakpoint_list, hkey, strlen(hkey), (void *) &admin)) {
		*type = admin->type;
		*key  = admin->key;
		return SUCCESS;
	} else {
		return FAILURE;
	}

}

static int breakpoint_admin_remove(xdebug_con *context, char *hkey)
{
	if (xdebug_hash_delete(context->breakpoint_list, hkey, strlen(hkey))) {
		return SUCCESS;
	} else {
		return FAILURE;
	}
}

static void breakpoint_brk_info_add_resolved(xdebug_xml_node *xml, xdebug_brk_info *brk_info)
{
	if (!XG(context).resolved_breakpoints) {
		return;
	}
	if (brk_info->resolved == XDEBUG_BRK_RESOLVED) {
		xdebug_xml_add_attribute(xml, "resolved", "resolved");
	} else {
		xdebug_xml_add_attribute(xml, "resolved", "unresolved");
	}
}

static void breakpoint_brk_info_add(xdebug_xml_node *xml, xdebug_brk_info *brk_info)
{
	TSRMLS_FETCH();

	xdebug_xml_add_attribute_ex(xml, "type", xdstrdup(XDEBUG_BREAKPOINT_TYPE_NAME(brk_info->brk_type)), 0, 1);
	breakpoint_brk_info_add_resolved(xml, brk_info);
	if (brk_info->file) {
		if (is_dbgp_url(brk_info->file)) {
			xdebug_xml_add_attribute_ex(xml, "function", xdstrdup(brk_info->file), 0, 1);
	} else {
			xdebug_xml_add_attribute_ex(xml, "filename", xdebug_path_to_url(brk_info->file TSRMLS_CC), 0, 1);
		}
	}
	if (brk_info->resolved_lineno) {
		xdebug_xml_add_attribute_ex(xml, "lineno", xdebug_sprintf("%lu", brk_info->resolved_lineno), 0, 1);
	}
	if (brk_info->functionname) {
		xdebug_xml_add_attribute_ex(xml, "function", xdstrdup(brk_info->functionname), 0, 1);
	}
	if (brk_info->classname) {
		xdebug_xml_add_attribute_ex(xml, "class", xdstrdup(brk_info->classname), 0, 1);
	}
	if (brk_info->exceptionname) {
		xdebug_xml_add_attribute_ex(xml, "exception", xdstrdup(brk_info->exceptionname), 0, 1);
	}
	if (brk_info->temporary) {
		xdebug_xml_add_attribute(xml, "state", "temporary");
	} else if (brk_info->disabled) {
		xdebug_xml_add_attribute(xml, "state", "disabled");
	} else {
		xdebug_xml_add_attribute(xml, "state", "enabled");
	}
	xdebug_xml_add_attribute_ex(xml, "hit_count", xdebug_sprintf("%lu", brk_info->hit_count), 0, 1);
	switch (brk_info->hit_condition) {
		case XDEBUG_HIT_GREATER_EQUAL:
			xdebug_xml_add_attribute(xml, "hit_condition", ">=");
			break;
		case XDEBUG_HIT_EQUAL:
			xdebug_xml_add_attribute(xml, "hit_condition", "==");
			break;
		case XDEBUG_HIT_MOD:
			xdebug_xml_add_attribute(xml, "hit_condition", "%");
			break;
	}
	if (brk_info->condition) {
		xdebug_xml_node *condition = xdebug_xml_node_init("expression");
		xdebug_xml_add_text_ex(condition, brk_info->condition, strlen(brk_info->condition), 0, 1);
		xdebug_xml_add_child(xml, condition);
	}
	xdebug_xml_add_attribute_ex(xml, "hit_value", xdebug_sprintf("%lu", brk_info->hit_value), 0, 1);
	xdebug_xml_add_attribute_ex(xml, "id", xdebug_sprintf("%lu", brk_info->id), 0, 1);
}

static xdebug_brk_info* breakpoint_brk_info_fetch(int type, char *hkey)
{
	xdebug_llist_element *le;
	xdebug_brk_info      *brk_info = NULL;
	xdebug_arg           *parts = (xdebug_arg*) xdmalloc(sizeof(xdebug_arg));

	TSRMLS_FETCH();

	switch (type) {
		case XDEBUG_BREAKPOINT_TYPE_LINE:
		case XDEBUG_BREAKPOINT_TYPE_CONDITIONAL:
			/* First we split the key into filename and linenumber */
			xdebug_arg_init(parts);
			xdebug_explode("$", hkey, parts, -1);

			/* Second we loop through the list of file/line breakpoints to
			 * look for our thingy */
			for (le = XDEBUG_LLIST_HEAD(XG(context).line_breakpoints); le != NULL; le = XDEBUG_LLIST_NEXT(le)) {
				brk_info = XDEBUG_LLIST_VALP(le);

				if (atoi(parts->args[1]) == brk_info->original_lineno && memcmp(brk_info->file, parts->args[0], brk_info->file_len) == 0) {
					xdebug_arg_dtor(parts);
					return brk_info;
				}
			}

			/* Cleaning up */
			xdebug_arg_dtor(parts);
			break;

		case XDEBUG_BREAKPOINT_TYPE_CALL:
		case XDEBUG_BREAKPOINT_TYPE_RETURN:
			if (xdebug_hash_find(XG(context).function_breakpoints, hkey, strlen(hkey), (void *) &brk_info)) {
				return brk_info;
			}
			break;

		case XDEBUG_BREAKPOINT_TYPE_EXCEPTION:
			if (xdebug_hash_find(XG(context).exception_breakpoints, hkey, strlen(hkey), (void *) &brk_info)) {
				return brk_info;
			}
			break;
	}
	return brk_info;
}

static int breakpoint_remove(int type, char *hkey)
{
	xdebug_llist_element *le;
	xdebug_brk_info      *brk_info = NULL;
	xdebug_arg           *parts = (xdebug_arg*) xdmalloc(sizeof(xdebug_arg));
	int                   retval = FAILURE;
	TSRMLS_FETCH();

	switch (type) {
		case XDEBUG_BREAKPOINT_TYPE_LINE:
		case XDEBUG_BREAKPOINT_TYPE_CONDITIONAL:
			/* First we split the key into filename and linenumber */
			xdebug_arg_init(parts);
			xdebug_explode("$", hkey, parts, -1);

			/* Second we loop through the list of file/line breakpoints to
			 * look for our thingy */
			for (le = XDEBUG_LLIST_HEAD(XG(context).line_breakpoints); le != NULL; le = XDEBUG_LLIST_NEXT(le)) {
				brk_info = XDEBUG_LLIST_VALP(le);

				if (atoi(parts->args[1]) == brk_info->original_lineno && memcmp(brk_info->file, parts->args[0], brk_info->file_len) == 0) {
					xdebug_llist_remove(XG(context).line_breakpoints, le, NULL);
					retval = SUCCESS;
					break;
				}
			}

			/* Cleaning up */
			xdebug_arg_dtor(parts);
			break;

		case XDEBUG_BREAKPOINT_TYPE_CALL:
		case XDEBUG_BREAKPOINT_TYPE_RETURN:
			if (xdebug_hash_delete(XG(context).function_breakpoints, hkey, strlen(hkey))) {
				retval = SUCCESS;
			}
			break;

		case XDEBUG_BREAKPOINT_TYPE_EXCEPTION:
			if (xdebug_hash_delete(XG(context).exception_breakpoints, hkey, strlen(hkey))) {
				retval = SUCCESS;
			}
			break;
	}
	return retval;
}

#define BREAKPOINT_ACTION_GET       1
#define BREAKPOINT_ACTION_REMOVE    2
#define BREAKPOINT_ACTION_UPDATE    3

#define BREAKPOINT_CHANGE_STATE() \
	XDEBUG_STR_SWITCH(CMD_OPTION_CHAR('s')) { \
		XDEBUG_STR_CASE("enabled") \
			brk_info->disabled = 0; \
		XDEBUG_STR_CASE_END \
 \
		XDEBUG_STR_CASE("disabled") \
			brk_info->disabled = 1; \
		XDEBUG_STR_CASE_END \
 \
		XDEBUG_STR_CASE_DEFAULT \
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS); \
		XDEBUG_STR_CASE_DEFAULT_END \
	}

#define BREAKPOINT_CHANGE_OPERATOR() \
	XDEBUG_STR_SWITCH(CMD_OPTION_CHAR('o')) { \
		XDEBUG_STR_CASE(">=") \
			brk_info->hit_condition = XDEBUG_HIT_GREATER_EQUAL; \
		XDEBUG_STR_CASE_END \
 \
		XDEBUG_STR_CASE("==") \
			brk_info->hit_condition = XDEBUG_HIT_EQUAL; \
		XDEBUG_STR_CASE_END \
 \
		XDEBUG_STR_CASE("%") \
			brk_info->hit_condition = XDEBUG_HIT_MOD; \
		XDEBUG_STR_CASE_END \
 \
		XDEBUG_STR_CASE_DEFAULT \
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS); \
		XDEBUG_STR_CASE_DEFAULT_END \
	}



static void breakpoint_do_action(DBGP_FUNC_PARAMETERS, int action)
{
	int                   type;
	char                 *hkey;
	xdebug_brk_info      *brk_info;
	xdebug_xml_node      *breakpoint_node;
	XDEBUG_STR_SWITCH_DECL;

	if (!CMD_OPTION_SET('d')) {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
	}
	/* Lets check if it exists */
	if (breakpoint_admin_fetch(context, CMD_OPTION_CHAR('d'), &type, (char**) &hkey) == SUCCESS) {
		/* so it exists, now we're going to find it in the correct hash/list
		 * and return the info we have on it */
		brk_info = breakpoint_brk_info_fetch(type, hkey);

		if (action == BREAKPOINT_ACTION_UPDATE) {
			if (CMD_OPTION_SET('s')) {
				BREAKPOINT_CHANGE_STATE();
			}
			if (CMD_OPTION_SET('n')) {
				brk_info->original_lineno = strtol(CMD_OPTION_CHAR('n'), NULL, 10);
				brk_info->resolved_lineno = brk_info->original_lineno;
				brk_info->resolved_span.start = XDEBUG_RESOLVED_SPAN_MIN;
				brk_info->resolved_span.end   = XDEBUG_RESOLVED_SPAN_MAX;
			}
			if (CMD_OPTION_SET('h')) {
				brk_info->hit_value = strtol(CMD_OPTION_CHAR('h'), NULL, 10);
			}
			if (CMD_OPTION_SET('o')) {
				BREAKPOINT_CHANGE_OPERATOR();
			}
		}

		breakpoint_node = xdebug_xml_node_init("breakpoint");
		breakpoint_brk_info_add(breakpoint_node, brk_info);
		xdebug_xml_add_child(*retval, breakpoint_node);

		if (action == BREAKPOINT_ACTION_REMOVE) {
			/* Now we remove the crap */
			breakpoint_remove(type, hkey);
			breakpoint_admin_remove(context, CMD_OPTION_CHAR('d'));
		}
	} else {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_NO_SUCH_BREAKPOINT)
	}
}

DBGP_FUNC(breakpoint_get)
{
	breakpoint_do_action(DBGP_FUNC_PASS_PARAMETERS, BREAKPOINT_ACTION_GET);
}

DBGP_FUNC(breakpoint_remove)
{
	breakpoint_do_action(DBGP_FUNC_PASS_PARAMETERS, BREAKPOINT_ACTION_REMOVE);
}

DBGP_FUNC(breakpoint_update)
{
	breakpoint_do_action(DBGP_FUNC_PASS_PARAMETERS, BREAKPOINT_ACTION_UPDATE);
}


static void breakpoint_list_helper(void *xml, xdebug_hash_element *he)
{
	xdebug_xml_node  *xml_node = (xdebug_xml_node*) xml;
	xdebug_xml_node  *child;
	xdebug_brk_admin *admin = (xdebug_brk_admin*) he->ptr;
	xdebug_brk_info  *brk_info;

	child = xdebug_xml_node_init("breakpoint");
	brk_info = breakpoint_brk_info_fetch(admin->type, admin->key);
	breakpoint_brk_info_add(child, brk_info);
	xdebug_xml_add_child(xml_node, child);
}

DBGP_FUNC(breakpoint_list)
{
	xdebug_hash_apply(context->breakpoint_list, (void *) *retval, breakpoint_list_helper);
}

DBGP_FUNC(breakpoint_set)
{
	xdebug_brk_info      *brk_info;
	char                 *tmp_name;
	size_t                new_length = 0;
	XDEBUG_STR_SWITCH_DECL;

	brk_info = xdmalloc(sizeof(xdebug_brk_info));
	brk_info->id = -1;
	brk_info->brk_type = -1;
	brk_info->resolved = XDEBUG_BRK_UNRESOLVED;
	brk_info->file = NULL;
	brk_info->file_len = 0;
	brk_info->original_lineno = 0;
	brk_info->resolved_lineno = 0;
	brk_info->resolved_span.start = XDEBUG_RESOLVED_SPAN_MIN;
	brk_info->resolved_span.end   = XDEBUG_RESOLVED_SPAN_MAX;
	brk_info->classname = NULL;
	brk_info->functionname = NULL;
	brk_info->function_break_type = 0;
	brk_info->exceptionname = NULL;
	brk_info->condition = NULL;
	brk_info->disabled = 0;
	brk_info->temporary = 0;
	brk_info->hit_count = 0;
	brk_info->hit_value = 0;
	brk_info->hit_condition = XDEBUG_HIT_DISABLED;

	if (!CMD_OPTION_SET('t')) {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
	} else {
		int i;
		int found = 0;

		for (i = 0; i < XDEBUG_BREAKPOINT_TYPES_COUNT; i++) {
			if (strcmp(xdebug_breakpoint_types[i].name, CMD_OPTION_CHAR('t')) == 0) {
				brk_info->brk_type = xdebug_breakpoint_types[i].value;
				found = 1;
				break;
			}
		}

		if (!found) {
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
		}
	}

	if (CMD_OPTION_SET('s')) {
		BREAKPOINT_CHANGE_STATE();
		xdebug_xml_add_attribute_ex(*retval, "state", xdstrdup(CMD_OPTION_CHAR('s')), 0, 1);
	}
	if (CMD_OPTION_SET('o') && CMD_OPTION_SET('h')) {
		BREAKPOINT_CHANGE_OPERATOR();
		brk_info->hit_value = strtol(CMD_OPTION_CHAR('h'), NULL, 10);
	}
	if (CMD_OPTION_SET('r')) {
		brk_info->temporary = strtol(CMD_OPTION_CHAR('r'), NULL, 10);
	}

	if ((strcmp(CMD_OPTION_CHAR('t'), "line") == 0) || (strcmp(CMD_OPTION_CHAR('t'), "conditional") == 0)) {
		if (!CMD_OPTION_SET('n')) {
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
		}
		brk_info->original_lineno = strtol(CMD_OPTION_CHAR('n'), NULL, 10);
		brk_info->resolved_lineno = brk_info->original_lineno;
		brk_info->resolved_span.start = XDEBUG_RESOLVED_SPAN_MIN;
		brk_info->resolved_span.end   = XDEBUG_RESOLVED_SPAN_MAX;

		/* If no filename is given, we use the current one */
		if (!CMD_OPTION_SET('f')) {
			function_stack_entry *fse = xdebug_get_stack_tail(TSRMLS_C);

			if (!fse) {
				RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_STACK_DEPTH_INVALID);
			} else {
				brk_info->file = xdebug_path_from_url(fse->filename TSRMLS_CC);
				brk_info->file_len = strlen(brk_info->file);
			}
		} else {
			char realpath_file[MAXPATHLEN];

			brk_info->file = xdebug_path_from_url(CMD_OPTION_CHAR('f') TSRMLS_CC);

			/* Now we do some real path checks to resolve symlinks. */
			if (VCWD_REALPATH(brk_info->file, realpath_file)) {
				xdfree(brk_info->file);
				brk_info->file = xdstrdup(realpath_file);
			}

			brk_info->file_len = strlen(brk_info->file);
		}

		/* Perhaps we have a break condition */
		if (CMD_OPTION_SET('-')) {
			brk_info->condition = (char*) xdebug_base64_decode((unsigned char*) CMD_OPTION_CHAR('-'), CMD_OPTION_LEN('-'), &new_length);
		}

		tmp_name = xdebug_sprintf("%s$%lu", brk_info->file, brk_info->original_lineno);
		if (strcmp(CMD_OPTION_CHAR('t'), "line") == 0) {
			brk_info->id = breakpoint_admin_add(context, XDEBUG_BREAKPOINT_TYPE_LINE, tmp_name);
		} else {
			brk_info->id = breakpoint_admin_add(context, XDEBUG_BREAKPOINT_TYPE_CONDITIONAL, tmp_name);
		}
		xdfree(tmp_name);
		xdebug_llist_insert_next(context->line_breakpoints, XDEBUG_LLIST_TAIL(context->line_breakpoints), (void*) brk_info);

		if (XG(context).resolved_breakpoints) {
			function_stack_entry *fse = xdebug_get_stack_tail(TSRMLS_C);

			if (fse) {
				line_breakpoint_resolve_helper(context, fse, brk_info);
			}
		}
	} else

	if ((strcmp(CMD_OPTION_CHAR('t'), "call") == 0) || (strcmp(CMD_OPTION_CHAR('t'), "return") == 0)) {
		if (strcmp(CMD_OPTION_CHAR('t'), "call") == 0) {
			brk_info->function_break_type = XDEBUG_BREAKPOINT_TYPE_CALL;
		} else {
			brk_info->function_break_type = XDEBUG_BREAKPOINT_TYPE_RETURN;
		}

		if (!CMD_OPTION_SET('m')) {
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
		}
		brk_info->functionname = xdstrdup(CMD_OPTION_CHAR('m'));
		if (CMD_OPTION_SET('a')) {
			int   res;

			brk_info->classname = xdstrdup(CMD_OPTION_CHAR('a'));
			tmp_name = xdebug_sprintf("%s::%s", CMD_OPTION_CHAR('a'), CMD_OPTION_CHAR('m'));
			res = xdebug_hash_add(context->function_breakpoints, tmp_name, strlen(tmp_name), (void*) brk_info);
			if (brk_info->function_break_type == XDEBUG_BREAKPOINT_TYPE_CALL) {
				brk_info->id = breakpoint_admin_add(context, XDEBUG_BREAKPOINT_TYPE_CALL, tmp_name);
			} else {
				brk_info->id = breakpoint_admin_add(context, XDEBUG_BREAKPOINT_TYPE_RETURN, tmp_name);
			}
			xdfree(tmp_name);

			if (!res) {
				RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
			}
		} else {
			if (!xdebug_hash_add(context->function_breakpoints, CMD_OPTION_CHAR('m'), CMD_OPTION_LEN('m'), (void*) brk_info)) {
				RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_BREAKPOINT_NOT_SET);
			} else {
				if (brk_info->function_break_type == XDEBUG_BREAKPOINT_TYPE_CALL) {
					brk_info->id = breakpoint_admin_add(context, XDEBUG_BREAKPOINT_TYPE_CALL, CMD_OPTION_CHAR('m'));
				} else {
					brk_info->id = breakpoint_admin_add(context, XDEBUG_BREAKPOINT_TYPE_RETURN, CMD_OPTION_CHAR('m'));
				}
			}
		}
	} else

	if (strcmp(CMD_OPTION_CHAR('t'), "exception") == 0) {
		if (!CMD_OPTION_SET('x')) {
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
		}
		brk_info->exceptionname = xdstrdup(CMD_OPTION_CHAR('x'));
		if (!xdebug_hash_add(context->exception_breakpoints, CMD_OPTION_CHAR('x'), CMD_OPTION_LEN('x'), (void*) brk_info)) {
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_BREAKPOINT_NOT_SET);
		} else {
			brk_info->id = breakpoint_admin_add(context, XDEBUG_BREAKPOINT_TYPE_EXCEPTION, CMD_OPTION_CHAR('x'));
		}
	} else

	if (strcmp(CMD_OPTION_CHAR('t'), "watch") == 0) {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_BREAKPOINT_TYPE_NOT_SUPPORTED);
	}

	xdebug_xml_add_attribute_ex(*retval, "id", xdebug_sprintf("%lu", brk_info->id), 0, 1);
	breakpoint_brk_info_add_resolved(*retval, brk_info);
}

static int xdebug_do_eval(char *eval_string, zval *ret_zval TSRMLS_DC)
{
	int                old_track_errors;
	int                res = FAILURE;
	zend_execute_data *original_execute_data = EG(current_execute_data);
	int                original_no_extensions = EG(no_extensions);
	zend_object       *original_exception = EG(exception);
	jmp_buf           *original_bailout = EG(bailout);

	/* Remember error reporting level and track errors */
	XG(error_reporting_override) = EG(error_reporting);
	XG(error_reporting_overridden) = 1;
	old_track_errors = PG(track_errors);
	EG(error_reporting) = 0;
	PG(track_errors) = 0;

	/* Do evaluation */
	XG(breakpoints_allowed) = 0;

	/* Reset exception in case we're triggered while being in xdebug_throw_exception_hook */
	EG(exception) = NULL;

	zend_first_try {
		res = zend_eval_string(eval_string, ret_zval, (char*) "xdebug://debug-eval" TSRMLS_CC);
	} zend_end_try();

	/* FIXME: Bubble up exception message to DBGp return packet */
	if (EG(exception)) {
		res = FAILURE;
	}

	/* Clean up */
	EG(error_reporting) = XG(error_reporting_override);
	XG(error_reporting_overridden) = 0;
	PG(track_errors) = old_track_errors;
	XG(breakpoints_allowed) = 1;

	EG(current_execute_data) = original_execute_data;
	EG(no_extensions) = original_no_extensions;
	EG(exception) = original_exception;
	EG(bailout) = original_bailout;

	return res;
}

DBGP_FUNC(eval)
{
	char            *eval_string;
	xdebug_xml_node *ret_xml;
	zval             ret_zval;
	size_t           new_length = 0;
	int              res;
	xdebug_var_export_options *options;

	if (!CMD_OPTION_SET('-')) {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
	}

	options = (xdebug_var_export_options*) context->options;

	if (CMD_OPTION_SET('p')) {
		options->runtime[0].page = strtol(CMD_OPTION_CHAR('p'), NULL, 10);
	} else {
		options->runtime[0].page = 0;
	}

	/* base64 decode eval string */
	eval_string = (char*) xdebug_base64_decode((unsigned char*) CMD_OPTION_CHAR('-'), CMD_OPTION_LEN('-'), &new_length);

	res = xdebug_do_eval(eval_string, &ret_zval TSRMLS_CC);

	xdfree(eval_string);

	/* Handle result */
	if (res == FAILURE) {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_EVALUATING_CODE);
	} else {
		ret_xml = xdebug_get_zval_value_xml_node(NULL, &ret_zval, options TSRMLS_CC);
		xdebug_xml_add_child(*retval, ret_xml);
		zval_ptr_dtor(&ret_zval);
	}
}

/* these functions interupt PHP's output functions, so we can
   redirect to our remote debugger! */
static void xdebug_send_stream(const char *name, const char *str, unsigned int str_length TSRMLS_DC)
{
	/* create an xml document to send as the stream */
	xdebug_xml_node *message;

	if (!xdebug_is_debug_connection_active()) {
		return;
	}

	message = xdebug_xml_node_init("stream");
	xdebug_xml_add_attribute(message, "xmlns", "urn:debugger_protocol_v1");
	xdebug_xml_add_attribute(message, "xmlns:xdebug", "https://xdebug.org/dbgp/xdebug");
	xdebug_xml_add_attribute_ex(message, "type", (char *)name, 0, 0);
	xdebug_xml_add_text_encodel(message, xdstrndup(str, str_length), str_length);
	send_message(&XG(context), message TSRMLS_CC);
	xdebug_xml_node_dtor(message);

	return;
}


DBGP_FUNC(stderr)
{
	xdebug_xml_add_attribute(*retval, "success", "0");
}

DBGP_FUNC(stdout)
{
	int mode = 0;
	const char *success = "0";

	if (!CMD_OPTION_SET('c')) {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
	}

	mode = strtol(CMD_OPTION_CHAR('c'), NULL, 10);
	XG(stdout_mode) = mode;
	success = "1";

	xdebug_xml_add_attribute_ex(*retval, "success", xdstrdup(success), 0, 1);
}

DBGP_FUNC(stop)
{
	XG(status) = DBGP_STATUS_STOPPED;
	xdebug_xml_add_attribute(*retval, "status", xdebug_dbgp_status_strings[XG(status)]);
	xdebug_xml_add_attribute(*retval, "reason", xdebug_dbgp_reason_strings[XG(reason)]);
}

DBGP_FUNC(run)
{
	xdebug_xml_add_attribute_ex(*retval, "filename", xdstrdup(context->program_name), 0, 1);
}

DBGP_FUNC(step_into)
{
	XG(context).do_next   = 0;
	XG(context).do_step   = 1;
	XG(context).do_finish = 0;
}

DBGP_FUNC(step_out)
{
	function_stack_entry *fse;

	XG(context).do_next   = 0;
	XG(context).do_step   = 0;
	XG(context).do_finish = 1;

	if ((fse = xdebug_get_stack_tail(TSRMLS_C))) {
		XG(context).finish_level = fse->level;
		XG(context).finish_func_nr = fse->function_nr;
	} else {
		XG(context).finish_level = -1;
		XG(context).finish_func_nr = -1;
	}
}

DBGP_FUNC(step_over)
{
	function_stack_entry *fse;

	XG(context).do_next   = 1;
	XG(context).do_step   = 0;
	XG(context).do_finish = 0;

	if ((fse = xdebug_get_stack_tail(TSRMLS_C))) {
		XG(context).next_level = fse->level;
	} else {
		XG(context).next_level = 0;
	}
}

DBGP_FUNC(detach)
{
	XG(status) = DBGP_STATUS_DETACHED;
	xdebug_xml_add_attribute(*retval, "status", xdebug_dbgp_status_strings[DBGP_STATUS_STOPPED]);
	xdebug_xml_add_attribute(*retval, "reason", xdebug_dbgp_reason_strings[XG(reason)]);
	XG(context).handler->remote_deinit(&(XG(context)));
	xdebug_mark_debug_connection_not_active();
	XG(stdout_mode) = 0;
	XG(remote_enable) = 0;
}


DBGP_FUNC(source)
{
	xdebug_str *source;
	int   begin = 0, end = 999999;
	char *filename;
	function_stack_entry *fse;

	if (!CMD_OPTION_SET('f')) {
		if ((fse = xdebug_get_stack_tail(TSRMLS_C))) {
			filename = fse->filename;
		} else {
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_STACK_DEPTH_INVALID);
		}
	} else {
		filename = CMD_OPTION_CHAR('f');
	}

	if (CMD_OPTION_SET('b')) {
		begin = strtol(CMD_OPTION_CHAR('b'), NULL, 10);
	}
	if (CMD_OPTION_SET('e')) {
		end = strtol(CMD_OPTION_CHAR('e'), NULL, 10);
	}

	/* return_source allocates memory for source */
	XG(breakpoints_allowed) = 0;
	source = return_source(filename, begin, end TSRMLS_CC);
	XG(breakpoints_allowed) = 1;

	if (!source) {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_CANT_OPEN_FILE);
	} else {
		xdebug_xml_add_text_ex(*retval, xdstrdup(source->d), source->l, 1, 1);
		xdebug_str_free(source);
	}
}

DBGP_FUNC(feature_get)
{
	xdebug_var_export_options *options;
	XDEBUG_STR_SWITCH_DECL;

	options = (xdebug_var_export_options*) context->options;

	if (!CMD_OPTION_SET('n')) {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
	}
	xdebug_xml_add_attribute_ex(*retval, "feature_name", xdstrdup(CMD_OPTION_CHAR('n')), 0, 1);

	XDEBUG_STR_SWITCH(CMD_OPTION_CHAR('n')) {
		XDEBUG_STR_CASE("breakpoint_languages")
			xdebug_xml_add_attribute(*retval, "supported", "0");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("breakpoint_types")
			xdebug_xml_add_text(*retval, xdstrdup("line conditional call return exception"));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("data_encoding")
			xdebug_xml_add_attribute(*retval, "supported", "0");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("encoding")
			xdebug_xml_add_text(*retval, xdstrdup("iso-8859-1"));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("language_name")
			xdebug_xml_add_text(*retval, xdstrdup("PHP"));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("language_supports_threads")
			xdebug_xml_add_text(*retval, xdstrdup("0"));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("language_version")
			xdebug_xml_add_text(*retval, xdstrdup(PHP_VERSION));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("max_children")
			xdebug_xml_add_text(*retval, xdebug_sprintf("%ld", options->max_children));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("max_data")
			xdebug_xml_add_text(*retval, xdebug_sprintf("%ld", options->max_data));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("max_depth")
			xdebug_xml_add_text(*retval, xdebug_sprintf("%ld", options->max_depth));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("protocol_version")
			xdebug_xml_add_text(*retval, xdstrdup(DBGP_VERSION));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("supported_encodings")
			xdebug_xml_add_text(*retval, xdstrdup("iso-8859-1"));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("supports_async")
			xdebug_xml_add_text(*retval, xdstrdup("0"));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("supports_postmortem")
			xdebug_xml_add_text(*retval, xdstrdup("1"));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("show_hidden")
			xdebug_xml_add_text(*retval, xdebug_sprintf("%ld", options->show_hidden));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("extended_properties")
			xdebug_xml_add_text(*retval, xdebug_sprintf("%ld", options->extended_properties));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("notify_ok")
			xdebug_xml_add_text(*retval, xdebug_sprintf("%ld", XG(context).send_notifications));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("resolved_breakpoints")
			xdebug_xml_add_text(*retval, xdebug_sprintf("%ld", XG(context).resolved_breakpoints));
			xdebug_xml_add_attribute(*retval, "supported", "1");
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE_DEFAULT
			xdebug_xml_add_text(*retval, xdstrdup(lookup_cmd(CMD_OPTION_CHAR('n')) ? "1" : "0"));
			xdebug_xml_add_attribute(*retval, "supported", lookup_cmd(CMD_OPTION_CHAR('n')) ? "1" : "0");
		XDEBUG_STR_CASE_DEFAULT_END
	}
}

DBGP_FUNC(feature_set)
{
	xdebug_var_export_options *options;
	XDEBUG_STR_SWITCH_DECL;

	options = (xdebug_var_export_options*) context->options;

	if (!CMD_OPTION_SET('n') || !CMD_OPTION_SET('v')) {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
	}

	XDEBUG_STR_SWITCH(CMD_OPTION_CHAR('n')) {

		XDEBUG_STR_CASE("encoding")
			if (strcmp(CMD_OPTION_CHAR('v'), "iso-8859-1") != 0) {
				RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_ENCODING_NOT_SUPPORTED);
			}
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("max_children")
			options->max_children = strtol(CMD_OPTION_CHAR('v'), NULL, 10);
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("max_data")
			options->max_data = strtol(CMD_OPTION_CHAR('v'), NULL, 10);
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("max_depth")
			int i;
			options->max_depth = strtol(CMD_OPTION_CHAR('v'), NULL, 10);

			/* Reallocating page structure */
			xdfree(options->runtime);
			options->runtime = (xdebug_var_runtime_page*) xdmalloc(options->max_depth * sizeof(xdebug_var_runtime_page));
			for (i = 0; i < options->max_depth; i++) {
				options->runtime[i].page = 0;
				options->runtime[i].current_element_nr = 0;
			}
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("show_hidden")
			options->show_hidden = strtol(CMD_OPTION_CHAR('v'), NULL, 10);
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("multiple_sessions")
			/* FIXME: Add new boolean option check / struct field for this */
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("extended_properties")
			options->extended_properties = strtol(CMD_OPTION_CHAR('v'), NULL, 10);
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("notify_ok")
			XG(context).send_notifications = strtol(CMD_OPTION_CHAR('v'), NULL, 10);
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE("resolved_breakpoints")
			XG(context).resolved_breakpoints = strtol(CMD_OPTION_CHAR('v'), NULL, 10);
		XDEBUG_STR_CASE_END

		XDEBUG_STR_CASE_DEFAULT
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
		XDEBUG_STR_CASE_DEFAULT_END
	}
	xdebug_xml_add_attribute_ex(*retval, "feature", xdstrdup(CMD_OPTION_CHAR('n')), 0, 1);
	xdebug_xml_add_attribute_ex(*retval, "success", "1", 0, 0);
}

DBGP_FUNC(typemap_get)
{
	int              i;
	xdebug_xml_node *type;

	xdebug_xml_add_attribute(*retval, "xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance");
	xdebug_xml_add_attribute(*retval, "xmlns:xsd", "http://www.w3.org/2001/XMLSchema");

	/* Add our basic types */
	for (i = 0; i < XDEBUG_TYPES_COUNT; i++) {
		type = xdebug_xml_node_init("map");
		xdebug_xml_add_attribute(type, "name", xdebug_dbgp_typemap[i][1]);
		xdebug_xml_add_attribute(type, "type", xdebug_dbgp_typemap[i][0]);
		if (xdebug_dbgp_typemap[i][2]) {
			xdebug_xml_add_attribute(type, "xsi:type", xdebug_dbgp_typemap[i][2]);
		}
		xdebug_xml_add_child(*retval, type);
	}
}

static int add_constant_node(xdebug_xml_node *node, xdebug_str *name, zval *const_val, xdebug_var_export_options *options TSRMLS_DC)
{
	xdebug_xml_node *contents;

	contents = xdebug_get_zval_value_xml_node_ex(name, const_val, XDEBUG_VAR_TYPE_CONSTANT, options);
	if (contents) {
		xdebug_xml_add_attribute(contents, "facet", "constant");
		xdebug_xml_add_child(node, contents);
		return SUCCESS;
	}
	return FAILURE;
}

static int add_variable_node(xdebug_xml_node *node, xdebug_str *name, int var_only, int non_null, int no_eval, xdebug_var_export_options *options TSRMLS_DC)
{
	xdebug_xml_node *contents;

	contents = get_symbol(name, options);
	if (contents) {
		xdebug_xml_add_child(node, contents);
		return SUCCESS;
	}
	return FAILURE;
}


DBGP_FUNC(property_get)
{
	int                        depth = 0;
	int                        context_nr = 0;
	function_stack_entry      *fse;
	int                        old_max_data;
	xdebug_var_export_options *options = (xdebug_var_export_options*) context->options;

	if (!CMD_OPTION_SET('n')) {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
	}

	if (CMD_OPTION_SET('d')) {
		depth = strtol(CMD_OPTION_CHAR('d'), NULL, 10);
	}

	if (CMD_OPTION_SET('c')) {
		context_nr = strtol(CMD_OPTION_CHAR('c'), NULL, 10);
	}

	/* Set the symbol table corresponding with the requested stack depth */
	if (context_nr == 0) { /* locals */
		if ((fse = xdebug_get_stack_frame(depth TSRMLS_CC))) {
			function_stack_entry *old_fse = xdebug_get_stack_frame(depth - 1 TSRMLS_CC);

			if (depth > 0) {
				XG(active_execute_data) = old_fse->execute_data;
			} else {
				XG(active_execute_data) = EG(current_execute_data);
			}
			XG(active_symbol_table) = fse->symbol_table;
			XG(This)                = fse->This;
			XG(active_fse)          = fse;
		} else {
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_STACK_DEPTH_INVALID);
		}
	} else if (context_nr == 1) { /* superglobals */
		XG(active_symbol_table) = &EG(symbol_table);
	} else if (context_nr == 2) { /* constants */
		/* Do nothing */
	} else {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
	}

	if (CMD_OPTION_SET('p')) {
		options->runtime[0].page = strtol(CMD_OPTION_CHAR('p'), NULL, 10);
	} else {
		options->runtime[0].page = 0;
	}

	/* Override max data size if necessary */
	old_max_data = options->max_data;
	if (CMD_OPTION_SET('m')) {
		options->max_data= strtol(CMD_OPTION_CHAR('m'), NULL, 10);
	}

	if (context_nr == 2) { /* constants */
		zval const_val;

		if (!xdebug_get_constant(CMD_OPTION_XDEBUG_STR('n'), &const_val TSRMLS_CC)) {
			options->max_data = old_max_data;
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_PROPERTY_NON_EXISTENT);
		}
		if (add_constant_node(*retval, CMD_OPTION_XDEBUG_STR('n'), &const_val, options TSRMLS_CC) == FAILURE) {
			options->max_data = old_max_data;
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_PROPERTY_NON_EXISTENT);
		}
	} else {
		int add_var_retval;

		XG(context).inhibit_notifications = 1;
		add_var_retval = add_variable_node(*retval, CMD_OPTION_XDEBUG_STR('n'), 1, 0, 0, options TSRMLS_CC);
		XG(context).inhibit_notifications = 0;

		if (add_var_retval) {
			options->max_data = old_max_data;
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_PROPERTY_NON_EXISTENT);
		}
	}
	options->max_data = old_max_data;
}

static void set_vars_from_EG(TSRMLS_D)
{
}

DBGP_FUNC(property_set)
{
	unsigned char             *new_value;
	size_t                     new_length = 0;
	int                        depth = 0;
	int                        context_nr = 0;
	int                        res;
	char                      *eval_string;
	const char                *cast_as;
	zval                       ret_zval;
	function_stack_entry      *fse;
	xdebug_var_export_options *options = (xdebug_var_export_options*) context->options;
	zend_execute_data         *original_execute_data;
	XDEBUG_STR_SWITCH_DECL;

	if (!CMD_OPTION_SET('n')) { /* name */
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
	}

	if (!CMD_OPTION_SET('-')) {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
	}

	if (CMD_OPTION_SET('d')) { /* depth */
		depth = strtol(CMD_OPTION_CHAR('d'), NULL, 10);
	}

	if (CMD_OPTION_SET('c')) { /* context_id */
		context_nr = strtol(CMD_OPTION_CHAR('c'), NULL, 10);
	}

	/* Set the symbol table corresponding with the requested stack depth */
	if (context_nr == 0) { /* locals */
		if ((fse = xdebug_get_stack_frame(depth TSRMLS_CC))) {
			function_stack_entry *old_fse = xdebug_get_stack_frame(depth - 1 TSRMLS_CC);

			if (depth > 0) {
				XG(active_execute_data) = old_fse->execute_data;
			} else {
				XG(active_execute_data) = EG(current_execute_data);
			}
			XG(active_symbol_table) = fse->symbol_table;
			XG(This)                = fse->This;
			XG(active_fse)          = fse;
		} else {
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_STACK_DEPTH_INVALID);
		}
	} else { /* superglobals */
		XG(active_symbol_table) = &EG(symbol_table);
	}

	if (CMD_OPTION_SET('p')) {
		options->runtime[0].page = strtol(CMD_OPTION_CHAR('p'), NULL, 10);
	} else {
		options->runtime[0].page = 0;
	}

	new_value = xdebug_base64_decode((unsigned char*) CMD_OPTION_CHAR('-'), CMD_OPTION_LEN('-'), &new_length);

	/* Set a cast, if requested through the 't' option */
	cast_as = "";

	if (CMD_OPTION_SET('t')) {
		XDEBUG_STR_SWITCH(CMD_OPTION_CHAR('t')) {
			XDEBUG_STR_CASE("bool")
				cast_as = "(bool) ";
			XDEBUG_STR_CASE_END

			XDEBUG_STR_CASE("int")
				cast_as = "(int) ";
			XDEBUG_STR_CASE_END

			XDEBUG_STR_CASE("float")
				cast_as = "(float) ";
			XDEBUG_STR_CASE_END

			XDEBUG_STR_CASE("string")
				cast_as = "(string) ";
			XDEBUG_STR_CASE_END

			XDEBUG_STR_CASE_DEFAULT
				xdebug_xml_add_attribute(*retval, "success", "0");
			XDEBUG_STR_CASE_DEFAULT_END
		}
	}

	/* backup executor state */
	if (depth > 0) {
		original_execute_data = EG(current_execute_data);

		EG(current_execute_data) = XG(active_execute_data);
		set_vars_from_EG(TSRMLS_C);
	}

	/* Do the eval */
	eval_string = xdebug_sprintf("%s = %s %s", CMD_OPTION_CHAR('n'), cast_as, new_value);
	res = xdebug_do_eval(eval_string, &ret_zval TSRMLS_CC);

	/* restore executor state */
	if (depth > 0) {
		EG(current_execute_data) = original_execute_data;
		set_vars_from_EG(TSRMLS_C);
	}

	/* Free data */
	xdfree(eval_string);
	xdfree(new_value);

	/* Handle result */
	if (res == FAILURE) {
		/* don't send an error, send success = zero */
		xdebug_xml_add_attribute(*retval, "success", "0");
	} else {
		zval_dtor(&ret_zval);
		xdebug_xml_add_attribute(*retval, "success", "1");
	}
}

static int add_variable_contents_node(xdebug_xml_node *node, xdebug_str *name, int var_only, int non_null, int no_eval, xdebug_var_export_options *options TSRMLS_DC)
{
	int contents_found;

	contents_found = get_symbol_contents(name, node, options);
	if (contents_found) {
		return SUCCESS;
	}
	return FAILURE;
}

DBGP_FUNC(property_value)
{
	int                        depth = 0;
	int                        context_nr = 0;
	function_stack_entry      *fse;
	int                        old_max_data;
	xdebug_var_export_options *options = (xdebug_var_export_options*) context->options;

	if (!CMD_OPTION_SET('n')) {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
	}

	if (CMD_OPTION_SET('d')) {
		depth = strtol(CMD_OPTION_CHAR('d'), NULL, 10);
	}

	if (CMD_OPTION_SET('c')) {
		context_nr = strtol(CMD_OPTION_CHAR('c'), NULL, 10);
	}

	/* Set the symbol table corresponding with the requested stack depth */
	if (context_nr == 0) { /* locals */
		if ((fse = xdebug_get_stack_frame(depth TSRMLS_CC))) {
			function_stack_entry *old_fse = xdebug_get_stack_frame(depth - 1 TSRMLS_CC);

			if (depth > 0) {
				XG(active_execute_data) = old_fse->execute_data;
			} else {
				XG(active_execute_data) = EG(current_execute_data);
			}
			XG(active_symbol_table) = fse->symbol_table;
			XG(This)                = fse->This;
			XG(active_fse)          = fse;
		} else {
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_STACK_DEPTH_INVALID);
		}
	} else { /* superglobals */
		XG(active_symbol_table) = &EG(symbol_table);
	}

	if (CMD_OPTION_SET('p')) {
		options->runtime[0].page = strtol(CMD_OPTION_CHAR('p'), NULL, 10);
	} else {
		options->runtime[0].page = 0;
	}

	/* Override max data size if necessary */
	old_max_data = options->max_data;
	if (CMD_OPTION_SET('m')) {
		options->max_data = strtol(CMD_OPTION_CHAR('m'), NULL, 10);
	}
	if (options->max_data < 0) {
		options->max_data = old_max_data;
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
	}
	if (add_variable_contents_node(*retval, CMD_OPTION_XDEBUG_STR('n'), 1, 0, 0, options TSRMLS_CC) == FAILURE) {
		options->max_data = old_max_data;
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_PROPERTY_NON_EXISTENT);
	}
	options->max_data = old_max_data;
}

static void attach_declared_var_with_contents(void *xml, xdebug_hash_element* he, void *options)
{
	xdebug_str         *name = (xdebug_str*) he->ptr;
	xdebug_xml_node    *node = (xdebug_xml_node *) xml;
	xdebug_xml_node    *contents;
	TSRMLS_FETCH();

	contents = get_symbol(name, options);
	if (contents) {
		xdebug_xml_add_child(node, contents);
	} else {
		xdebug_attach_uninitialized_var(options, node, name);
	}
}

# define HASH_KEY_VAL(k) (k)->key->val
# define HASH_KEY_LEN(k) (k)->key->len

static int xdebug_add_filtered_symboltable_var(zval *symbol TSRMLS_DC, int num_args, va_list args, zend_hash_key *hash_key)
{
	xdebug_hash *tmp_hash;

	tmp_hash = va_arg(args, xdebug_hash *);

	/* We really ought to deal properly with non-associate keys for symbol
	 * tables, but for now, we'll just ignore them. */
	if (!hash_key->key) { return 0; }

	if (!HASH_KEY_VAL(hash_key) || HASH_KEY_LEN(hash_key) == 0) { return 0; }

	if (strcmp("argc", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
	if (strcmp("argv", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
	if (HASH_KEY_VAL(hash_key)[0] == '_') {
		if (strcmp("_COOKIE", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
		if (strcmp("_ENV", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
		if (strcmp("_FILES", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
		if (strcmp("_GET", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
		if (strcmp("_POST", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
		if (strcmp("_REQUEST", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
		if (strcmp("_SERVER", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
		if (strcmp("_SESSION", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
	}
	if (HASH_KEY_VAL(hash_key)[0] == 'H') {
		if (strcmp("HTTP_COOKIE_VARS", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
		if (strcmp("HTTP_ENV_VARS", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
		if (strcmp("HTTP_GET_VARS", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
		if (strcmp("HTTP_POST_VARS", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
		if (strcmp("HTTP_POST_FILES", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
		if (strcmp("HTTP_RAW_POST_DATA", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
		if (strcmp("HTTP_SERVER_VARS", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
		if (strcmp("HTTP_SESSION_VARS", HASH_KEY_VAL(hash_key)) == 0) { return 0; }
	}
	if (strcmp("GLOBALS", HASH_KEY_VAL(hash_key)) == 0) { return 0; }

	xdebug_hash_add(tmp_hash, (char*) HASH_KEY_VAL(hash_key), HASH_KEY_LEN(hash_key), xdebug_str_create(HASH_KEY_VAL(hash_key), HASH_KEY_LEN(hash_key)));

	return 0;
}

#undef HASH_KEY_VAL
#undef HASH_KEY_LEN

static int attach_context_vars(xdebug_xml_node *node, xdebug_var_export_options *options, long context_id, long depth, void (*func)(void *, xdebug_hash_element*, void*) TSRMLS_DC)
{
	function_stack_entry *fse;
	char                 *var_name;

	/* right now, we only have zero, one, or two with one being globals, which
	 * is always the head of the stack */
	if (context_id == 1) {
		/* add super globals */
		XG(active_symbol_table) = &EG(symbol_table);
		XG(active_execute_data) = NULL;
		add_variable_node(node, XDEBUG_STR_WRAP_CHAR("_COOKIE"),  1, 1, 0, options);
		add_variable_node(node, XDEBUG_STR_WRAP_CHAR("_ENV"),     1, 1, 0, options);
		add_variable_node(node, XDEBUG_STR_WRAP_CHAR("_FILES"),   1, 1, 0, options);
		add_variable_node(node, XDEBUG_STR_WRAP_CHAR("_GET"),     1, 1, 0, options);
		add_variable_node(node, XDEBUG_STR_WRAP_CHAR("_POST"),    1, 1, 0, options);
		add_variable_node(node, XDEBUG_STR_WRAP_CHAR("_REQUEST"), 1, 1, 0, options);
		add_variable_node(node, XDEBUG_STR_WRAP_CHAR("_SERVER"),  1, 1, 0, options);
		add_variable_node(node, XDEBUG_STR_WRAP_CHAR("_SESSION"), 1, 1, 0, options);
		add_variable_node(node, XDEBUG_STR_WRAP_CHAR("GLOBALS"),  1, 1, 0, options);
		XG(active_symbol_table) = NULL;
		return 0;
	}

	/* add user defined constants */
	if (context_id == 2) {
		zend_constant *val;

		ZEND_HASH_FOREACH_PTR(EG(zend_constants), val) {
			xdebug_str *tmp_name;

			if (!val->name) {
				/* skip special constants */
				continue;
			}

			if (XDEBUG_ZEND_CONSTANT_MODULE_NUMBER(val) != PHP_USER_CONSTANT) {
				/* we're only interested in user defined constants */
				continue;
			}

			tmp_name = xdebug_str_create(val->name->val, val->name->len);
			add_constant_node(node, tmp_name, &(val->value), options TSRMLS_CC);
			xdebug_str_free(tmp_name);
		} ZEND_HASH_FOREACH_END();

		return 0;
	}

	/* Here the context_id is 0 */
	if ((fse = xdebug_get_stack_frame(depth TSRMLS_CC))) {
		function_stack_entry *old_fse = xdebug_get_stack_frame(depth - 1 TSRMLS_CC);

		if (depth > 0) {
			XG(active_execute_data) = old_fse->execute_data;
		} else {
			XG(active_execute_data) = EG(current_execute_data);
		}
		XG(active_symbol_table) = fse->symbol_table;
		XG(This)                = fse->This;

		/* Only show vars when they are scanned */
		if (fse->declared_vars) {
			xdebug_hash *tmp_hash;

			/* Get a hash from all the used vars (which can have duplicates) */
			tmp_hash = xdebug_declared_var_hash_from_llist(fse->declared_vars);

			/* Check for dynamically defined variables, but make sure we don't already
			 * have them. Also blacklist superglobals and argv/argc */
			if (XG(active_symbol_table)) {
				zend_hash_apply_with_arguments(XG(active_symbol_table) TSRMLS_CC, (apply_func_args_t) xdebug_add_filtered_symboltable_var, 1, tmp_hash);
			}

			/* Add all the found variables to the node */
			xdebug_hash_apply_with_argument(tmp_hash, (void *) node, func, (void *) options);

			/* Zend engine 2 does not give us $this, eval so we can get it */
			if (!xdebug_hash_find(tmp_hash, "this", 4, (void *) &var_name)) {
				add_variable_node(node, XDEBUG_STR_WRAP_CHAR("this"), 1, 1, 0, options TSRMLS_CC);
			}

			xdebug_hash_destroy(tmp_hash);
		}

		/* Check for static variables and constants, but only if it's a static
		 * method call as we attach constants and static properties to "this"
		 * too normally. */
		if (fse->function.type == XFUNC_STATIC_MEMBER) {
			zend_class_entry *ce = xdebug_fetch_class(fse->function.class, strlen(fse->function.class), ZEND_FETCH_CLASS_DEFAULT TSRMLS_CC);

#if PHP_VERSION_ID >= 70400
			if (ce->type == ZEND_INTERNAL_CLASS || (ce->ce_flags & ZEND_ACC_IMMUTABLE)) {
				zend_class_init_statics(ce);
			}
#endif

			xdebug_attach_static_vars(node, options, ce TSRMLS_CC);
		}

		XG(active_symbol_table) = NULL;
		XG(active_execute_data) = NULL;
		XG(This)                = NULL;
		return 0;
	}

	return 1;
}

DBGP_FUNC(stack_depth)
{
	xdebug_xml_add_attribute_ex(*retval, "depth", xdebug_sprintf("%lu", XG(level)), 0, 1);
}

DBGP_FUNC(stack_get)
{
	xdebug_xml_node      *stackframe;
	xdebug_llist_element *le;
	int                   counter = 0;
	long                  depth;

	if (CMD_OPTION_SET('d')) {
		depth = strtol(CMD_OPTION_CHAR('d'), NULL, 10);
		if (depth >= 0 && depth < (long) XG(level)) {
			stackframe = return_stackframe(depth TSRMLS_CC);
			xdebug_xml_add_child(*retval, stackframe);
		} else {
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_STACK_DEPTH_INVALID);
		}
	} else {
		counter = 0;
		for (le = XDEBUG_LLIST_TAIL(XG(stack)); le != NULL; le = XDEBUG_LLIST_PREV(le)) {
			stackframe = return_stackframe(counter TSRMLS_CC);
			xdebug_xml_add_child(*retval, stackframe);
			counter++;
		}
	}
}

DBGP_FUNC(status)
{
	xdebug_xml_add_attribute(*retval, "status", xdebug_dbgp_status_strings[XG(status)]);
	xdebug_xml_add_attribute(*retval, "reason", xdebug_dbgp_reason_strings[XG(reason)]);
}


DBGP_FUNC(context_names)
{
	xdebug_xml_node *child;

	child = xdebug_xml_node_init("context");
	xdebug_xml_add_attribute(child, "name", "Locals");
	xdebug_xml_add_attribute(child, "id", "0");
	xdebug_xml_add_child(*retval, child);

	child = xdebug_xml_node_init("context");
	xdebug_xml_add_attribute(child, "name", "Superglobals");
	xdebug_xml_add_attribute(child, "id", "1");
	xdebug_xml_add_child(*retval, child);

	child = xdebug_xml_node_init("context");
	xdebug_xml_add_attribute(child, "name", "User defined constants");
	xdebug_xml_add_attribute(child, "id", "2");
	xdebug_xml_add_child(*retval, child);
}

DBGP_FUNC(context_get)
{
	int                        res;
	int                        context_id = 0;
	int                        depth = 0;
	xdebug_var_export_options *options = (xdebug_var_export_options*) context->options;

	if (CMD_OPTION_SET('c')) {
		context_id = atol(CMD_OPTION_CHAR('c'));
	}
	if (CMD_OPTION_SET('d')) {
		depth = atol(CMD_OPTION_CHAR('d'));
	}
	/* Always reset to page = 0, as it might have been modified by property_get or property_value */
	options->runtime[0].page = 0;

	res = attach_context_vars(*retval, options, context_id, depth, attach_declared_var_with_contents TSRMLS_CC);
	switch (res) {
		case 1:
			RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_STACK_DEPTH_INVALID);
			break;
	}

	xdebug_xml_add_attribute_ex(*retval, "context", xdebug_sprintf("%d", context_id), 0, 1);
}

DBGP_FUNC(xcmd_profiler_name_get)
{
	if (XG(profiler_enabled) && XG(profile_filename)) {
		xdebug_xml_add_text(*retval, xdstrdup(XG(profile_filename)));
	} else {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_PROFILING_NOT_STARTED);
	}
}

DBGP_FUNC(xcmd_get_executable_lines)
{
	function_stack_entry *fse;
	unsigned int          i;
	long                  depth;
	xdebug_xml_node      *lines, *line;

	if (!CMD_OPTION_SET('d')) {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_INVALID_ARGS);
	}

	depth = strtol(CMD_OPTION_CHAR('d'), NULL, 10);
	if (depth >= 0 && depth < (long) XG(level)) {
		fse = xdebug_get_stack_frame(depth TSRMLS_CC);
	} else {
		RETURN_RESULT(XG(status), XG(reason), XDEBUG_ERROR_STACK_DEPTH_INVALID);
	}

	lines = xdebug_xml_node_init("xdebug:lines");
	for (i = 0; i < fse->op_array->last; i++ ) {
		if (fse->op_array->opcodes[i].opcode == ZEND_EXT_STMT ) {
			line = xdebug_xml_node_init("xdebug:line");
			xdebug_xml_add_attribute_ex(line, "lineno", xdebug_sprintf("%lu", fse->op_array->opcodes[i].lineno), 0, 1);
			xdebug_xml_add_child(lines, line);
		}
	}
	xdebug_xml_add_child(*retval, lines);
}


/*****************************************************************************
** Parsing functions
*/

/* {{{ Constants for state machine */
#define STATE_NORMAL                   0
#define STATE_QUOTED                   1
#define STATE_OPT_FOLLOWS              2
#define STATE_SEP_FOLLOWS              3
#define STATE_VALUE_FOLLOWS_FIRST_CHAR 4
#define STATE_VALUE_FOLLOWS            5
#define STATE_SKIP_CHAR                6
#define STATE_ESCAPED_CHAR_FOLLOWS     7
/* }}} */

static void xdebug_dbgp_arg_dtor(xdebug_dbgp_arg *arg)
{
	int i;

	for (i = 0; i < 27; i++) {
		if (arg->value[i]) {
			xdebug_str_free(arg->value[i]);
		}
	}
	xdfree(arg);
}

static int xdebug_dbgp_parse_cmd(char *line, char **cmd, xdebug_dbgp_arg **ret_args)
{
	xdebug_dbgp_arg *args = NULL;
	char *ptr;
	int   state;
	int   charescaped = 0;
	char  opt = ' ', *value_begin = NULL;

	args = xdmalloc(sizeof (xdebug_dbgp_arg));
	memset(args->value, 0, sizeof(args->value));
	*cmd = NULL;

	/* Find the end of the command, this is always on the first space */
	ptr = strchr(line, ' ');
	if (!ptr) {
		/* No space found. If the line is not empty, return the line
		 * and assume it only consists of the command name. If the line
		 * is 0 chars long, we return a failure. */
		if (strlen(line)) {
			*cmd = strdup(line);
			*ret_args = args;
			return XDEBUG_ERROR_OK;
		} else {
			goto parse_error;
		}
	} else {
		/* A space was found, so we copy everything before it
		 * into the cmd parameter. */
		*cmd = xdcalloc(1, ptr - line + 1);
		memcpy(*cmd, line, ptr - line);
	}
	/* Now we loop until we find the end of the string, which is the \0
	 * character */
	state = STATE_NORMAL;
	do {
		ptr++;
		switch (state) {
			case STATE_NORMAL:
				if (*ptr != '-') {
					goto parse_error;
				} else {
					state = STATE_OPT_FOLLOWS;
				}
				break;
			case STATE_OPT_FOLLOWS:
				opt = *ptr;
				state = STATE_SEP_FOLLOWS;
				break;
			case STATE_SEP_FOLLOWS:
				if (*ptr != ' ') {
					goto parse_error;
				} else {
					state = STATE_VALUE_FOLLOWS_FIRST_CHAR;
					value_begin = ptr + 1;
				}
				break;
			case STATE_VALUE_FOLLOWS_FIRST_CHAR:
				if (*ptr == '"' && opt != '-') {
					value_begin = ptr + 1;
					state = STATE_QUOTED;
				} else {
					state = STATE_VALUE_FOLLOWS;
				}
				break;
			case STATE_VALUE_FOLLOWS:
				if ((*ptr == ' ' && opt != '-') || *ptr == '\0') {
					int opt_index = opt - 'a';

					if (opt == '-') {
						opt_index = 26;
					}

					if (!args->value[opt_index]) {
						args->value[opt_index] = xdebug_str_create(value_begin, ptr - value_begin);
						state = STATE_NORMAL;
					} else {
						goto duplicate_opts;
					}
				}
				break;
			case STATE_QUOTED:
				if (*ptr == '\\') {
					state = STATE_ESCAPED_CHAR_FOLLOWS;
				} else
				if (*ptr == '"') {
					int opt_index = opt - 'a';

					if (charescaped) {
						charescaped = 0;
						break;
					}
					if (opt == '-') {
						opt_index = 26;
					}

					if (!args->value[opt_index]) {
						int len = ptr - value_begin;

						args->value[opt_index] = xdebug_str_create(value_begin, len);
						xdebug_stripcslashes(args->value[opt_index]->d, &len);
						args->value[opt_index]->l = len;

						state = STATE_SKIP_CHAR;
					} else {
						goto duplicate_opts;
					}
				}
				break;
			case STATE_SKIP_CHAR:
				state = STATE_NORMAL;
				break;
			case STATE_ESCAPED_CHAR_FOLLOWS:
				state = STATE_QUOTED;
				break;

		}
	} while (*ptr);
	*ret_args = args;
	return XDEBUG_ERROR_OK;

parse_error:
	*ret_args = args;
	return XDEBUG_ERROR_PARSE;

duplicate_opts:
	*ret_args = args;
	return XDEBUG_ERROR_DUP_ARG;
}

static int xdebug_dbgp_parse_option(xdebug_con *context, char* line, int flags, xdebug_xml_node *retval TSRMLS_DC)
{
	char *cmd = NULL;
	int res, ret = 0;
	xdebug_dbgp_arg *args;
	xdebug_dbgp_cmd *command;
	xdebug_xml_node *error;

	context->handler->log(XDEBUG_LOG_COM, "<- %s\n", line);
	res = xdebug_dbgp_parse_cmd(line, (char**) &cmd, (xdebug_dbgp_arg**) &args);

	/* Add command name to return packet */
	if (cmd) {
		/* if no cmd res will be XDEBUG_ERROR_PARSE */
		xdebug_xml_add_attribute_ex(retval, "command", xdstrdup(cmd), 0, 1);
	}

	/* Handle missing transaction ID, and if it exist add it to the result */
	if (!CMD_OPTION_SET('i')) {
		/* we need the transaction_id even for errors in parse_cmd, but if
		   we error out here, just force the error to happen below */
		res = XDEBUG_ERROR_INVALID_ARGS;
	} else {
		xdebug_xml_add_attribute_ex(retval, "transaction_id", xdstrdup(CMD_OPTION_CHAR('i')), 0, 1);
	}

	/* Handle parse errors */
	/* FIXME: use RETURN_RESULT here too */
	if (res != XDEBUG_ERROR_OK) {
		error = xdebug_xml_node_init("error");
		xdebug_xml_add_attribute_ex(error, "code", xdebug_sprintf("%lu", res), 0, 1);
		xdebug_xml_add_child(retval, error);
		ADD_REASON_MESSAGE(res);
	} else {

		/* Execute commands and stuff */
		command = lookup_cmd(cmd);

		if (command) {
			if (command->cont) {
				XG(status) = DBGP_STATUS_RUNNING;
				XG(reason) = DBGP_REASON_OK;
			}
			XG(lastcmd) = command->name;
			if (XG(lasttransid)) {
				xdfree(XG(lasttransid));
			}
			XG(lasttransid) = xdstrdup(CMD_OPTION_CHAR('i'));
			if (XG(status) != DBGP_STATUS_STOPPING || (XG(status) == DBGP_STATUS_STOPPING && command->flags & XDEBUG_DBGP_POST_MORTEM)) {
				command->handler((xdebug_xml_node**) &retval, context, args TSRMLS_CC);
				ret = command->cont;
			} else {
				error = xdebug_xml_node_init("error");
				xdebug_xml_add_attribute_ex(error, "code", xdebug_sprintf("%lu", XDEBUG_ERROR_COMMAND_UNAVAILABLE), 0, 1);
				ADD_REASON_MESSAGE(XDEBUG_ERROR_COMMAND_UNAVAILABLE);
				xdebug_xml_add_child(retval, error);

				ret = -1;
			}
		} else {
			error = xdebug_xml_node_init("error");
			xdebug_xml_add_attribute_ex(error, "code", xdebug_sprintf("%lu", XDEBUG_ERROR_UNIMPLEMENTED), 0, 1);
			ADD_REASON_MESSAGE(XDEBUG_ERROR_UNIMPLEMENTED);
			xdebug_xml_add_child(retval, error);

			ret = -1;
		}
	}

	xdfree(cmd);
	xdebug_dbgp_arg_dtor(args);
	return ret;
}

/*****************************************************************************
** Handlers for debug functions
*/

static int xdebug_dbgp_cmdloop(xdebug_con *context, int bail TSRMLS_DC)
{
	char *option;
	int   ret;
	xdebug_xml_node *response;

	do {
		option = xdebug_fd_read_line_delim(context->socket, context->buffer, FD_RL_SOCKET, '\0', NULL);
		if (!option) {
			return 0;
		}

		response = xdebug_xml_node_init("response");
		xdebug_xml_add_attribute(response, "xmlns", "urn:debugger_protocol_v1");
		xdebug_xml_add_attribute(response, "xmlns:xdebug", "https://xdebug.org/dbgp/xdebug");
		ret = xdebug_dbgp_parse_option(context, option, 0, response TSRMLS_CC);
		if (ret != 1) {
			send_message(context, response TSRMLS_CC);
		}
		xdebug_xml_node_dtor(response);

		free(option);
	} while (0 == ret);

	if (bail && XG(status) == DBGP_STATUS_STOPPED) {
		_zend_bailout((char*)__FILE__, __LINE__);
	}
	return ret;

}

int xdebug_dbgp_init(xdebug_con *context, int mode)
{
	xdebug_var_export_options *options;
	xdebug_xml_node *response, *child;
	int i;
	TSRMLS_FETCH();

	/* initialize our status information */
	if (mode == XDEBUG_REQ) {
		XG(status) = DBGP_STATUS_STARTING;
		XG(reason) = DBGP_REASON_OK;
	} else if (mode == XDEBUG_JIT) {
		XG(status) = DBGP_STATUS_BREAK;
		XG(reason) = DBGP_REASON_ERROR;
	}
	XG(lastcmd) = NULL;
	XG(lasttransid) = NULL;

	response = xdebug_xml_node_init("init");
	xdebug_xml_add_attribute(response, "xmlns", "urn:debugger_protocol_v1");
	xdebug_xml_add_attribute(response, "xmlns:xdebug", "https://xdebug.org/dbgp/xdebug");

/* {{{ XML Init Stuff*/
	child = xdebug_xml_node_init("engine");
	xdebug_xml_add_attribute(child, "version", XDEBUG_VERSION);
	xdebug_xml_add_text(child, xdstrdup(XDEBUG_NAME));
	xdebug_xml_add_child(response, child);

	child = xdebug_xml_node_init("author");
	xdebug_xml_add_text(child, xdstrdup(XDEBUG_AUTHOR));
	xdebug_xml_add_child(response, child);

	child = xdebug_xml_node_init("url");
	xdebug_xml_add_text(child, xdstrdup(XDEBUG_URL));
	xdebug_xml_add_child(response, child);

	child = xdebug_xml_node_init("copyright");
	xdebug_xml_add_text(child, xdstrdup(XDEBUG_COPYRIGHT));
	xdebug_xml_add_child(response, child);

	if (strcmp(context->program_name, "-") == 0 || strcmp(context->program_name, "Command line code") == 0) {
		xdebug_xml_add_attribute_ex(response, "fileuri", xdstrdup("dbgp://stdin"), 0, 1);
	} else {
		xdebug_xml_add_attribute_ex(response, "fileuri", xdebug_path_to_url(context->program_name TSRMLS_CC), 0, 1);
	}
	xdebug_xml_add_attribute_ex(response, "language", "PHP", 0, 0);
	xdebug_xml_add_attribute_ex(response, "xdebug:language_version", PHP_VERSION, 0, 0);
	xdebug_xml_add_attribute_ex(response, "protocol_version", DBGP_VERSION, 0, 0);
	xdebug_xml_add_attribute_ex(response, "appid", xdebug_sprintf(ZEND_ULONG_FMT, xdebug_get_pid()), 0, 1);

	if (getenv("DBGP_COOKIE")) {
		xdebug_xml_add_attribute_ex(response, "session", xdstrdup(getenv("DBGP_COOKIE")), 0, 1);
	}

	if (XG(ide_key) && *XG(ide_key)) {
		xdebug_xml_add_attribute_ex(response, "idekey", xdstrdup(XG(ide_key)), 0, 1);
	}

	context->buffer = xdmalloc(sizeof(fd_buf));
	context->buffer->buffer = NULL;
	context->buffer->buffer_size = 0;

	send_message_ex(context, response, DBGP_STATUS_STARTING TSRMLS_CC);
	xdebug_xml_node_dtor(response);
/* }}} */

	context->options = xdmalloc(sizeof(xdebug_var_export_options));
	options = (xdebug_var_export_options*) context->options;
	options->max_children = 32;
	options->max_data     = 1024;
	options->max_depth    = 1;
	options->show_hidden  = 0;
	options->extended_properties         = 0;
	options->encode_as_extended_property = 0;
	options->runtime = (xdebug_var_runtime_page*) xdmalloc((options->max_depth + 1) * sizeof(xdebug_var_runtime_page));
	for (i = 0; i < options->max_depth; i++) {
		options->runtime[i].page = 0;
		options->runtime[i].current_element_nr = 0;
	}

	context->breakpoint_list = xdebug_hash_alloc(64, (xdebug_hash_dtor_t) xdebug_hash_admin_dtor);
	context->function_breakpoints = xdebug_hash_alloc(64, (xdebug_hash_dtor_t) xdebug_hash_brk_dtor);
	context->exception_breakpoints = xdebug_hash_alloc(64, (xdebug_hash_dtor_t) xdebug_hash_brk_dtor);
	context->line_breakpoints = xdebug_llist_alloc((xdebug_llist_dtor) xdebug_llist_brk_dtor);
	context->eval_id_lookup = xdebug_hash_alloc(64, (xdebug_hash_dtor_t) xdebug_hash_eval_info_dtor);
	context->eval_id_sequence = 0;
	context->send_notifications = 0;
	context->inhibit_notifications = 0;
	context->resolved_breakpoints = 0;

	xdebug_mark_debug_connection_active();
	xdebug_dbgp_cmdloop(context, 1 TSRMLS_CC);

	return 1;
}

int xdebug_dbgp_deinit(xdebug_con *context)
{
	xdebug_xml_node           *response;
	xdebug_var_export_options *options;
	TSRMLS_FETCH();

	if (xdebug_is_debug_connection_active_for_current_pid()) {
		XG(status) = DBGP_STATUS_STOPPING;
		XG(reason) = DBGP_REASON_OK;
		response = xdebug_xml_node_init("response");
		xdebug_xml_add_attribute(response, "xmlns", "urn:debugger_protocol_v1");
		xdebug_xml_add_attribute(response, "xmlns:xdebug", "https://xdebug.org/dbgp/xdebug");
		/* lastcmd and lasttransid are not always set (for example when the
		 * connection is severed before the first command is send) */
		if (XG(lastcmd) && XG(lasttransid)) {
			xdebug_xml_add_attribute_ex(response, "command", XG(lastcmd), 0, 0);
			xdebug_xml_add_attribute_ex(response, "transaction_id", XG(lasttransid), 0, 0);
		}
		xdebug_xml_add_attribute_ex(response, "status", xdebug_dbgp_status_strings[XG(status)], 0, 0);
		xdebug_xml_add_attribute_ex(response, "reason", xdebug_dbgp_reason_strings[XG(reason)], 0, 0);

		send_message(context, response TSRMLS_CC);
		xdebug_xml_node_dtor(response);

		xdebug_dbgp_cmdloop(context, 0 TSRMLS_CC);
	}

	if (xdebug_is_debug_connection_active_for_current_pid()) {
		options = (xdebug_var_export_options*) context->options;
		xdfree(options->runtime);
		xdfree(context->options);
		xdebug_hash_destroy(context->function_breakpoints);
		xdebug_hash_destroy(context->exception_breakpoints);
		xdebug_hash_destroy(context->eval_id_lookup);
		xdebug_llist_destroy(context->line_breakpoints, NULL);
		xdebug_hash_destroy(context->breakpoint_list);
		xdfree(context->buffer);
		context->buffer = NULL;
	}

	if (XG(lasttransid)) {
		xdfree(XG(lasttransid));
		XG(lasttransid) = NULL;
	}
	xdebug_mark_debug_connection_not_active();
	return 1;
}

int xdebug_dbgp_error(xdebug_con *context, int type, char *exception_type, char *message, const char *location, const unsigned int line, xdebug_llist *stack)
{
	char               *errortype;
	xdebug_xml_node     *response, *error;
	TSRMLS_FETCH();

	if (exception_type) {
		errortype = exception_type;
	} else {
		errortype = xdebug_error_type(type);
	}

	if (exception_type) {
		XG(status) = DBGP_STATUS_BREAK;
		XG(reason) = DBGP_REASON_EXCEPTION;
	} else {
		switch (type) {
			case E_CORE_ERROR:
			/* no break - intentionally */
			case E_ERROR:
			/*case E_PARSE: the parser would return 1 (failure), we can bail out nicely */
			case E_COMPILE_ERROR:
			case E_USER_ERROR:
				XG(status) = DBGP_STATUS_STOPPING;
				XG(reason) = DBGP_REASON_ABORTED;
				break;
			default:
				XG(status) = DBGP_STATUS_BREAK;
				XG(reason) = DBGP_REASON_ERROR;
		}
	}
/*
	runtime_allowed = (
		(type != E_ERROR) &&
		(type != E_CORE_ERROR) &&
		(type != E_COMPILE_ERROR) &&
		(type != E_USER_ERROR)
	) ? XDEBUG_BREAKPOINT | XDEBUG_RUNTIME : 0;
*/

	response = xdebug_xml_node_init("response");
	xdebug_xml_add_attribute(response, "xmlns", "urn:debugger_protocol_v1");
	xdebug_xml_add_attribute(response, "xmlns:xdebug", "https://xdebug.org/dbgp/xdebug");
	/* lastcmd and lasttransid are not always set (for example when the
	 * connection is severed before the first command is send) */
	if (XG(lastcmd) && XG(lasttransid)) {
		xdebug_xml_add_attribute_ex(response, "command", XG(lastcmd), 0, 0);
		xdebug_xml_add_attribute_ex(response, "transaction_id", XG(lasttransid), 0, 0);
	}
	xdebug_xml_add_attribute(response, "status", xdebug_dbgp_status_strings[XG(status)]);
	xdebug_xml_add_attribute(response, "reason", xdebug_dbgp_reason_strings[XG(reason)]);

	error = xdebug_xml_node_init("error");
	xdebug_xml_add_attribute_ex(error, "code", xdebug_sprintf("%lu", type), 0, 1);
	xdebug_xml_add_attribute_ex(error, "exception", xdstrdup(errortype), 0, 1);
	xdebug_xml_add_text(error, xdstrdup(message));
	xdebug_xml_add_child(response, error);

	send_message(context, response TSRMLS_CC);
	xdebug_xml_node_dtor(response);
	if (!exception_type) {
		xdfree(errortype);
	}

	xdebug_dbgp_cmdloop(context, 1 TSRMLS_CC);

	return 1;
}

int xdebug_dbgp_break_on_line(xdebug_con *context, xdebug_brk_info *brk, const char *file, int file_len, int lineno)
{
	char *tmp_file = (char*) file;
	int   tmp_file_len = file_len;

	context->handler->log(XDEBUG_LOG_DEBUG, "Checking whether to break on %s:%d\n", brk->file, brk->resolved_lineno);

	if (brk->disabled) {
		context->handler->log(XDEBUG_LOG_DEBUG, "R: Breakpoint is disabled\n");
		return 0;
	}

	context->handler->log(XDEBUG_LOG_DEBUG, "I: Current location: %s:%d\n", tmp_file, lineno);

	if (is_dbgp_url(brk->file) && check_evaled_code(NULL, &tmp_file, 0)) {
		tmp_file_len = strlen(tmp_file);
		context->handler->log(XDEBUG_LOG_DEBUG, "I: Found eval code for '%s': %s\n", file, tmp_file);
	}

	context->handler->log(XDEBUG_LOG_DEBUG, "I: Matching breakpoint '%s:%d' against location '%s:%d'\n", brk->file, brk->resolved_lineno, tmp_file, lineno);

	if (brk->file_len != tmp_file_len) {
		context->handler->log(XDEBUG_LOG_DEBUG, "R: File name length (%d) doesn't match with breakpoint (%d)\n", tmp_file_len, brk->file_len);
		return 0;
	}

	if (brk->resolved_lineno != lineno) {
		context->handler->log(XDEBUG_LOG_DEBUG, "R: Line number (%d) doesn't match with breakpoint (%d)\n", lineno, brk->resolved_lineno);
		return 0;
	}

	if (strncasecmp(brk->file, tmp_file, brk->file_len) == 0) {
		context->handler->log(XDEBUG_LOG_DEBUG, "F: File names match (%s)\n", brk->file);
		return 1;
	}

	context->handler->log(XDEBUG_LOG_DEBUG, "R: File names (%s) doesn't match with breakpoint (%s)\n", tmp_file, brk->file);
	return 0;
}

int xdebug_dbgp_breakpoint(xdebug_con *context, xdebug_llist *stack, char *file, long lineno, int type, char *exception, char *code, char *message)
{
	xdebug_xml_node *response, *error_container;
	TSRMLS_FETCH();

	XG(status) = DBGP_STATUS_BREAK;
	XG(reason) = DBGP_REASON_OK;

	response = xdebug_xml_node_init("response");
	xdebug_xml_add_attribute(response, "xmlns", "urn:debugger_protocol_v1");
	xdebug_xml_add_attribute(response, "xmlns:xdebug", "https://xdebug.org/dbgp/xdebug");
	/* lastcmd and lasttransid are not always set (for example when the
	 * connection is severed before the first command is send) */
	if (XG(lastcmd) && XG(lasttransid)) {
		xdebug_xml_add_attribute_ex(response, "command", XG(lastcmd), 0, 0);
		xdebug_xml_add_attribute_ex(response, "transaction_id", XG(lasttransid), 0, 0);
	}
	xdebug_xml_add_attribute(response, "status", xdebug_dbgp_status_strings[XG(status)]);
	xdebug_xml_add_attribute(response, "reason", xdebug_dbgp_reason_strings[XG(reason)]);

	error_container = xdebug_xml_node_init("xdebug:message");
	if (file) {
		char *tmp_filename = file;
		if (check_evaled_code(NULL, &tmp_filename, 0)) {
			xdebug_xml_add_attribute_ex(error_container, "filename", xdstrdup(tmp_filename), 0, 1);
		} else {
			xdebug_xml_add_attribute_ex(error_container, "filename", xdebug_path_to_url(file TSRMLS_CC), 0, 1);
		}
	}
	if (lineno) {
		xdebug_xml_add_attribute_ex(error_container, "lineno", xdebug_sprintf("%lu", lineno), 0, 1);
	}
	if (exception) {
		xdebug_xml_add_attribute_ex(error_container, "exception", xdstrdup(exception), 0, 1);
	}
	if (code) {
		xdebug_xml_add_attribute_ex(error_container, "code", xdstrdup(code), 0, 1);
	}
	if (message) {
		xdebug_xml_add_text(error_container, xdstrdup(message));
	}
	xdebug_xml_add_child(response, error_container);

	send_message(context, response TSRMLS_CC);
	xdebug_xml_node_dtor(response);

	XG(lastcmd) = NULL;
	if (XG(lasttransid)) {
		xdfree(XG(lasttransid));
		XG(lasttransid) = NULL;
	}

	xdebug_dbgp_cmdloop(context, 1 TSRMLS_CC);

	return xdebug_is_debug_connection_active_for_current_pid();
}

xdebug_set *get_executable_lines_from_oparray(function_stack_entry *fse)
{
	int         i;
	zend_op_array *opa = fse->op_array;
	xdebug_set *tmp;

	if (fse->executable_lines_cache) {
		return fse->executable_lines_cache;
	}

	tmp = xdebug_set_create(opa->line_end);

	for (i = 0; i < opa->last; i++ ) {
		if (opa->opcodes[i].opcode == ZEND_EXT_STMT ) {
			xdebug_set_add(tmp, opa->opcodes[i].lineno);
		}
	}

	return tmp;
}

static int xdebug_dbgp_resolved_breakpoint_notification(xdebug_con *context, xdebug_brk_info *brk_info)
{
	xdebug_xml_node *response, *child;

	response = xdebug_xml_node_init("notify");
	xdebug_xml_add_attribute(response, "xmlns", "urn:debugger_protocol_v1");
	xdebug_xml_add_attribute(response, "xmlns:xdebug", "https://xdebug.org/dbgp/xdebug");
	xdebug_xml_add_attribute(response, "name", "breakpoint_resolved");

	child = xdebug_xml_node_init("breakpoint");
	breakpoint_brk_info_add(child, brk_info);
	xdebug_xml_add_child(response, child);

	send_message(context, response TSRMLS_CC);
	xdebug_xml_node_dtor(response);

	return 1;
}

inline static int function_span_is_smaller_than_resolved_span(zend_op_array *opa, xdebug_brk_span *span)
{
	if (
		(opa->line_start >= span->start && opa->line_end < span->end) ||
		(opa->line_start > span->start && opa->line_end <= span->end)
	) {
		return 1;
	}
	return 0;
}

static void function_breakpoint_resolve_helper(void *rctxt, xdebug_brk_info *brk_info, xdebug_hash_element *he)
{
	xdebug_dbgp_resolve_context *ctxt = (xdebug_dbgp_resolve_context*) rctxt;

	if (brk_info->resolved == XDEBUG_BRK_RESOLVED) {
		ctxt->context->handler->log(XDEBUG_LOG_DEBUG, "R: %s breakpoint for '%s' has already been resolved\n",
			XDEBUG_BREAKPOINT_TYPE_NAME(brk_info->brk_type), ctxt->fse->function.function);
		return;
	}

	if (ctxt->fse->function.type == XFUNC_NORMAL) {
		ctxt->context->handler->log(XDEBUG_LOG_DEBUG, "I: '%s' is a normal function (%02x)\n", ctxt->fse->function.function, ctxt->fse->function.type);

		if (strcmp(ctxt->fse->function.function, brk_info->functionname) == 0) {
			ctxt->context->handler->log(XDEBUG_LOG_DEBUG, "F: Breakpoint function (%s) matches current function (%s)\n", brk_info->functionname, ctxt->fse->function.function);
			brk_info->resolved = XDEBUG_BRK_RESOLVED;
			xdebug_dbgp_resolved_breakpoint_notification(ctxt->context, brk_info);
			return;
		}
	} else if (ctxt->fse->function.type == XFUNC_MEMBER || ctxt->fse->function.type == XFUNC_STATIC_MEMBER) {
		char  *tmp_name = NULL;
		size_t tmp_len = 0;

		tmp_len = strlen(ctxt->fse->function.class) + strlen(ctxt->fse->function.function) + 3;
		tmp_name = xdmalloc(tmp_len);
		snprintf(tmp_name, tmp_len, "%s::%s", ctxt->fse->function.class, ctxt->fse->function.function);

		ctxt->context->handler->log(XDEBUG_LOG_DEBUG, "I: '%s::%s' is a normal method (%02x)\n", ctxt->fse->function.class, ctxt->fse->function.function, ctxt->fse->function.type);

		if (strcmp(tmp_name, brk_info->functionname) == 0) {
			ctxt->context->handler->log(XDEBUG_LOG_DEBUG, "F: Breakpoint method (%s) matches current method (%s)\n", brk_info->functionname, tmp_name);
			brk_info->resolved = XDEBUG_BRK_RESOLVED;
			xdebug_dbgp_resolved_breakpoint_notification(ctxt->context, brk_info);
			xdfree(tmp_name);
			return;
		}

		xdfree(tmp_name);
	} else {
		ctxt->context->handler->log(XDEBUG_LOG_DEBUG, "R: We don't handle this function type (%02x) yet\n", ctxt->fse->function.type);
		return;
	}
}

static void line_breakpoint_resolve_helper(xdebug_con *context, function_stack_entry *fse, xdebug_brk_info *brk_info)
{
	/* If the breakpoint's line number isn't in the function's range, bail out */
	if (brk_info->original_lineno < fse->op_array->line_start || brk_info->original_lineno > fse->op_array->line_end) {
		context->handler->log(XDEBUG_LOG_DEBUG, "R: Line number (%d) out of range (%d-%d)\n", brk_info->original_lineno, fse->op_array->line_start, fse->op_array->line_end);
		return;
	}

	/* If we have resolved before, we only resolve again if the current scope's
	 * line-span is *smaller* then the one that was used for the already
	 * resolved case */
	if (brk_info->resolved == XDEBUG_BRK_RESOLVED && !function_span_is_smaller_than_resolved_span(fse->op_array, &brk_info->resolved_span)) {
		context->handler->log(XDEBUG_LOG_DEBUG, "R: Resolved span (%d-%d) is not smaller than function span (%d-%d)\n", brk_info->resolved_span.start, brk_info->resolved_span.end, fse->op_array->line_start, fse->op_array->line_end);
		return;
	} else if (brk_info->resolved != XDEBUG_BRK_RESOLVED) {
		context->handler->log(XDEBUG_LOG_DEBUG, "I: Has not been resolved yet\n");
	} else {
		context->handler->log(XDEBUG_LOG_DEBUG, "I: Resolved span (%d-%d) is smaller than function span (%d-%d)\n", brk_info->resolved_span.start, brk_info->resolved_span.end, fse->op_array->line_start, fse->op_array->line_end);
	}

	/* If we're not in a normal function or method: */
	if (XDEBUG_IS_NORMAL_FUNCTION(&fse->function)) {
		context->handler->log(XDEBUG_LOG_DEBUG, "I: '%s' is a normal function or method (%02x)\n", fse->function.function, fse->function.type);

		/* If the 'line' breakpoint's file and current file don't match, bail out */
		if (strcmp(brk_info->file, STR_NAME_VAL(fse->op_array->filename)) != 0) {
			context->handler->log(XDEBUG_LOG_DEBUG, "R: Breakpoint file name (%s) does not match function's file name (%s)\n", brk_info->file, STR_NAME_VAL(fse->op_array->filename));
			return;
		}
	}
	/* else, if we're in an eval: */
	else if (fse->function.type == XFUNC_EVAL) {
		char *key, *dbgp_eval_key;
		xdebug_eval_info *ei;

		context->handler->log(XDEBUG_LOG_DEBUG, "I: Current 'function' is an eval statement\n");

		key = create_eval_key_file(fse->filename, fse->lineno);
		context->handler->log(XDEBUG_LOG_DEBUG, "   I: Looking up eval ID for '%s'\n", key);
		if (!xdebug_hash_find(context->eval_id_lookup, key, strlen(key), (void *) &ei)) {
			context->handler->log(XDEBUG_LOG_DEBUG, "   R: Eval ID not found\n");
			xdfree(key);
			return;
		}
		xdfree(key);

		context->handler->log(XDEBUG_LOG_DEBUG, "   I: Constructing 'filename' for eval ID '%d'\n", ei->id);
		dbgp_eval_key = xdebug_sprintf("dbgp://%d", ei->id);

		if (strcmp(dbgp_eval_key, brk_info->file) != 0) {
			context->handler->log(XDEBUG_LOG_DEBUG, "   R: Breakpoint file name (%s) does not match eval's file name (%s)\n", brk_info->file, dbgp_eval_key);
			xdfree(dbgp_eval_key);
			return;
		}
		xdfree(dbgp_eval_key);
	}
	/* else, if we're an include or require: */
	else if (fse->function.type & XFUNC_INCLUDES) {
		context->handler->log(XDEBUG_LOG_DEBUG, "I: Current 'function' is a file scope (%s)\n", STR_NAME_VAL(fse->op_array->filename));

		/* If the 'line' breakpoint's file and current file don't match, bail out */
		if (strcmp(brk_info->file, STR_NAME_VAL(fse->op_array->filename)) != 0) {
			context->handler->log(XDEBUG_LOG_DEBUG, "   R: Breakpoint file name (%s) does not match file's name (%s)\n", brk_info->file, STR_NAME_VAL(fse->op_array->filename));
			return;
		}
	} else {
		context->handler->log(XDEBUG_LOG_DEBUG, "R: We don't handle this function type (%02x) yet\n", fse->function.type);
		return;
	}

	/* If the breakpoint's line number is in the set, mark as resolved */
	if (xdebug_set_in(get_executable_lines_from_oparray(fse), brk_info->original_lineno)) {
		context->handler->log(XDEBUG_LOG_DEBUG, "F: Breakpoint line (%d) found in set of executable lines\n", brk_info->original_lineno);
		brk_info->resolved_lineno = brk_info->original_lineno;
		brk_info->resolved_span.start = fse->op_array->line_start;
		brk_info->resolved_span.end   = fse->op_array->line_end;
		brk_info->resolved = XDEBUG_BRK_RESOLVED;
		xdebug_dbgp_resolved_breakpoint_notification(context, brk_info);
		return;
	} else {
		int tmp_lineno;

		context->handler->log(XDEBUG_LOG_DEBUG, "I: Breakpoint line (%d) NOT found in set of executable lines\n", brk_info->original_lineno);

		/* Check for a following line in the function */
		tmp_lineno = brk_info->original_lineno;
		do {
			tmp_lineno++;

			if (xdebug_set_in(get_executable_lines_from_oparray(fse), tmp_lineno)) {
				context->handler->log(XDEBUG_LOG_DEBUG, "  F: Line (%d) in set (with span: %d-%d)\n", tmp_lineno, fse->op_array->line_start, fse->op_array->line_end);

				brk_info->resolved_lineno = tmp_lineno;
				brk_info->resolved_span.start = fse->op_array->line_start;
				brk_info->resolved_span.end   = fse->op_array->line_end;
				brk_info->resolved = XDEBUG_BRK_RESOLVED;
				xdebug_dbgp_resolved_breakpoint_notification(context, brk_info);
				return;
			} else {
				context->handler->log(XDEBUG_LOG_DEBUG, "  I: Line (%d) not in set\n", tmp_lineno);
			}
		} while (tmp_lineno < fse->op_array->line_end && (tmp_lineno < brk_info->original_lineno + XDEBUG_DBGP_SCAN_RANGE));

		/* Check for a previous line in the function */
		tmp_lineno = brk_info->original_lineno;
		do {
			tmp_lineno--;

			if (xdebug_set_in(get_executable_lines_from_oparray(fse), tmp_lineno)) {
				context->handler->log(XDEBUG_LOG_DEBUG, "  F: Line (%d) in set\n", tmp_lineno);

				brk_info->resolved_lineno = tmp_lineno;
				brk_info->resolved_span.start = fse->op_array->line_start;
				brk_info->resolved_span.end   = fse->op_array->line_end;
				brk_info->resolved = XDEBUG_BRK_RESOLVED;
				xdebug_dbgp_resolved_breakpoint_notification(context, brk_info);
				return;
			} else {
				context->handler->log(XDEBUG_LOG_DEBUG, "  I: Line (%d) not in set\n", tmp_lineno);
			}
		} while (tmp_lineno > fse->op_array->line_start && (tmp_lineno > brk_info->original_lineno - XDEBUG_DBGP_SCAN_RANGE));
	}
}

static void exception_breakpoint_resolve_helper(xdebug_con *context, xdebug_brk_info *brk_info)
{
	if (brk_info->resolved == XDEBUG_BRK_RESOLVED) {
		context->handler->log(XDEBUG_LOG_DEBUG, "R: %s breakpoint for '%s' has already been resolved\n", XDEBUG_BREAKPOINT_TYPE_NAME(brk_info->brk_type), brk_info->exceptionname);
		return;
	}

	if (strcmp("*", brk_info->exceptionname) == 0) {
		context->handler->log(XDEBUG_LOG_DEBUG, "F: Breakpoint exception (%s) matches every exception\n", brk_info->exceptionname);
		brk_info->resolved = XDEBUG_BRK_RESOLVED;
		xdebug_dbgp_resolved_breakpoint_notification(context, brk_info);
		return;
	}

	context->handler->log(XDEBUG_LOG_DEBUG, "F: Breakpoint exception (%s) matches\n", brk_info->exceptionname);
	brk_info->resolved = XDEBUG_BRK_RESOLVED;
	xdebug_dbgp_resolved_breakpoint_notification(context, brk_info);
}

static void breakpoint_resolve_helper(void *rctxt, xdebug_hash_element *he)
{
	xdebug_dbgp_resolve_context *ctxt = (xdebug_dbgp_resolve_context*) rctxt;
	xdebug_brk_admin            *admin = (xdebug_brk_admin*) he->ptr;
	xdebug_brk_info             *brk_info;

	brk_info = breakpoint_brk_info_fetch(admin->type, admin->key);

	/* This helper doesn't deal with XDEBUG_BREAKPOINT_TYPE_EXCEPTION, and hence this condition should never match */
	if (brk_info->brk_type == XDEBUG_BREAKPOINT_TYPE_EXCEPTION) {
		ctxt->context->handler->log(XDEBUG_LOG_ERR, "E: Not a user defined function (%s)\n", ctxt->fse->function.function);
	}

	ctxt->context->handler->log(XDEBUG_LOG_DEBUG, "Breakpoint %d (type: %s)\n", admin->id, XDEBUG_BREAKPOINT_TYPE_NAME(brk_info->brk_type));
	if (! (brk_info->brk_type & ctxt->breakpoint_type_set)) {
		ctxt->context->handler->log(XDEBUG_LOG_DEBUG, "R: Breakpoint type '%s' did not match requested set '%02x'\n", XDEBUG_BREAKPOINT_TYPE_NAME(brk_info->brk_type), ctxt->breakpoint_type_set);
		return;
	}

	/* If we're not in a user defined function or method, bail out */
	if (ctxt->fse->user_defined != XDEBUG_USER_DEFINED) {
		ctxt->context->handler->log(XDEBUG_LOG_DEBUG, "R: Not a user defined function (%s)\n", ctxt->fse->function.function);
		return;
	}

	switch (brk_info->brk_type) {
		case XDEBUG_BREAKPOINT_TYPE_LINE:
		case XDEBUG_BREAKPOINT_TYPE_CONDITIONAL:
			line_breakpoint_resolve_helper(ctxt->context, ctxt->fse, brk_info);
			return;

		case XDEBUG_BREAKPOINT_TYPE_CALL:
		case XDEBUG_BREAKPOINT_TYPE_RETURN:
			function_breakpoint_resolve_helper(rctxt, brk_info, he);
			return;

		default:
			ctxt->context->handler->log(XDEBUG_LOG_DEBUG, "R: The breakpoint type '%s' can not be resolved\n", XDEBUG_BREAKPOINT_TYPE_NAME(brk_info->brk_type));
			return;
	}
}

int xdebug_dbgp_resolve_breakpoints(xdebug_con *context, int breakpoint_type_set, void *data)
{
	xdebug_dbgp_resolve_context resolv_ctxt;

	if (XDEBUG_BREAKPOINT_TYPE_EXCEPTION & breakpoint_type_set) {
		exception_breakpoint_resolve_helper(context, (xdebug_brk_info*) data);
		return 1;
	}

	resolv_ctxt.context = context;
	resolv_ctxt.breakpoint_type_set = breakpoint_type_set;
	resolv_ctxt.fse = (function_stack_entry *) data;
	resolv_ctxt.executable_lines = get_executable_lines_from_oparray(resolv_ctxt.fse);
	xdebug_hash_apply(context->breakpoint_list, (void *) &resolv_ctxt, breakpoint_resolve_helper);

	return 1;
}

int xdebug_dbgp_stream_output(const char *string, unsigned int length TSRMLS_DC)
{
	if ((XG(stdout_mode) == 1 || XG(stdout_mode) == 2) && length) {
		xdebug_send_stream("stdout", string, length TSRMLS_CC);
	}

	if (XG(stdout_mode) == 0 || XG(stdout_mode) == 1) {
		return 0;
	}
	return -1;
}

int xdebug_dbgp_notification(xdebug_con *context, const char *file, long lineno, int type, char *type_string, char *message)
{
	xdebug_xml_node *response, *error_container;
	TSRMLS_FETCH();

	response = xdebug_xml_node_init("notify");
	xdebug_xml_add_attribute(response, "xmlns", "urn:debugger_protocol_v1");
	xdebug_xml_add_attribute(response, "xmlns:xdebug", "https://xdebug.org/dbgp/xdebug");
	xdebug_xml_add_attribute(response, "name", "error");

	error_container = xdebug_xml_node_init("xdebug:message");
	if (file) {
		char *tmp_filename = (char*) file;

		if (check_evaled_code(NULL, &tmp_filename, 0)) {
			xdebug_xml_add_attribute_ex(error_container, "filename", xdstrdup(tmp_filename), 0, 1);
		} else {
			xdebug_xml_add_attribute_ex(error_container, "filename", xdebug_path_to_url(file TSRMLS_CC), 0, 1);
		}
	}
	if (lineno) {
		xdebug_xml_add_attribute_ex(error_container, "lineno", xdebug_sprintf("%lu", lineno), 0, 1);
	}
	if (type_string) {
		xdebug_xml_add_attribute_ex(error_container, "type", xdstrdup(type_string), 0, 1);
	}
	if (message) {
		char *tmp_buf;

		if (type == E_ERROR && ((tmp_buf = xdebug_strip_php_stack_trace(message)) != NULL)) {
			xdebug_xml_add_text(error_container, tmp_buf);
		} else {
			xdebug_xml_add_text(error_container, xdstrdup(message));
		}
	}
	xdebug_xml_add_child(response, error_container);

	send_message(context, response TSRMLS_CC);
	xdebug_xml_node_dtor(response);

	return 1;
}

static char *create_eval_key_file(char *filename, int lineno)
{
	return xdebug_sprintf("%s(%d) : eval()'d code", filename, lineno);
}

static char *create_eval_key_id(int id)
{
	return xdebug_sprintf("%04x", id);
}

int xdebug_dbgp_register_eval_id(xdebug_con *context, function_stack_entry *fse)
{
	char             *key;
	xdebug_eval_info *ei;

	context->eval_id_sequence++;

	ei = xdcalloc(sizeof(xdebug_eval_info), 1);
	ei->id = context->eval_id_sequence;
	ei->contents = xdstrndup(fse->include_filename, strlen(fse->include_filename));
	ei->refcount = 2;

	key = create_eval_key_file(fse->filename, fse->lineno);
	xdebug_hash_add(context->eval_id_lookup, key, strlen(key), (void*) ei);

	key = create_eval_key_id(ei->id);
	xdebug_hash_add(context->eval_id_lookup, key, strlen(key), (void*) ei);

	return ei->id;
}
