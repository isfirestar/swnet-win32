#include "network.h"
#include "iocp.h"


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define IOCP_INVALID_SIZE_TRANSFER			(0xFFFFFFFF)
#define IOCP_INVALID_COMPLETION_KEY			((ULONG_PTR)(~0))
#define IOCP_INVALID_OVERLAPPED_PTR			((OVERLAPPED *)0)

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
typedef struct _IO_THREAD_MGR {
	HANDLE	th_;
	int tid_;
}IO_THREAD_MGR, *PIO_THREAD_MGR;

typedef struct _IOCP_GENERAL_MANAGER {
	PIO_THREAD_MGR	th_mgr_;
	int th_cnt_;
	HANDLE iocp_port_;
}IOCP_GENERAL_MANAGER, *PIOCP_GENERAL_MANAGER;

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static PIOCP_GENERAL_MANAGER __iocp = NULL;

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static int iocp_complete_routine()
{
	return 0;
}

static uint32_t __stdcall iocp_thread_routine( void * p )
{
	uint32_t bytes_transfered;
	ULONG_PTR completion_key;
	LPOVERLAPPED ovlp = NULL;
	BOOL successful;

	while ( TRUE ) {
		ovlp = NULL;
		successful = GetQueuedCompletionStatus( __iocp->iocp_port_, &bytes_transfered, &completion_key, &ovlp, INFINITE );

		if ( !successful ) {
			if ( ovlp ) {
				/*
				If *lpOverlapped is not NULL and the function dequeues a completion packet for a failed I/O operation from the completion port, 
				the function stores information about the failed operation in the variables pointed to by lpNumberOfBytes, lpCompletionKey, and lpOverlapped. 
				*/
				so_dispatch_io_event( ovlp, bytes_transfered );		
			} else {
				/*
				If *lpOverlapped is NULL, the function did not dequeue a completion packet from the completion port
				In this case, 
				the function does not store information in the variables pointed to by the lpNumberOfBytes and lpCompletionKey parameters, and their values are indeterminate.
				*/
				continue; // 本次IO失败， 不代表线程应该错误, 不需要退出线程
			}
		} else {
			// 线程主动退出
			if ( IOCP_INVALID_SIZE_TRANSFER == bytes_transfered ) {
				break;
			}

			// 投递处理IO事件
			so_dispatch_io_event( ovlp, bytes_transfered );		
		}
	}

	os_dbg_info( "IO thread exit." );
	return 0L;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int iocp_init( int Cnt_IoThread )
{
	int i;
	uint32_t cnt_processtor;
	int cnt = Cnt_IoThread;
	static SYSTEM_INFO SysInfo;

	// 使用缺省的线程个数启动 IOCP 线程管理关联
	if ( 0 == cnt ) {
		GetSystemInfo( &SysInfo );
		cnt_processtor = SysInfo.dwNumberOfProcessors;
		if ( 0 == cnt_processtor ) {
			return -1;
		}
		cnt = cnt_processtor * 2;
	}

	// 至少保证 4 个线程
	if ( cnt < 4 ) cnt = 4;

	// 分配iocp对象
	__iocp = ( IOCP_GENERAL_MANAGER * ) malloc( sizeof( IOCP_GENERAL_MANAGER ) );
	if ( !__iocp ) {
		os_dbg_error("fail to allocate memory for __iocp" );
		return -1;
	}
	memset( __iocp, 0, sizeof( IOCP_GENERAL_MANAGER ) );

	// 初始化线程管理数组
	__iocp->th_mgr_ = ( PIO_THREAD_MGR ) malloc( sizeof( IO_THREAD_MGR ) * cnt );
	if ( !__iocp->th_mgr_ ) {
		os_dbg_error("fail to allocate memory for __iocp->th_mgr_" );
		free( __iocp );
		return -1;
	}

	// 创建完成端口句柄
	__iocp->iocp_port_ = CreateIoCompletionPort( INVALID_HANDLE_VALUE, NULL, ( ULONG_PTR ) iocp_complete_routine, cnt );
	if ( !__iocp->iocp_port_ ) {
		os_dbg_error("syscall CreateIoCompletionPort failed,error code=%u", GetLastError() );
		free( __iocp->th_mgr_ );
		free( __iocp );
		return -1;
	}

	// 创建IO线程
	for ( i = 0; i < cnt; i++ ) {
		__iocp->th_mgr_[i].th_ = CreateThread( NULL, 0, iocp_thread_routine, NULL, 0, &__iocp->th_mgr_[i].tid_ );
		if ( !__iocp->th_mgr_[i].th_ ) {
			os_dbg_error("syscall CreateThread failed,error code=%u", GetLastError() );
			goto exception;
		}
		__iocp->th_cnt_++;
	}

	return 0;

exception:

	// 处理异常回滚
	for ( i = 0; i < cnt; i++ ) {
		if ( !__iocp->th_mgr_[i].th_ ) {
			break;
		}
		PostQueuedCompletionStatus( __iocp->iocp_port_, IOCP_INVALID_SIZE_TRANSFER, IOCP_INVALID_COMPLETION_KEY, IOCP_INVALID_OVERLAPPED_PTR );
		WaitForSingleObject( __iocp->th_mgr_[i].th_, INFINITE );
		CloseHandle( __iocp->th_mgr_[i].th_ );
	}
	CloseHandle( __iocp->iocp_port_ );
	free( __iocp->th_mgr_ );
	free( __iocp );
	return -1;
}

int iocp_bind( const UINT_PTR file_handle )
{
	HANDLE bind_iocp;

	if ( !file_handle || !__iocp || ( ( UINT_PTR ) -1 ) == file_handle ) return -1;

	bind_iocp = CreateIoCompletionPort( ( void * ) file_handle, __iocp->iocp_port_, ( ULONG_PTR ) NULL, 0 );
	if ( ( bind_iocp ) && ( bind_iocp == __iocp->iocp_port_ ) ) {
		return 0;
	}

	os_dbg_error("syscall failed, error code=%u", GetLastError() );
	return -1;
}

void iocp_uninit()
{
	int i;

	if ( !__iocp ) return;

	// 没有任何线程在IOCP管理容器中
	if ( !__iocp->th_mgr_ ) {
		if ( __iocp->iocp_port_ ) CloseHandle( __iocp->iocp_port_ );
		free( __iocp );
		return;
	}

	// 向各个IO线程抛送结束信息， 并等待线程结束
	for ( i = 0; i < __iocp->th_cnt_; i++ ) {
		PostQueuedCompletionStatus( __iocp->iocp_port_, IOCP_INVALID_SIZE_TRANSFER, IOCP_INVALID_COMPLETION_KEY, IOCP_INVALID_OVERLAPPED_PTR );
	}
	for ( i = 0; i < __iocp->th_cnt_; i++ ) {
		if ( __iocp->th_mgr_[i].th_ ) {
			WaitForSingleObject( __iocp->th_mgr_[i].th_, INFINITE );
			CloseHandle( __iocp->th_mgr_[i].th_ );
		}
	}

	CloseHandle( __iocp->iocp_port_ );
	free( __iocp->th_mgr_ );
	free( __iocp );
}

int iocp_thcnts()
{
	return ( ( NULL == __iocp ) ? ( 0 ) : ( __iocp->th_cnt_ ) );
}