#include "network.h"
#include "io.h"
#include "mxx.h"
#include "posix_ifos.h"
#include "ncb.h"
#include "posix_atomic.h"

#define IOCP_INVALID_SIZE_TRANSFER			(0xFFFFFFFF)
#define IOCP_INVALID_COMPLETION_KEY			((ULONG_PTR)(~0))
#define IOCP_INVALID_OVERLAPPED_PTR			((OVERLAPPED *)0)

struct epoll_object {
	HANDLE epfd;
	boolean_t actived;
	HANDLE thread;
	DWORD tid;
	int load; /* load of current thread */
};

struct epoll_object_manager {
	struct epoll_object *epos;
	int divisions;		/* count of epoll thread */
};

static struct epoll_object_manager epmgr;

static int iocp_complete_routine()
{
	return 0;
}

static DWORD WINAPI __iorun(LPVOID p)
{
	uint32_t bytes_transfered;
	ULONG_PTR completion_key;
	LPOVERLAPPED ovlp = NULL;
	BOOL successful;
	struct epoll_object *epos;

	epos = (struct epoll_object *)p;

	nis_call_ecr("[nshost.io.epoll]: epfd:%d LWP:%u startup.", epos->epfd, posix__gettid());

	while ( TRUE ) {
		ovlp = NULL;
		successful = GetQueuedCompletionStatus(epos->epfd, &bytes_transfered, &completion_key, &ovlp, INFINITE);

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
				one time IO error does not represent an thread error, the IO thread not need to quit
				*/
				continue;
			}
		} else {
			/* ask IO thread exit initiative */
			if ( IOCP_INVALID_SIZE_TRANSFER == bytes_transfered ) {
				break;
			}
			so_dispatch_io_event( ovlp, bytes_transfered );
		}
	}

	nis_call_ecr("[nshost.io.epoll]:epfd:%d LWP:%u terminated.", epos->epfd, posix__gettid());
	return 0L;
}

static void *__epoll_proc(void *p) 
{
	__iorun(p);
	return NULL;
}

int __ioinit() 
{
	int i;

	epmgr.divisions = posix__getnprocs();
	if (NULL == (epmgr.epos = (struct epoll_object *)malloc(sizeof(struct epoll_object) * epmgr.divisions))) {
		return -1;
	}

	for (i = 0; i < epmgr.divisions; i++) {
		epmgr.epos[i].load = 0;
		epmgr.epos[i].epfd = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, (ULONG_PTR)&iocp_complete_routine, 1);
		if (epmgr.epos[i].epfd < 0) {
			nis_call_ecr("[nshost.io.epoll]:file descriptor creat failed. error:%u", GetLastError());
			epmgr.epos[i].actived = NO;
			continue;
		}

		/* active field as a judge of operational effectiveness, as well as a control symbol of operation  */
		epmgr.epos[i].actived = YES;
		epmgr.epos[i].thread = CreateThread(NULL, 0, &__iorun, &epmgr.epos[i], 0, &epmgr.epos[i].tid);
		if (!epmgr.epos[i].thread) {
			nis_call_ecr("[nshost.io.epoll]:io thread create failed. error:%u", GetLastError());
			epmgr.epos[i].actived = NO;
		}
	}

	return 0;
}

posix__atomic_initial_declare_variable(__inited__);

int ioinit() 
{
	if (posix__atomic_initial_try(&__inited__)) {
		if (__ioinit() < 0) {
			posix__atomic_initial_exception(&__inited__);
		} else {
			posix__atomic_initial_complete(&__inited__);
		}
	}

	return __inited__;
}

int ioatth(void *ncbptr)
{
	ncb_t *ncb;
	struct epoll_object *epos;
	HANDLE bind_iocp;

	if (!posix__atomic_initial_passed(__inited__)) {
		return -1;
	}

	ncb = (ncb_t *)ncbptr;
	if (!ncb) {
		return -EINVAL;
	}

	epos = &epmgr.epos[ncb->hld % epmgr.divisions];
	bind_iocp = CreateIoCompletionPort((HANDLE)ncb->sockfd, epos->epfd, (ULONG_PTR)NULL, 0);
	if ((bind_iocp) && (bind_iocp == epos->epfd)) {
		nis_call_ecr("[nshost.io.ioatth] success associate sockfd:%d with epfd:%d, link:%I64d", ncb->sockfd, epos->epfd, ncb->hld);
		return 0;
	}

	nis_call_ecr("[nshost.io.ioattach] link:%I64u syscall CreateIoCompletionPort failed, error code=%u", ncb->hld, GetLastError());
	return -1;
}

void ioclose(void *ncbptr)
{
	ncb_t *ncb;

	ncb = (ncb_t *)ncbptr;
	if (ncb) {
		if (INVALID_SOCKET != ncb->sockfd) {
			shutdown(ncb->sockfd, SD_BOTH);
			closesocket(ncb->sockfd);
			ncb->sockfd = INVALID_SOCKET;
		}
	}
}

void iouninit() 
{
	int i;
	struct epoll_object *epos;

	if (!posix__atomic_initial_regress(__inited__)) {
		return;
	}

	if (!epmgr.epos) {
		return;
	}

	for (i = 0; i < epmgr.divisions; i++){
		epos = &epmgr.epos[i];
		if (YES == epmgr.epos[i].actived){
			posix__atomic_xchange(epos->actived, NO);
			PostQueuedCompletionStatus(epos->epfd, IOCP_INVALID_SIZE_TRANSFER, IOCP_INVALID_COMPLETION_KEY, IOCP_INVALID_OVERLAPPED_PTR);
			WaitForSingleObject(epos->thread, INFINITE);
			CloseHandle(epos->thread);
		}

		if (epmgr.epos[i].epfd > 0){
			CloseHandle(epmgr.epos[i].epfd);
			epmgr.epos[i].epfd = INVALID_HANDLE_VALUE;
		}
	}

	free(epmgr.epos);
	epmgr.epos = NULL;
}

void *io_get_pipefd(void *ncbptr)
{
	return epmgr.epos[((ncb_t *)ncbptr)->hld % epmgr.divisions].epfd;
}
