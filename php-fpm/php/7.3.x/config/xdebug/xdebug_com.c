/*
   +----------------------------------------------------------------------+
   | Xdebug                                                               |
   +----------------------------------------------------------------------+
   | Copyright (c) 2002-2018 Derick Rethans                               |
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
   |          Thomas Vanhaniemi <thomas.vanhaniemi@arcada.fi>             |
   +----------------------------------------------------------------------+
 */
#include "php_xdebug.h"
#include <string.h>
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#ifndef PHP_WIN32
# if HAVE_POLL_H
#  include <poll.h>
# elif HAVE_SYS_POLL_H
#  include <sys/poll.h>
# endif
# include <unistd.h>
# include <sys/socket.h>
# include <sys/un.h>
# include <netinet/tcp.h>
# if HAVE_NETINET_IN_H
#  include <netinet/in.h>
# endif
# include <netdb.h>
#else
# include <process.h>
# include <direct.h>
# include "win32/time.h"
# undef UNICODE
# include <winsock2.h>
# include <ws2tcpip.h>
# include <mstcpip.h>
# pragma comment (lib, "Ws2_32.lib")
# define PATH_MAX MAX_PATH
# define poll WSAPoll
#endif

#include "xdebug_private.h"
#include "xdebug_com.h"

ZEND_EXTERN_MODULE_GLOBALS(xdebug)

#if !WIN32 && !WINNT
static int xdebug_create_socket_unix(const char *path TSRMLS_DC)
{
	struct sockaddr_un sa;
	int sockfd;

	if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) == SOCK_ERR) {
		XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for 'unix://%s', socket: %s.\n", path, strerror(errno));
		return SOCK_ERR;
	}

	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
	if (connect(sockfd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
		XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for 'unix://%s', connect: %s.\n", path, strerror(errno));
		SCLOSE(sockfd);
		return (errno == EACCES) ? SOCK_ACCESS_ERR : SOCK_ERR;
	}

	/* Prevent the socket from being inherited by exec'd children */
	if (fcntl(sockfd, F_SETFD, FD_CLOEXEC) < 0) {
		XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for 'unix://%s', fcntl(FD_CLOEXEC): %s.\n", path, strerror(errno));
	}

	return sockfd;
}
#endif

int xdebug_create_socket(const char *hostname, int dport, int timeout TSRMLS_DC)
{
	struct addrinfo            hints;
	struct addrinfo            *remote;
	struct addrinfo            *ptr;
	int                        status;
	int                        sockfd = 0;
	int                        sockerror;
	char                       sport[10];
	int                        actually_connected;
	struct sockaddr_in6        sa;
	socklen_t                  size = sizeof(sa);
#if WIN32|WINNT
	WSAPOLLFD                  ufds[1] = {0};
	WORD                       wVersionRequested;
	WSADATA                    wsaData;
	char                       optval = 1;
	u_long                     yes = 1;
	u_long                     no = 0;

	wVersionRequested = MAKEWORD(2, 2);
	WSAStartup(wVersionRequested, &wsaData);
#else
	struct pollfd              ufds[1];
	long                       optval = 1;
#endif

	if (!strncmp(hostname, "unix://", strlen("unix://"))) {
#if WIN32|WINNT
		XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s', Unix domain socket not supported.\n", hostname);
		return SOCK_ERR;
#else
		return xdebug_create_socket_unix(hostname + strlen("unix://") TSRMLS_CC);
#endif
	}

	/* Make a string of the port number that can be used with getaddrinfo */
	sprintf(sport, "%d", dport);

	/* Create hints for getaddrinfo saying that we want IPv4 and IPv6 TCP stream sockets */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	/* Call getaddrinfo and return SOCK_ERR if the call fails for some reason */
	if ((status = getaddrinfo(hostname, sport, &hints, &remote)) != 0) {
#if WIN32|WINNT
		XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', getaddrinfo: %d.\n", hostname, dport, WSAGetLastError());
#else
		XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', getaddrinfo: %s.\n", hostname, dport, strerror(errno));
#endif
		return SOCK_ERR;
	}

	/* Go through every returned IP address */
	for (ptr = remote; ptr != NULL; ptr = ptr->ai_next) {
		/* Try to create the socket. If the creation fails continue on with the
		 * next IP address in the list */
		if ((sockfd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol)) == SOCK_ERR) {
#if WIN32|WINNT
			XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', socket: %d.\n", hostname, dport, WSAGetLastError());
#else
			XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', socket: %s.\n", hostname, dport, strerror(errno));
#endif
			continue;
		}

		/* Put socket in non-blocking mode so we can use poll for timeouts */
#ifdef WIN32
		status = ioctlsocket(sockfd, FIONBIO, &yes);
		if (SOCKET_ERROR == status) {
			XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', FIONBIO: %d.\n", hostname, dport, WSAGetLastError());
		}
#else
		fcntl(sockfd, F_SETFL, O_NONBLOCK);
#endif

#if !WIN32 && !WINNT
		/* Prevent the socket from being inherited by exec'd children on *nix (not necessary on Win) */
		if (fcntl(sockfd, F_SETFD, FD_CLOEXEC) < 0) {
			XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', fcntl(FD_CLOEXEC): %s.\n", hostname, dport, strerror(errno));
		}
#endif

		/* Try to connect to the newly created socket */
		/* Worth noting is that the port is set in the getaddrinfo call before */
		status = connect(sockfd, ptr->ai_addr, ptr->ai_addrlen);

		/* Determine if we got a connection. If no connection could be made
		 * we close the socket and continue with the next IP address in the list */
		if (status < 0) {
#ifdef WIN32
			errno = WSAGetLastError();
			if (errno != WSAEINPROGRESS && errno != WSAEWOULDBLOCK) {
				XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', connect: %d.\n", hostname, dport, errno);
#else
			if (errno == EACCES) {
				XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', connect: %s.\n", hostname, dport, strerror(errno));
				SCLOSE(sockfd);
				sockfd = SOCK_ACCESS_ERR;

				continue;
			}
			if (errno != EINPROGRESS) {
				XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', connect: %s.\n", hostname, dport, strerror(errno));
#endif
				SCLOSE(sockfd);
				sockfd = SOCK_ERR;

				continue;
			}

			ufds[0].fd = sockfd;
#if WIN32|WINNT
			ufds[0].events = POLLIN | POLLOUT;
#else
			ufds[0].events = POLLIN | POLLOUT | POLLPRI;
#endif
			while (1) {
				sockerror = poll(ufds, 1, timeout);

#if WIN32|WINNT
				errno = WSAGetLastError();
				if (errno == WSAEINPROGRESS || errno == WSAEWOULDBLOCK) {
					/* XXX introduce retry count? */
					continue;
				}
#endif
				/* If an error occured when doing the poll */
				if (sockerror == SOCK_ERR) {
#if WIN32|WINNT
					XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', WSAPoll error: %d (%d, %d).\n", hostname, dport, WSAGetLastError(), sockerror, errno);
#else
					XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', poll error: %s (%d).\n", hostname, dport, strerror(errno), sockerror);
#endif
					sockerror = SOCK_ERR;
					break;
				}

				/* A timeout occured when polling the socket */
				if (sockerror == 0) {
					sockerror = SOCK_TIMEOUT_ERR;
					break;
				}

				/* If the poll was successful but an error occured */
				if (ufds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
#if WIN32|WINNT
					XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', WSAPoll success, but error: %d (%d).\n", hostname, dport, WSAGetLastError(), ufds[0].revents);
#else
					XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', poll success, but error: %s (%d).\n", hostname, dport, strerror(errno), ufds[0].revents);
#endif
					sockerror = SOCK_ERR;
					break;
				}

				/* If the poll was successful break out */
				if (ufds[0].revents & (POLLIN | POLLOUT)) {
					sockerror = sockfd;
					break;
				} else {
					/* We should never get here, but added as a failsafe to break out from any loops */
#if WIN32|WINNT
					XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', WSAPoll: %d.\n", hostname, dport, WSAGetLastError());
#else
					XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', poll: %s.\n", hostname, dport, strerror(errno));
#endif
					sockerror = SOCK_ERR;
					break;
				}
			}

			if (sockerror > 0) {
				actually_connected = getpeername(sockfd, (struct sockaddr *)&sa, &size);
				if (actually_connected == -1) {
#if WIN32|WINNT
					XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', getpeername: %d.\n", hostname, dport, WSAGetLastError());
#else
					XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', getpeername: %s.\n", hostname, dport, strerror(errno));
#endif
					sockerror = SOCK_ERR;
				}
			}

			/* If there where some errors close the socket and continue with the next IP address */
			if (sockerror < 0) {
				SCLOSE(sockfd);
				sockfd = sockerror;

				continue;
			}
		}

		break;
	}

	/* Free the result returned by getaddrinfo */
	freeaddrinfo(remote);

	/* If we got a socket, set the option "No delay" to true (1) */
	if (sockfd > 0) {
#ifdef WIN32
		status = ioctlsocket(sockfd, FIONBIO, &no);
		if (SOCKET_ERROR == status) {
			XG(context).handler->log(XDEBUG_LOG_WARN, "Creating socket for '%s:%d', FIONBIO: %d.\n", hostname, dport, WSAGetLastError());
		}
#else
		fcntl(sockfd, F_SETFL, 0);
#endif

		setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
	}

	return sockfd;
}

void xdebug_close_socket(int socketfd)
{
	SCLOSE(socketfd);
}

/* Remote debugger helper functions */
int xdebug_handle_hit_value(xdebug_brk_info *brk_info)
{
	/* If this is a temporary breakpoint, disable the breakpoint */
	if (brk_info->temporary) {
		brk_info->disabled = 1;
	}

	/* Increase hit counter */
	brk_info->hit_count++;

	/* If the hit_value is 0, the condition check is disabled */
	if (!brk_info->hit_value) {
		return 1;
	}

	switch (brk_info->hit_condition) {
		case XDEBUG_HIT_GREATER_EQUAL:
			if (brk_info->hit_count >= brk_info->hit_value) {
				return 1;
			}
			break;
		case XDEBUG_HIT_EQUAL:
			if (brk_info->hit_count == brk_info->hit_value) {
				return 1;
			}
			break;
		case XDEBUG_HIT_MOD:
			if (brk_info->hit_count % brk_info->hit_value == 0) {
				return 1;
			}
			break;
		case XDEBUG_HIT_DISABLED:
			return 1;
			break;
	}
	return 0;
}

/* Log related functions */
static void xdebug_open_log(void)
{
	zend_ulong pid = xdebug_get_pid();

	/* initialize remote log file */
	XG(remote_log_file) = NULL;
	if (XG(remote_log) && strlen(XG(remote_log))) {
		XG(remote_log_file) = xdebug_fopen(XG(remote_log), "a", NULL, NULL);
	}
	if (XG(remote_log_file)) {
		char *timestr = xdebug_get_time();
		fprintf(XG(remote_log_file), "[" ZEND_ULONG_FMT "] Log opened at %s\n", pid, timestr);
		fflush(XG(remote_log_file));
		xdfree(timestr);
	} else if (strlen(XG(remote_log))) {
		php_log_err(xdebug_sprintf("Xdebug could not open the remote debug file '%s'.", XG(remote_log)));
	}
}

static void xdebug_close_log()
{

	if (XG(remote_log_file)) {
		zend_ulong pid = xdebug_get_pid();
		char *timestr = xdebug_get_time();

		fprintf(XG(remote_log_file), "[" ZEND_ULONG_FMT "] Log closed at %s\n\n", pid, timestr);
		fflush(XG(remote_log_file));
		xdfree(timestr);
		fclose(XG(remote_log_file));
		XG(remote_log_file) = NULL;
	}
}

/* Starting the debugger */
static void xdebug_init_debugger()
{
	xdebug_open_log();

	/* Get handler from mode */
	XG(context).handler = xdebug_handler_get(XG(remote_handler));
	if (!XG(context).handler) {
		zend_error(E_WARNING, "The remote debug handler '%s' is not supported", XG(remote_handler));
		return;
	}

	if (XG(remote_connect_back)) {
		zval *remote_addr = NULL;

		XG(context).handler->log(XDEBUG_LOG_INFO, "Checking remote connect back address.\n");
		if (XG(remote_addr_header) && XG(remote_addr_header)[0]) {
			XG(context).handler->log(XDEBUG_LOG_INFO, "Checking user configured header '%s'.\n", XG(remote_addr_header));
			remote_addr = zend_hash_str_find(Z_ARRVAL(PG(http_globals)[TRACK_VARS_SERVER]), XG(remote_addr_header), HASH_KEY_STRLEN(XG(remote_addr_header)));
		}
		if (!remote_addr) {
			XG(context).handler->log(XDEBUG_LOG_INFO, "Checking header 'HTTP_X_FORWARDED_FOR'.\n");
			remote_addr = zend_hash_str_find(Z_ARRVAL(PG(http_globals)[TRACK_VARS_SERVER]), "HTTP_X_FORWARDED_FOR", HASH_KEY_SIZEOF("HTTP_X_FORWARDED_FOR"));
		}
		if (!remote_addr) {
			XG(context).handler->log(XDEBUG_LOG_INFO, "Checking header 'REMOTE_ADDR'.\n");
			remote_addr = zend_hash_str_find(Z_ARRVAL(PG(http_globals)[TRACK_VARS_SERVER]), "REMOTE_ADDR", HASH_KEY_SIZEOF("REMOTE_ADDR"));
		}

		if (remote_addr && strstr(Z_STRVAL_P(remote_addr), "://")) {
			XG(context).handler->log(XDEBUG_LOG_WARN, "Invalid remote address provided containing URI spec '%s'.\n", Z_STRVAL_P(remote_addr));
			remote_addr = NULL;
		}

		if (remote_addr) {
			char *cp = NULL;
			int   cp_found = 0;

			/* Use first IP according to RFC 7239 */
			cp = strchr(Z_STRVAL_P(remote_addr), ',');
			if (cp) {
				*cp = '\0';
				cp_found = 1;
			}

			XG(context).handler->log(XDEBUG_LOG_INFO, "Remote address found, connecting to %s:%ld.\n", Z_STRVAL_P(remote_addr), (long int) XG(remote_port));
			XG(context).socket = xdebug_create_socket(Z_STRVAL_P(remote_addr), XG(remote_port), XG(remote_connect_timeout));

			/* Replace the ',', in case we had changed the original header due
			 * to multiple values */
			if (cp_found) {
				*cp = ',';
			}
		} else {
			XG(context).handler->log(XDEBUG_LOG_WARN, "Remote address not found, connecting to configured address/port: %s:%ld. :-|\n", XG(remote_host), (long int) XG(remote_port));
			XG(context).socket = xdebug_create_socket(XG(remote_host), XG(remote_port), XG(remote_connect_timeout));
		}
	} else {
		XG(context).handler->log(XDEBUG_LOG_INFO, "Connecting to configured address/port: %s:%ld.\n", XG(remote_host), (long int) XG(remote_port));
		XG(context).socket = xdebug_create_socket(XG(remote_host), XG(remote_port), XG(remote_connect_timeout));
	}
	if (XG(context).socket >= 0) {
		XG(context).handler->log(XDEBUG_LOG_INFO, "Connected to client. :-)\n");
		xdebug_mark_debug_connection_pending();

		if (!XG(context).handler->remote_init(&(XG(context)), XDEBUG_REQ)) {
			/* The request could not be started, ignore it then */
			XG(context).handler->log(XDEBUG_LOG_ERR, "The debug session could not be started. :-(\n");
		} else {
			/* All is well, turn off script time outs */
			zend_string *ini_name = zend_string_init("max_execution_time", sizeof("max_execution_time") - 1, 0);
			zend_string *ini_val = zend_string_init("0", sizeof("0") - 1, 0);

			zend_alter_ini_entry(ini_name, ini_val, PHP_INI_SYSTEM, PHP_INI_STAGE_ACTIVATE);

			zend_string_release(ini_val);
			zend_string_release(ini_name);
		}
	} else if (XG(context).socket == -1) {
		XG(context).handler->log(XDEBUG_LOG_ERR, "Could not connect to client. :-(\n");
	} else if (XG(context).socket == -2) {
		XG(context).handler->log(XDEBUG_LOG_ERR, "Time-out connecting to client (Waited: " ZEND_LONG_FMT " ms). :-(\n", XG(remote_connect_timeout));
	} else if (XG(context).socket == -3) {
		XG(context).handler->log(XDEBUG_LOG_ERR, "No permission connecting to client. This could be SELinux related. :-(\n");
	}
	if (!XG(remote_connection_enabled)) {
		xdebug_close_log();
	}
}

void xdebug_abort_debugger()
{
	if (XG(remote_connection_enabled)) {
		xdebug_mark_debug_connection_not_active();
	}
}

void xdebug_restart_debugger()
{
	xdebug_abort_debugger();
	xdebug_init_debugger();
}

/* Remote connection activation and house keeping */
int xdebug_is_debug_connection_active()
{
	return (
		XG(remote_connection_enabled)
	);
}

int xdebug_is_debug_connection_active_for_current_pid()
{
	zend_ulong pid;

	/* Early return to save some getpid() calls */
	if (!xdebug_is_debug_connection_active()) {
		return 0;
	}

	pid = xdebug_get_pid();

	/* Start debugger if previously a connection was established and this
	 * process no longer has the same PID */
	if (XG(remote_connection_pid) != pid) {
		xdebug_restart_debugger();
	}

	return (
		XG(remote_connection_enabled) && (XG(remote_connection_pid) == pid)
	);
}

void xdebug_mark_debug_connection_active()
{
	XG(remote_connection_enabled) = 1;
	XG(remote_connection_pid) = xdebug_get_pid();
}

void xdebug_mark_debug_connection_pending()
{
	XG(remote_connection_enabled) = 0;
	XG(remote_connection_pid) = 0;
}

void xdebug_mark_debug_connection_not_active()
{
	if (XG(remote_connection_enabled)) {
		xdebug_close_socket(XG(context).socket);
		xdebug_close_log();
	}

	XG(remote_connection_enabled) = 0;
	XG(remote_connection_pid) = 0;
}

void xdebug_do_jit()
{
	if (
		(XG(remote_mode) == XDEBUG_JIT) &&
		!xdebug_is_debug_connection_active_for_current_pid() &&
		XG(remote_enable)
	) {
		xdebug_init_debugger();
	}
}

static void xdebug_update_ide_key(char *new_key)
{
	if (XG(ide_key)) {
		xdfree(XG(ide_key));
	}
	XG(ide_key) = xdstrdup(new_key);
}

static int xdebug_handle_start_session()
{
	int   activate_session = 0;
	zval *dummy;

	/* Set session cookie if requested */
	if (
		((
			(dummy = zend_hash_str_find(Z_ARR(PG(http_globals)[TRACK_VARS_GET]), "XDEBUG_SESSION_START", sizeof("XDEBUG_SESSION_START") - 1)) != NULL
		) || (
			(dummy = zend_hash_str_find(Z_ARR(PG(http_globals)[TRACK_VARS_POST]), "XDEBUG_SESSION_START", sizeof("XDEBUG_SESSION_START") - 1)) != NULL
		))
		&& !SG(headers_sent)
	) {
		convert_to_string_ex(dummy);
		xdebug_update_ide_key(Z_STRVAL_P(dummy));
		
		xdebug_setcookie("XDEBUG_SESSION", sizeof("XDEBUG_SESSION") - 1, Z_STRVAL_P(dummy), Z_STRLEN_P(dummy), time(NULL) + XG(remote_cookie_expire_time), "/", 1, NULL, 0, 0, 1, 0);
		activate_session = 1;
	} else if (
		(dummy = zend_hash_str_find(Z_ARR(PG(http_globals)[TRACK_VARS_COOKIE]), "XDEBUG_SESSION", sizeof("XDEBUG_SESSION") - 1)) != NULL
	) {
		convert_to_string_ex(dummy);
		xdebug_update_ide_key(Z_STRVAL_P(dummy));
		
		activate_session = 1;
	} else if (getenv("XDEBUG_CONFIG")) {
		if (XG(ide_key) && *XG(ide_key) && !SG(headers_sent)) {
			xdebug_setcookie("XDEBUG_SESSION", sizeof("XDEBUG_SESSION") - 1, XG(ide_key), strlen(XG(ide_key)), time(NULL) + XG(remote_cookie_expire_time), "/", 1, NULL, 0, 0, 1, 0);
		}
		activate_session = 1;
	}

	return activate_session;
}

static void xdebug_handle_stop_session()
{
	/* Remove session cookie if requested */
	if (
		((
			zend_hash_str_find(Z_ARR(PG(http_globals)[TRACK_VARS_GET]), "XDEBUG_SESSION_STOP", sizeof("XDEBUG_SESSION_STOP") - 1) != NULL
		) || (
			zend_hash_str_find(Z_ARR(PG(http_globals)[TRACK_VARS_POST]), "XDEBUG_SESSION_STOP", sizeof("XDEBUG_SESSION_STOP") - 1) != NULL
		))
		&& !SG(headers_sent)
	) {
		xdebug_setcookie("XDEBUG_SESSION", sizeof("XDEBUG_SESSION") - 1, (char*) "", 0, time(NULL) + XG(remote_cookie_expire_time), "/", 1, NULL, 0, 0, 1, 0);
	}
}

void xdebug_do_req(void)
{
	if (XG(remote_mode) != XDEBUG_REQ) {
		return;
	}

	if (
		XG(remote_enable) &&
		!xdebug_is_debug_connection_active_for_current_pid() &&
		(XG(remote_autostart) || xdebug_handle_start_session())
	) {
		xdebug_init_debugger();
	}
	
	xdebug_handle_stop_session();
}
