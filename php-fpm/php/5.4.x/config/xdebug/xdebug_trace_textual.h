/*
   +----------------------------------------------------------------------+
   | Xdebug                                                               |
   +----------------------------------------------------------------------+
   | Copyright (c) 2002-2015 Derick Rethans                               |
   +----------------------------------------------------------------------+
   | This source file is subject to version 1.0 of the Xdebug license,    |
   | that is bundled with this package in the file LICENSE, and is        |
   | available at through the world-wide-web at                           |
   | http://xdebug.derickrethans.nl/license.php                           |
   | If you did not receive a copy of the Xdebug license and are unable   |
   | to obtain it through the world-wide-web, please send a note to       |
   | xdebug@derickrethans.nl so we can mail you a copy immediately.       |
   +----------------------------------------------------------------------+
   | Authors: Derick Rethans <derick@xdebug.org>                          |
   +----------------------------------------------------------------------+
 */
#ifndef XDEBUG_TRACE_TEXTUAL_H
#define XDEBUG_TRACE_TEXTUAL_H

#include "xdebug_tracing.h"

typedef struct _xdebug_trace_textual_context
{
	FILE *trace_file;
	char *trace_filename;
} xdebug_trace_textual_context;

extern xdebug_trace_handler_t xdebug_trace_handler_textual;
#endif
