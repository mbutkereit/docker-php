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
   +----------------------------------------------------------------------+
 */

#ifndef __HAVE_XDEBUG_HANDLERS_H__
#define __HAVE_XDEBUG_HANDLERS_H__

#include "php_xdebug.h"
#include "xdebug_compat.h"
#include "xdebug_llist.h"
#include "xdebug_hash.h"
#include "xdebug_private.h"
#include "usefulstuff.h"

typedef struct _xdebug_brk_admin            xdebug_brk_admin;
typedef struct _xdebug_brk_info             xdebug_brk_info;
typedef struct _xdebug_brk_span             xdebug_brk_span;
typedef struct _xdebug_eval_info            xdebug_eval_info;
typedef struct _xdebug_con                  xdebug_con;
typedef struct _xdebug_debug_list           xdebug_debug_list;
typedef struct _xdebug_remote_handler       xdebug_remote_handler;
typedef struct _xdebug_remote_handler_info  xdebug_remote_handler_info;

struct _xdebug_debug_list {
	char *last_file;
	int   last_line;
};

#define XDEBUG_BREAKPOINT_TYPE_LINE        0x01
#define XDEBUG_BREAKPOINT_TYPE_CONDITIONAL 0x02
#define XDEBUG_BREAKPOINT_TYPE_CALL        0x04
#define XDEBUG_BREAKPOINT_TYPE_RETURN      0x08
#define XDEBUG_BREAKPOINT_TYPE_EXCEPTION   0x10
#define XDEBUG_BREAKPOINT_TYPE_WATCH       0x20

#define XDEBUG_BREAKPOINT_TYPE_NAME(v) (xdebug_breakpoint_types[(int)(log2(v))]).name

struct _xdebug_brk_admin {
	int   id;
	int   type;
	char *key;
};

struct _xdebug_con {
	int                    socket;
	void                  *options;
	xdebug_remote_handler *handler;
	fd_buf                *buffer;
	char                  *program_name;
	xdebug_hash           *breakpoint_list;
	xdebug_hash           *function_breakpoints;
	xdebug_hash           *eval_id_lookup;
	int                    eval_id_sequence;
	xdebug_llist          *line_breakpoints;
	xdebug_hash           *exception_breakpoints;
	xdebug_debug_list      list;
	int                    do_break;

	int                    do_step;
	int                    do_next;
	int                    next_level;
	int                    do_finish;
	int                    finish_level;
	int                    finish_func_nr;

	int                    send_notifications;
	int                    inhibit_notifications;

	int                    resolved_breakpoints;
};

#define XDEBUG_BRK_UNRESOLVED     0
#define XDEBUG_BRK_RESOLVED       1

#define XDEBUG_HIT_DISABLED       0
#define XDEBUG_HIT_GREATER_EQUAL  1
#define XDEBUG_HIT_EQUAL          2
#define XDEBUG_HIT_MOD            3

#define XDEBUG_RESOLVED_SPAN_MIN          -1
#define XDEBUG_RESOLVED_SPAN_MAX  4294967295

struct _xdebug_brk_span {
	int start;
	int end;
};

struct _xdebug_brk_info {
	int                   id;
	int                   brk_type;
	int                   resolved;
	char                 *classname;
	char                 *functionname;
	char                 *exceptionname;
	int                   function_break_type; /* XDEBUG_BRK_FUNC_* */
	char                 *file;
	int                   file_len;
	int                   original_lineno; /* line number that was set through breakpoint_set */
	int                   resolved_lineno; /* line number after resolving, initialised with 'original_lineno' */
	xdebug_brk_span       resolved_span;
	char                 *condition;
	int                   disabled;
	int                   temporary;
	int                   hit_count;
	int                   hit_value;
	int                   hit_condition;
};

struct _xdebug_eval_info {
	int   id;
	int   refcount;
	char *contents;
};

struct _xdebug_remote_handler {
	/* Init / deinit */
	int (*remote_init)(xdebug_con *h, int mode);
	int (*remote_deinit)(xdebug_con *h);

	/* Stack messages */
	int (*remote_error)(xdebug_con *h, int type, char *exception_type, char *message, const char *location, const unsigned int line, xdebug_llist *stack);

	/* Breakpoints */
	int (*break_on_line)(xdebug_con *h, xdebug_brk_info *brk, const char *file, int filename_len, int lineno);
	int (*remote_breakpoint)(xdebug_con *h, xdebug_llist *stack, char *file, long lineno, int type, char *exception, char *code, char *message);
	int (*resolve_breakpoints)(xdebug_con *h, int type, void *data);

	/* Output redirection */
	int (*remote_stream_output)(const char *string, unsigned int length TSRMLS_DC);

	/* Notifications & Logging */
	int (*remote_notification)(xdebug_con *h, const char *file, long lineno, int type, char *type_string, char *message TSRMLS_DC);
	void XDEBUG_ATTRIBUTE_FORMAT(printf, 2, 3) (*log)(int log_level, const char *fmt, ...);

	/* Eval ID registration and removal */
	int (*register_eval_id)(xdebug_con *h, function_stack_entry *fse);
};

struct _xdebug_remote_handler_info {
	const char            *name;
	const char            *description;
	xdebug_remote_handler  handler;
};

xdebug_remote_handler* xdebug_handler_get(char* mode);
xdebug_remote_handler_info* xdebug_handlers_get(void);

void xdebug_brk_info_dtor(xdebug_brk_info *brk);
void xdebug_llist_brk_dtor(void *dummy, xdebug_brk_info *brk);
void xdebug_hash_brk_dtor(xdebug_brk_info *brk);
void xdebug_hash_eval_info_dtor(xdebug_eval_info *ei);

#endif
