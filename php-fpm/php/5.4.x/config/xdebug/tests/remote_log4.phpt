--TEST--
Test for Xdebug's remote log (with xdebug.remote_addr_header value)
--ENV--
I_LIKE_COOKIES=doesnotexist3
--INI--
xdebug.remote_enable=1
xdebug.remote_log=/tmp/remote-log4.txt
xdebug.remote_autostart=1
xdebug.remote_connect_back=1
xdebug.remote_host=doesnotexist2
xdebug.remote_port=9003
xdebug.remote_addr_header=I_LIKE_COOKIES
--FILE--
<?php
echo strlen("foo"), "\n";
echo file_get_contents("/tmp/remote-log4.txt");
unlink ("/tmp/remote-log4.txt");
?>
--EXPECTF--
3
Log opened at %d-%d-%d %d:%d:%d
I: Checking remote connect back address.
I: Checking user configured header 'I_LIKE_COOKIES'.
I: Remote address found, connecting to doesnotexist3:9003.
E: Could not connect to client. :-(
Log closed at %d-%d-%d %d:%d:%d
