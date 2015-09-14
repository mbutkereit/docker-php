--TEST--
Test for bug #389: Destructors called on fatal error 
--INI--
log_errors=0
xdebug.default_enable=1
xdebug.dump.GET=
xdebug.dump.SERVER=
xdebug.show_local_vars=0
--FILE--
<?php
class Food {
    public function __destruct() {
        echo "__destruct called\n";
    }
  }

$f = new Food();
echo "forcing fatal error:\n";
Abc::def();

?>
DONE
--EXPECTF--
forcing fatal error:

Fatal error: Class 'Abc' not found in %sbug00389.php on line 10

Call Stack:
%w%f%w%d   1. {main}() %sbug00389.php:0
