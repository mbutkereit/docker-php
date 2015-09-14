--TEST--
Test for segmentation fault with unusual variables (< PHP 5.2)
--SKIPIF--
<?php if (!version_compare(phpversion(), "5.2", '<')) echo "skip < PHP 5.2 needed\n"; ?>
--INI--
xdebug.default_enable=1
xdebug.auto_trace=0
xdebug.collect_params=1
xdebug.collect_assignments=0
xdebug.profiler_enable=0
xdebug.show_local_vars=0
--FILE--
<?php
	${1} = "foo";
	echo ${1} . "\n";

	${STDIN} = "foo";
	echo ${STDIN} . "\n";

	${array(1,2,3)} = "foo";
	echo ${array(1,2,3)} . "\n";

	${new stdclass} = "foo";
	echo ${new stdclass} . "\n";
?>
--EXPECTF--
foo
foo

Notice: Array to string conversion in %sbug00032.php on line 8

Call Stack:
%w%f %w%d   1. {main}() %sbug00032.php:0


Notice: Array to string conversion in %sbug00032.php on line 9

Call Stack:
%w%f %w%d   1. {main}() %sbug00032.php:0

foo

Notice: Object of class stdClass to string conversion in %sbug00032.php on line 11

Call Stack:
%w%f %w%d   1. {main}() %sbug00032.php:0


Notice: Object of class stdClass to string conversion in %sbug00032.php on line 12

Call Stack:
%w%f %w%d   1. {main}() %sbug00032.php:0

foo
