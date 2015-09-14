--TEST--
Test for bug #631: Summary not written when script ended with "exit()"
--INI--
xdebug.profiler_enable=1
--FILE--
<?php
function capture() {
	echo file_get_contents(xdebug_get_profiler_filename());
}

register_shutdown_function('capture');
strlen("5");
exit();
?>
--EXPECTF--
version: 1
creator: xdebug 3.%s
cmd: %sbug00631.php
part: 1
positions: line

events: Time

fl=(1) php:internal
fn=(1) php::register_shutdown_function
%d %d

fl=(1)
fn=(2) php::strlen
%d %d

fl=(2) %sbug00631.php
fn=(3) {main}

summary: %d

%d %d
cfl=(1)
cfn=(1)
calls=1 0 0
%d %d
cfl=(1)
cfn=(2)
calls=1 0 0
%d %d

fl=(1)
fn=(4) php::xdebug_get_profiler_filename
%d %d
