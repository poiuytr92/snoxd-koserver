#pragma once

#include <queue>

/* Identify environment */

// current platform
#define PLATFORM_WIN32 0
#define PLATFORM_UNIX  1
#define PLATFORM_APPLE 2

#define UNIX_FLAVOUR_LINUX 1
#define UNIX_FLAVOUR_BSD 2
#define UNIX_FLAVOUR_OTHER 3
#define UNIX_FLAVOUR_OSX 4

/* Define the platform */
#if defined(__WIN32__) || defined(WIN32) || defined(_WIN32)

#	define PLATFORM PLATFORM_WIN32
#	define PLATFORM_TEXT "Win32"
#	define CONFIG_USE_IOCP

#	define VC_EXTRALEAN
#	define WIN32_LEAN_AND_MEAN

#	include <Windows.h>
#	include <winsock2.h>
#	include <ws2tcpip.h>

#else /* not a Windows environment */

#	include <sys/time.h>
#	include <sys/types.h>
#	include <sys/ioctl.h>
#	include <sys/socket.h>
#	include <netinet/in.h>
#	include <arpa/inet.h>
#	include <unistd.h>
#	include <signal.h>
#	include <netdb.h>
#	include <climits>

#	if defined(__APPLE_CC__)
#		define PLATFORM PLATFORM_APPLE
#	else
#		define PLATFORM PLATFORM_UNIX
#	endif

	/* Define the Unix flavour */
#	if defined(HAVE_DARWIN)
#		define UNIX_FLAVOUR UNIX_FLAVOUR_OSX
#		define PLATFORM_TEXT "MacOSX"
#		define CONFIG_USE_KQUEUE
#	elif defined(__FreeBSD__)
#		define UNIX_FLAVOUR UNIX_FLAVOUR_BSD
#		define PLATFORM_TEXT "BSD"
#		define CONFIG_USE_KQUEUE
#	else
#		define UNIX_FLAVOUR UNIX_FLAVOUR_LINUX
#		define PLATFORM_TEXT "Linux"
#		define CONFIG_USE_EPOLL
#	endif

#endif

/* Define the build we're compiling */
#ifdef _DEBUG
#		define BUILD_TYPE "Debug"
#		include <cassert>
#		include "DebugUtils.h"

#		define ASSERT assert
#		define TRACE FormattedDebugString
#else
#		define BUILD_TYPE "Release"
#		define ASSERT 
#		define TRACE 
#endif

/* Define the architecture we're compiling for */
#ifdef X64
#	define ARCH "X64"
#else
#	define ARCH "X86"
#endif

// Ignore the warning "nonstandard extension used: enum '<enum name>' used in qualified name"
// Sometimes it's necessary to avoid collisions, but aside from that, specifying the enumeration helps make code intent clearer.
#pragma warning(disable: 4482)


#define STR(str) #str
#define STRINGIFY(str) STR(str)

// If we support C++11, use experimental std::thread implementation
#if (__cplusplus >= 201103L) /* C++11 */  || (_MSC_VER >= 1700) /* VS2012 */
#	include <thread>
#	include <chrono>
#	include <atomic>

// Use new portable C++11 functionality.
#	define USE_STD_THREAD
#	define USE_STD_MUTEX
#	define USE_STD_ATOMIC
#	ifdef USE_STD_MUTEX
#		define USE_STD_CONDITION_VARIABLE
#	endif

// Portable C++11 thread sleep call.
#	define sleep(ms) std::this_thread::sleep_for(std::chrono::milliseconds(ms))

// Retain support for older Windows compilers (for now).
// Other platforms rely on C++11 support.
#else
#	define sleep(ms) Sleep(ms)
#endif

#ifndef WIN32
#	define SetConsoleTitle(title) /* unsupported & unnecessary */
#endif

#include "types.h"
#include "globals.h"
#include "Network.h"
#include "TimeThread.h"
