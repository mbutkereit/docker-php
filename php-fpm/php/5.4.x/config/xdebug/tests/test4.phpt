--TEST--
Test for simple backtrace
--INI--
xdebug.default_enable=1
xdebug.dump_globals=0
xdebug.show_mem_delta=0
xdebug.profiler_enable=0
xdebug.trace_format=0
--FILE--
<?php
	function a () {
		b();
	}

	function b () {
		c();
	}

	a();
?>
--EXPECTF--
Fatal error: Call to undefined function%sc() in /%s/test4.php on line 7

Call Stack:
%w%f %w%d   1. {main}() /%s/test4.php:0
%w%f %w%d   2. a() /%s/test4.php:10
%w%f %w%d   3. b() /%s/test4.php:3
