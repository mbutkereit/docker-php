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

#include "php.h"
#include "TSRM.h"
#include "php_globals.h"
#include "php_xdebug.h"
#include "Zend/zend_alloc.h"
#include "xdebug_mm.h"
#include "xdebug_profiler.h"
#include "xdebug_str.h"
#include "xdebug_var.h"
#include "usefulstuff.h"
#ifdef PHP_WIN32
#include <process.h>
#endif

ZEND_EXTERN_MODULE_GLOBALS(xdebug)

void xdebug_profile_aggr_call_entry_dtor(void *elem)
{
	xdebug_aggregate_entry *xae = (xdebug_aggregate_entry *) elem;
	if (xae->filename) {
		xdfree(xae->filename);
	}
	if (xae->function) {
		xdfree(xae->function);
	}
}

void xdebug_profile_call_entry_dtor(void *dummy, void *elem)
{
	xdebug_call_entry *ce = elem;

	if (ce->function) {
		xdfree(ce->function);
	}
	if (ce->filename) {
		xdfree(ce->filename);
	}
	xdfree(ce);
}

static void profiler_write_header(FILE *file, char *script_name)
{
	if (XG(profiler_append)) {
		fprintf(file, "\n==== NEW PROFILING FILE ==============================================\n");
	}
	fprintf(file, "version: 1\ncreator: xdebug %s (PHP %s)\n", XDEBUG_VERSION, PHP_VERSION);
	fprintf(file, "cmd: %s\npart: 1\npositions: line\n\n", script_name);
	fprintf(file, "events: Time Memory\n\n");
	fflush(file);
}

void xdebug_profiler_init(char *script_name)
{
	char *filename = NULL, *fname = NULL;

	if (XG(profiler_enabled)) {
		return;
	}

	if (!strlen(XG(profiler_output_name)) ||
		xdebug_format_output_filename(&fname, XG(profiler_output_name), script_name) <= 0
	) {
		/* Invalid or empty xdebug.profiler_output_name */
		return;
	}

	/* Add a slash if none is present in the profiler_output_dir setting */
	if (IS_SLASH(XG(profiler_output_dir)[strlen(XG(profiler_output_dir)) - 1])) {
		filename = xdebug_sprintf("%s%s", XG(profiler_output_dir), fname);
	} else {
		filename = xdebug_sprintf("%s%c%s", XG(profiler_output_dir), DEFAULT_SLASH, fname);
	}
	xdfree(fname);

	if (XG(profiler_append)) {
		XG(profile_file) = xdebug_fopen(filename, "a", NULL, &XG(profile_filename));
	} else {
		XG(profile_file) = xdebug_fopen(filename, "w", NULL, &XG(profile_filename));
	}
	xdfree(filename);

	if (!XG(profile_file)) {
		return;
	}

	profiler_write_header(XG(profile_file), script_name);

	if (!SG(headers_sent)) {
		sapi_header_line ctr = {0};

		ctr.line = xdebug_sprintf("X-Xdebug-Profile-Filename: %s", XG(profile_filename));
		ctr.line_len = strlen(ctr.line);
		sapi_header_op(SAPI_HEADER_REPLACE, &ctr);
		xdfree(ctr.line);
	}

	XG(profiler_start_time) = xdebug_get_utime();

	XG(profiler_enabled) = 1;
	XG(profile_filename_refs) = xdebug_hash_alloc(128, NULL);
	XG(profile_functionname_refs) = xdebug_hash_alloc(128, NULL);
	XG(profile_last_filename_ref) = 0;
	XG(profile_last_functionname_ref) = 0;
	return;
}

void xdebug_profiler_deinit()
{
	function_stack_entry *fse;
	xdebug_llist_element *le;

	for (le = XDEBUG_LLIST_TAIL(XG(stack)); le != NULL; le = XDEBUG_LLIST_PREV(le)) {
		fse = XDEBUG_LLIST_VALP(le);
		xdebug_profiler_function_end(fse TSRMLS_CC);
	}
		
	fprintf(
		XG(profile_file),
		"summary: %lu %zd\n\n",
		(unsigned long) ((xdebug_get_utime() - (XG(profiler_start_time))) * 1000000),
		zend_memory_peak_usage(0)
	);

	XG(profiler_enabled) = 0;

	fflush(XG(profile_file));

	if (XG(profile_file)) {
		fclose(XG(profile_file));
		XG(profile_file) = NULL;
	}

	if (XG(profile_filename)) {
		xdfree(XG(profile_filename));
	}

	xdebug_hash_destroy(XG(profile_filename_refs));
	xdebug_hash_destroy(XG(profile_functionname_refs));
	XG(profile_filename_refs) = NULL;
	XG(profile_functionname_refs) = NULL;
}

static inline void xdebug_profiler_function_push(function_stack_entry *fse)
{
	fse->profile.time += xdebug_get_utime();
	fse->profile.time -= fse->profile.mark;
	fse->profile.mark = 0;
	fse->profile.memory += zend_memory_usage(0 TSRMLS_CC);
	fse->profile.memory -= fse->profile.mem_mark;
	fse->profile.mem_mark = 0;
}

void xdebug_profiler_function_continue(function_stack_entry *fse)
{
	fse->profile.mark = xdebug_get_utime();
}

void xdebug_profiler_function_pause(function_stack_entry *fse)
{
	xdebug_profiler_function_push(fse);
}

static char* get_filename_ref(char *name TSRMLS_DC)
{
	long nr;

	if (xdebug_hash_find(XG(profile_filename_refs), name, strlen(name), (void*) &nr)) {
		return xdebug_sprintf("(%d)", nr);
	} else {
		XG(profile_last_filename_ref)++;
		xdebug_hash_add(XG(profile_filename_refs), name, strlen(name), (void*) (size_t) XG(profile_last_filename_ref));
		return xdebug_sprintf("(%d) %s", XG(profile_last_filename_ref), name);
	}
}

static char* get_functionname_ref(char *name TSRMLS_DC)
{
	long nr;

	if (xdebug_hash_find(XG(profile_functionname_refs), name, strlen(name), (void*) &nr)) {
		return xdebug_sprintf("(%d)", nr);
	} else {
		XG(profile_last_functionname_ref)++;
		xdebug_hash_add(XG(profile_functionname_refs), name, strlen(name), (void*) (size_t) XG(profile_last_functionname_ref));
		return xdebug_sprintf("(%d) %s", XG(profile_last_functionname_ref), name);
	}
}

void xdebug_profiler_add_function_details_user(function_stack_entry *fse, zend_op_array *op_array TSRMLS_DC)
{
	char *tmp_fname, *tmp_name;

	tmp_name = xdebug_show_fname(fse->function, 0, 0 TSRMLS_CC);
	switch (fse->function.type) {
		case XFUNC_INCLUDE:
		case XFUNC_INCLUDE_ONCE:
		case XFUNC_REQUIRE:
		case XFUNC_REQUIRE_ONCE:
			tmp_fname = xdebug_sprintf("%s::%s", tmp_name, fse->include_filename);
			xdfree(tmp_name);
			tmp_name = tmp_fname;
			fse->profiler.lineno = 1;
			break;

		default:
			if (op_array/* && op_array->function_name*/) {
				fse->profiler.lineno = fse->op_array->line_start;
			} else {
				fse->profiler.lineno = fse->lineno;
			}
			break;
	}
	if (fse->profiler.lineno == 0) {
		fse->profiler.lineno = 1;
	}

	if (op_array && op_array->filename) {
		fse->profiler.filename = xdstrdup((char*) STR_NAME_VAL(op_array->filename));
	} else {
		fse->profiler.filename = xdstrdup(fse->filename);
	}
	fse->profiler.funcname = xdstrdup(tmp_name);
	xdfree(tmp_name);
}

void xdebug_profiler_add_function_details_internal(function_stack_entry *fse TSRMLS_DC)
{
	char *tmp_fname, *tmp_name;

	tmp_name = xdebug_show_fname(fse->function, 0, 0 TSRMLS_CC);
	switch (fse->function.type) {
		case XFUNC_INCLUDE:
		case XFUNC_INCLUDE_ONCE:
		case XFUNC_REQUIRE:
		case XFUNC_REQUIRE_ONCE:
			tmp_fname = xdebug_sprintf("%s::%s", tmp_name, fse->include_filename);
			xdfree(tmp_name);
			tmp_name = tmp_fname;
			fse->profiler.lineno = 1;
			break;

		default:
			fse->profiler.lineno = fse->lineno;
			break;
	}
	if (fse->profiler.lineno == 0) {
		fse->profiler.lineno = 1;
	}

	fse->profiler.filename = xdstrdup(fse->filename);
	fse->profiler.funcname = xdstrdup(tmp_name);

	xdfree(tmp_name);
}

void xdebug_profiler_function_begin(function_stack_entry *fse TSRMLS_DC)
{
	fse->profile.time = 0;
	fse->profile.mark = xdebug_get_utime();
	fse->profile.memory = 0;
	fse->profile.mem_mark = zend_memory_usage(0 TSRMLS_CC);
}

void xdebug_profiler_function_end(function_stack_entry *fse TSRMLS_DC)
{
	xdebug_llist_element *le;

	if (fse->prev && !fse->prev->profile.call_list) {
		fse->prev->profile.call_list = xdebug_llist_alloc(xdebug_profile_call_entry_dtor);
	}
	if (!fse->profile.call_list) {
		fse->profile.call_list = xdebug_llist_alloc(xdebug_profile_call_entry_dtor);
	}
	xdebug_profiler_function_push(fse);

	if (fse->prev) {
		xdebug_call_entry *ce = xdmalloc(sizeof(xdebug_call_entry));
		ce->filename = xdstrdup(fse->profiler.filename);
		ce->function = xdstrdup(fse->profiler.funcname);
		ce->time_taken = fse->profile.time;
		ce->lineno = fse->lineno;
		ce->user_defined = fse->user_defined;
		ce->mem_used = fse->profile.memory;

		xdebug_llist_insert_next(fse->prev->profile.call_list, NULL, ce);
	}

	/* use previously created filename and funcname (or a reference to them) to show
	 * time spend */
	if (fse->user_defined == XDEBUG_BUILT_IN) {
		char *tmp_key = xdebug_sprintf("php::%s", fse->profiler.funcname);
		char *fl_ref = NULL, *fn_ref = NULL;

		fl_ref = get_filename_ref((char*) "php:internal" TSRMLS_CC);
		fn_ref = get_functionname_ref(tmp_key TSRMLS_CC);

		fprintf(XG(profile_file), "fl=%s\n", fl_ref);
		fprintf(XG(profile_file), "fn=%s\n", fn_ref);

		xdfree(fl_ref);
		xdfree(fn_ref);
		xdfree(tmp_key);
	} else {
		char *fl_ref = NULL, *fn_ref = NULL;

		fl_ref = get_filename_ref(fse->profiler.filename TSRMLS_CC);
		fn_ref = get_functionname_ref(fse->profiler.funcname TSRMLS_CC);

		fprintf(XG(profile_file), "fl=%s\n", fl_ref);
		fprintf(XG(profile_file), "fn=%s\n", fn_ref);

		xdfree(fl_ref);
		xdfree(fn_ref);
	}


	/* update aggregate data */
	if (XG(profiler_aggregate)) {
		fse->aggr_entry->time_inclusive += fse->profile.time;
		fse->aggr_entry->call_count++;
	}

	/* Subtract time in calledfunction from time here */
	for (le = XDEBUG_LLIST_HEAD(fse->profile.call_list); le != NULL; le = XDEBUG_LLIST_NEXT(le))
	{
		xdebug_call_entry *call_entry = XDEBUG_LLIST_VALP(le);
		fse->profile.time -= call_entry->time_taken;
		fse->profile.memory -= call_entry->mem_used;
	}
	fprintf(XG(profile_file), "%d %lu %ld\n", fse->profiler.lineno, (unsigned long) (fse->profile.time * 1000000), (fse->profile.memory));

	/* update aggregate data */
	if (XG(profiler_aggregate)) {
		fse->aggr_entry->time_own += fse->profile.time;
		fse->aggr_entry->mem_used += fse->profile.memory;
	}

	/* dump call list */
	for (le = XDEBUG_LLIST_HEAD(fse->profile.call_list); le != NULL; le = XDEBUG_LLIST_NEXT(le))
	{
		char *fl_ref = NULL, *fn_ref = NULL;
		xdebug_call_entry *call_entry = XDEBUG_LLIST_VALP(le);

		if (call_entry->user_defined == XDEBUG_BUILT_IN) {
			char *tmp_key = xdebug_sprintf("php::%s", call_entry->function);

			fl_ref = get_filename_ref((char*) "php:internal" TSRMLS_CC);
			fn_ref = get_functionname_ref(tmp_key TSRMLS_CC);

			xdfree(tmp_key);
		} else {
			fl_ref = get_filename_ref(call_entry->filename TSRMLS_CC);
			fn_ref = get_functionname_ref(call_entry->function TSRMLS_CC);
		}

		fprintf(XG(profile_file), "cfl=%s\n", fl_ref);
		fprintf(XG(profile_file), "cfn=%s\n", fn_ref);

		xdfree(fl_ref);
		xdfree(fn_ref);

		fprintf(XG(profile_file), "calls=1 0 0\n");
		fprintf(XG(profile_file), "%d %lu %ld\n", call_entry->lineno, (unsigned long) (call_entry->time_taken * 1000000), (call_entry->mem_used));
	}
	fprintf(XG(profile_file), "\n");
	fflush(XG(profile_file));
}

void xdebug_profiler_free_function_details(function_stack_entry *fse TSRMLS_DC)
{
	xdfree(fse->profiler.funcname);
	xdfree(fse->profiler.filename);
	fse->profiler.funcname = NULL;
	fse->profiler.filename = NULL;
}

static int xdebug_print_aggr_entry(zval *pDest, void *argument TSRMLS_DC)
{
	FILE *fp = (FILE *) argument;
	xdebug_aggregate_entry *xae = (xdebug_aggregate_entry *) pDest;

	fprintf(fp, "fl=%s\n", xae->filename);
	fprintf(fp, "fn=%s\n", xae->function);
	fprintf(fp, "%d %lu %ld\n", 0, (unsigned long) (xae->time_own * 1000000), (xae->mem_used));
	if (strcmp(xae->function, "{main}") == 0) {
		fprintf(fp, "\nsummary: %lu %lu\n\n", (unsigned long) (xae->time_inclusive * 1000000), (xae->mem_used));
	}
	if (xae->call_list) {
		xdebug_aggregate_entry *xae_call;

		ZEND_HASH_FOREACH_PTR(xae->call_list, xae_call) {
			fprintf(fp, "cfn=%s\n", (xae_call)->function);
			fprintf(fp, "calls=%d 0 0\n", (xae_call)->call_count);
			fprintf(fp, "%d %lu %ld\n", (xae_call)->lineno, (unsigned long) ((xae_call)->time_inclusive * 1000000), ((xae_call)->mem_used));
		} ZEND_HASH_FOREACH_END();
	}
	fprintf(fp, "\n");
	fflush(fp);

	return ZEND_HASH_APPLY_KEEP;
}

int xdebug_profiler_output_aggr_data(const char *prefix TSRMLS_DC)
{
	char *filename;
	FILE *aggr_file;

	fprintf(stderr, "in xdebug_profiler_output_aggr_data() with %d entries\n", zend_hash_num_elements(&XG(aggr_calls)));

	if (zend_hash_num_elements(&XG(aggr_calls)) == 0) return SUCCESS;

	if (prefix) {
		filename = xdebug_sprintf("%s/cachegrind.out.aggregate.%s." ZEND_ULONG_FMT, XG(profiler_output_dir), prefix, (zend_ulong) xdebug_get_pid());
	} else {
		filename = xdebug_sprintf("%s/cachegrind.out.aggregate." ZEND_ULONG_FMT, XG(profiler_output_dir), (zend_ulong) xdebug_get_pid());
	}

	fprintf(stderr, "opening %s\n", filename);
	aggr_file = xdebug_fopen(filename, "w", NULL, NULL);
	if (!aggr_file) {
		return FAILURE;
	}
	fprintf(aggr_file, "version: 0.9.6\ncmd: Aggregate\npart: 1\n\nevents: Time\n\n");
	fflush(aggr_file);
	zend_hash_apply_with_argument(&XG(aggr_calls), xdebug_print_aggr_entry, aggr_file TSRMLS_CC);
	fclose(aggr_file);
	fprintf(stderr, "wrote info for %d entries to %s\n", zend_hash_num_elements(&XG(aggr_calls)), filename);
	return SUCCESS;
}
