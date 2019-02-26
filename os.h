#if !defined SWNET_OS_HEAD_20170112
#define SWNET_OS_HEAD_20170112

#if !defined WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <MSWSock.h>
#pragma comment(lib, "ws2_32.lib")
#include <WS2tcpip.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#if !defined NTSTATUS_TYPE
#define NTSTATUS_TYPE
typedef long NTSTATUS;
#endif

#if !defined NT_SUCCESS
#define NT_SUCCESS(status)		(status >= 0)
#endif

#if !defined NULL
#define NULL ((void *)0)
#endif

#if !defined MAXPATH
#define MAXPATH	(260)
#endif

#if !defined PAGE_SIZE
#define PAGE_SIZE		(4096)
#endif

#if !defined containing_record
#define containing_record(__address, __type, __field) ((__type *)( (char *)(__address) -  (char *)(&((__type *)0)->__field)))
#endif

#if !defined cchof
#define cchof(__array)   (uint32_t)(sizeof(__array) / sizeof(__array[0]))
#endif

#if !defined offsetof
#define offsetof(__type, __field)      (( unsigned int )(&((__type*)0)->__field))
#endif

#if !defined msizeof
#define msizeof(__type, __field)      (sizeof(((__type*)0)->__field))
#endif

#if !defined POSIX__EOL
#if _WIN32
#define POSIX__EOL              "\r\n"
#define POSIX__DIR_SYMBOL       '\\'
#else
#define POSIX__EOL              "\n"
#define POSIX__DIR_SYMBOL       '/'
#endif
#endif

#if !defined MAXDWORD
#define MAXDWORD		(0xFFFFFFFF)
#endif

#if !defined INFINITE
#define INFINITE		MAXDWORD
#endif

extern
void *os_lock_virtual_pages( void *MemoryBlock, uint32_t Size );
extern
void *os_allocate_block( HANDLE handleProcess, uint32_t blockSizeCb, uint32_t Protect );
extern 
int os_unlock_and_free_virtual_pages( void *MemoryBlock, uint32_t Size );
extern
void os_free_memory_block( void *MemoryBlock );

#define os_is_page_aligned(size)							(0 == (size % PAGE_SIZE))

#endif