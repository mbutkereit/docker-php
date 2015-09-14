--TEST--
Test for overloaded member functions / classes
--INI--
xdebug.enable=1
xdebug.auto_trace=0
xdebug.auto_profile=0
xdebug.collect_params=3
xdebug.collect_return=0
xdebug.collect_assignments=0
xdebug.show_mem_delta=0
xdebug.profiler_enable=0
xdebug.trace_format=0
--FILE--
<?php
	$tf = xdebug_start_trace('/tmp/'. uniqid('xdt', TRUE));

	class a {

		function func_a1() {
		}

		function func_a2() {
		}

	}

	class b extends a {

		function func_b1() {
		}

		function func_b2() {
		}

	}

	$B = new b;
	$B->func_a1();
	$B->func_b1();

	echo file_get_contents($tf);
	unlink($tf);
?>
--EXPECTF--
TRACE START [%d-%d-%d %d:%d:%d]
%w%f %w%d     -> a->func_a1() /%s/test16b.php:25
%w%f %w%d     -> b->func_b1() /%s/test16b.php:26
%w%f %w%d     -> file_get_contents('/tmp/%s') /%s/test16b.php:28
