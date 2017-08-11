#include "network.h"
#include "ncb.h"
#include "packet.h"

/*++
	For a connectionless socket (for example, type SOCK_DGRAM),
	the operation performed by WSAConnect is merely to establish a default destination address so that the socket can be used on subsequent connection-oriented send and receive operations
	(send, WSASend, recv, and WSARecv). Any datagrams received from an address other than the destination address specified will be discarded. If the entire name structure is all zeros
	(not just the address parameter of the name structure), then the socket will be disconnected. Then, the default remote address will be indeterminate, so send, WSASend, recv,
	and WSARecv calls will return the error code WSAENOTCONN. However, sendto, WSASendTo, recvfrom, and WSARecvFrom can still be used.
	The default destination can be changed by simply calling WSAConnect again, even if the socket is already connected.
	Any datagrams queued for receipt are discarded if name is different from the previous WSAConnect.
	--*/

//---------------------------------------------------------------------------------------------------------------------------------------------------------
// 其他MXX相关 部分																				
//---------------------------------------------------------------------------------------------------------------------------------------------------------
int __stdcall nis_setctx( HLNK lnk, void * ncb_ctx, int ncb_ctx_size )
{
	ncb_t *ncb;
	int retval;
	char *ctx;

	ncb = objrefr( lnk );
	if ( !ncb ) {
		os_dbg_error( "reference NCB object failed,link=0x%08X", lnk );
		return -1;
	}

	do {
		retval = 0;

		// 指定空指针可以清空现有的上下文
		if ( !ncb_ctx || 0 == ncb_ctx_size ) {
			if ( ncb->ncb_ctx_ ) free( ncb->ncb_ctx_ );
			ncb->ncb_ctx_size_ = 0;
			break;
		}

		// 如果要求设置的上下文和当前上下文大小不一致， 则覆盖当前上下文
		if ( ncb_ctx_size != ncb->ncb_ctx_size_ ) {
			if ( ncb->ncb_ctx_ ) free( ncb->ncb_ctx_ );
			ncb->ncb_ctx_size_ = ncb_ctx_size;
			ctx = ( char * ) malloc( ncb_ctx_size );
			if ( !ctx ) {
				ncb_report_debug_information(ncb,  "fail to allocate memory for ncb context, size=%u", ncb_ctx_size );
				retval = -1;
				break;
			}
			ncb->ncb_ctx_ = ctx;
		}
		memcpy( ncb->ncb_ctx_, ncb_ctx, ncb->ncb_ctx_size_ );
	} while ( FALSE );

	objdefr( ncb->link );
	return retval;
}

int __stdcall nis_getctx( HLNK lnk, void * user_context, int *user_context_size )
{
	ncb_t * ncb;

	if ( !user_context ) return -1;

	ncb = objrefr( lnk );
	if ( !ncb ) {
		os_dbg_error( "reference NCB object failed,link=0x%08X", lnk );
		return -1;
	}

	if ( ncb->ncb_ctx_ && 0 != ncb->ncb_ctx_size_ ) memcpy( user_context, ncb->ncb_ctx_, ncb->ncb_ctx_size_ );
	if ( user_context_size ) *user_context_size = ncb->ncb_ctx_size_;
	objdefr( ncb->link );
	return 0;
}

void *__stdcall nis_refctx( HLNK lnk, int *user_context_size )
{
	ncb_t * ncb;
	void *ctxptr = NULL;

	ncb = objrefr( lnk );
	if ( ncb ) {
		if ( ncb->ncb_ctx_ && 0 != ncb->ncb_ctx_size_ ) {
			ctxptr = ncb->ncb_ctx_;
		}
		if ( user_context_size ) {
			*user_context_size = ncb->ncb_ctx_size_;
		}
		objdefr( ncb->link );
	}

	return ctxptr;
}

int __stdcall nis_ctxsize( HLNK lnk )
{
	ncb_t * ncb;
	long size;

	ncb = objrefr( lnk );
	if ( !ncb ) {
		os_dbg_error( "reference NCB object failed,link=0x%08X", lnk );
		return -1;
	}
	size = ncb->ncb_ctx_size_;
	objdefr( ncb->link );
	return size;
}

int __stdcall nis_getver( swnet_version_t  *version )
{
	if ( !version ) return -1;

	version->procedure_ = 0;
	version->main_ = 1;
	version->sub_ = 1;
	version->leaf_ = 13;
	return 0;
}

int __stdcall nis_gethost( const char *name, uint32_t *ipv4 ) {

	struct hostent *remote;
    struct in_addr addr;

	if ( !name || !ipv4 ) return -1;

	so_init( kProto_Unknown, -1 );

	*ipv4 = 0;

	 if (isalpha(name[0])) {        /* host address is a name */
        remote = gethostbyname(name);
	 } else {
		 addr.s_addr = inet_addr( name );
		 if ( INADDR_NONE == addr.s_addr ) {
			 return -1;
		 } else {
			 remote = gethostbyaddr( ( char * ) &addr, 4, AF_INET );
		 }
	 }

    if (!remote) {
		return -1;
    } 

	/* 目前仅支持 IPv4 */
	if ( AF_INET != remote->h_addrtype ) {
		return -1;
	}
	
	if ( remote->h_length < sizeof( uint32_t ) ) {
        return -1;
    }
    
    addr.s_addr = *((uint32_t *) remote->h_addr_list[0]);
    *ipv4 = ntohl(addr.s_addr);
	return 0;
}