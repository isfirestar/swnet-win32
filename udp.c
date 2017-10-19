#include "network.h"
#include "ncb.h"
#include "packet.h"
#include "iocp.h"

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
typedef struct _udp_cinit {
	uint32_t ipv4_;
	uint16_t port_;
	udp_io_callback_t f_user_callback_;
	int ncb_flag_;
}udp_cinit_t;

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void udp_shutdown( packet_t * packet );
static
int udp_set_boardcast( ncb_t *ncb, int enable );
static
int udp_get_boardcast( ncb_t *ncb, int *enabled );

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static
ncb_t *udprefr( objhld_t handle ) {
	ncb_t *ncb;

	if ( handle < 0 ) {
		return NULL;
	}

	ncb = objrefr( handle );
	if ( ncb ) {
		if ( ncb->proto_type_ == kProto_UDP ) {
			return ncb;
		}
		objdefr( handle );
	}
	return NULL;
}

static void udp_dispatch_io_recv( packet_t *packet )
{
	nis_event_t c_event;
	udp_data_t c_data;
	ncb_t *ncb;

	if ( !packet ) return;

	ncb = udprefr( packet->link );
	if ( !ncb ) {
		os_dbg_warn("fail to reference ncb object:0x%08X", packet->link);
		return;
	}

	// 填充接收数据事件
	c_event.Ln.Udp.Link = (HUDPLINK)packet->link;
	c_event.Event = EVT_RECEIVEDATA;

	// 通知接收事件
	c_data.e.Packet.Size = packet->size_for_translation_;
	c_data.e.Packet.Data = ( const char * ) packet->irp_;
	c_data.e.Packet.RemotePort = ntohs( packet->r_addr_.sin_port );
	if ( !inet_ntop( AF_INET, &packet->r_addr_.sin_addr, c_data.e.Packet.RemoteAddress, _countof( c_data.e.Packet.RemoteAddress ) ) ) {
		RtlZeroMemory( c_data.e.Packet.RemoteAddress, _countof( c_data.e.Packet.RemoteAddress ) );
	}
	ncb_callback( ncb, &c_event, ( void * ) &c_data );

	// 这个时候已经是完成了处理, 继续将一个异步的 IRP 投递下去
	// 本次操作， 包括缓冲区在内的一切东西都可以忽略了
	packet->size_for_translation_ = 0;
	asio_udp_recv( packet );

	objdefr( ncb->link );
}

static void udp_dispatch_io_send( packet_t * packet )
{
	if ( packet ) {
		free_packet( packet );
		packet = NULL;
	}
}

static void udp_dispatch_io_exception( packet_t * packet, NTSTATUS status )
{
	nis_event_t c_event;
	udp_data_t c_data;
	ncb_t *ncb;

	if ( !packet ) return;

	ncb = udprefr(packet->link);
	if ( !ncb ) {
		os_dbg_warn("fail to reference ncb object:0x%08X", packet->link);
		return;
	}

	os_dbg_warn("IO exception catched on lnk [0x%08X], NTSTATUS=0x%08X", packet->link, status);

	// 回调通知异常事件
	c_event.Ln.Udp.Link = (HUDPLINK)packet->link;
	c_event.Event = EVT_EXCEPTION;
	if ( kSend == packet->type_ ) {
		c_data.e.Exception.SubEvent = EVT_SENDDATA;
	} else if ( kRecv == packet->type_ ) {
		c_data.e.Exception.SubEvent = EVT_RECEIVEDATA;
	} else {
		;
	}
	c_data.e.Exception.ErrorCode = status;
	ncb_callback( ncb, &c_event, ( void * ) &c_data );

	// 回收包内存
	udp_shutdown( packet );
}

static int udp_update_opts(ncb_t *ncb) {
	static const int RECV_BUFFER_SIZE = 0xFFFF;
    static const int SEND_BUFFER_SIZE = 0xFFFF;

    if (!ncb) {
		return -EINVAL;
    }

    ncb_set_window_size(ncb, SO_RCVBUF, RECV_BUFFER_SIZE);
    ncb_set_window_size(ncb, SO_SNDBUF, SEND_BUFFER_SIZE);
    ncb_set_linger(ncb, 0, 1);
	return 0;
}

static int udp_entry( objhld_t h, void * user_buffer, const void * ncb_ctx )
{
	udp_cinit_t *ctx = ( udp_cinit_t * ) ncb_ctx;
	nis_event_t c_event;
	udp_data_t c_data;
	int behavior;
	ncb_t *ncb = ( ncb_t * ) user_buffer;
	uint32_t bytes_returned;
	struct sockaddr_in conn_addr;

	if ( !ctx || !ncb ) return -1;

	ncb_init( ncb, kProto_UDP );
	ncb->link = h;
	ncb->sockfd = so_allocate_asio_socket(SOCK_DGRAM, IPPROTO_UDP);
	if (ncb->sockfd < 0) {
		return -1;
	}

	do {
		if (so_bind(&ncb->sockfd, ctx->ipv4_, ctx->port_) < 0) {
			break;
		}

		// 直接填写地址结构
		ncb->l_addr_.sin_family = AF_INET;
		ncb->l_addr_.sin_addr.S_un.S_addr = ctx->ipv4_;
		ncb->l_addr_.sin_port = ctx->port_;

		// 如果采用随机端口绑定， 则应该获取真实的绑定端口
		if ( 0 == ctx->port_ ) {
			int len = sizeof( conn_addr );
			if (getsockname(ncb->sockfd, (SOCKADDR*)&conn_addr, &len) >= 0) {
				ncb->l_addr_.sin_port = ntohs( conn_addr.sin_port );
			}
		}

		// 设置一些套接字参数
		udp_update_opts( ncb );

		// UDP标记
		if ( ctx->ncb_flag_ &  UDP_FLAG_BROADCAST ) {
			if ( udp_set_boardcast( ncb, 1 ) < 0 ) {
				break;
			}
			ncb->flag_ |= UDP_FLAG_BROADCAST;
		} else {
            if (ctx->ncb_flag_ & UDP_FLAG_MULTICAST) {
                ncb->flag_ |= UDP_FLAG_MULTICAST;
            }
        }

		// 关闭因对端被强制无效化，导致的本地 io 错误
		// 这项属性打开情形下会导致 WSAECONNRESET 错误返回
		behavior = -1;
		if (WSAIoctl(ncb->sockfd, SIO_UDP_CONNRESET, &behavior, sizeof(behavior), NULL, 0, &bytes_returned, NULL, NULL) < 0) {
			ncb_report_debug_information(ncb, "syscall WSAIoctl failed to control SIO_UDP_CONNRESET,error cdoe=%u ", WSAGetLastError() );
			break;
		}

		// 将对象绑定到异步IO的完成端口
		if (iocp_bind(ncb->sockfd) < 0) break;

		// 回调用户地址， 通知调用线程， UDP 子对象已经创建完成
		ncb_set_callback( ncb, ctx->f_user_callback_ );
		c_event.Event = EVT_CREATED;
		c_event.Ln.Udp.Link = (HUDPLINK)ncb->link;
		c_data.e.LinkOption.OptionLink = (HUDPLINK)ncb->link;
		ncb_callback(ncb, &c_event, &c_data);

		return 0;

	} while ( FALSE );

	so_close(&ncb->sockfd);
	return -1;
}

static void udp_unload( objhld_t h, void * user_buffer )
{
	nis_event_t c_event;
	udp_data_t c_data;
	ncb_t * ncb = ( ncb_t * ) user_buffer;

	if ( !user_buffer ) return;

	// 关闭前事件
	c_event.Ln.Udp.Link = ( HUDPLINK ) h;
	c_event.Event = EVT_PRE_CLOSE;
	c_data.e.LinkOption.OptionLink = ( HUDPLINK ) h;
	ncb_callback( ncb, &c_event, &h );

	// 关闭内部套接字
	so_close(&ncb->sockfd);

	// 关闭后事件
	c_event.Event = EVT_CLOSED;
	ncb_callback( ncb, &c_event, &h );

	// 释放用户上下文数据指针
	if ( ncb->ncb_ctx_ && 0 != ncb->ncb_ctx_size_ ) free( ncb->ncb_ctx_ );
}

static objhld_t udp_allocate_object(const udp_cinit_t *ctx) {
	ncb_t *ncb;
	objhld_t h;
	int retval;

	h = objallo( (int)sizeof( ncb_t ), &objentry, &udp_unload, NULL, 0 );
	if ( h < 0 ) {
		return -1;
	}
	ncb = objrefr( h );
	retval = udp_entry( h, ncb, ctx );
	objdefr( h );
	ncb = NULL;

	if ( retval < 0 ) {
		objclos( h );
		return -1;
	}

	return h;
}

static packet_t **udp_allocate_recv_array( objhld_t h, int cnt )
{
	packet_t **pkt_array;
	int retval = -1;
	int i;
	int a_size = sizeof( packet_t * ) * cnt;

	// 用多个包来记录申请内存过程可能存在的异常
	pkt_array = ( packet_t ** ) malloc( a_size );
	if ( !pkt_array ) return NULL;
	memset( pkt_array, 0, a_size );

	// 对于每个 UDP 对象， 都会有一块 8KB 的数据在异步等待中
	// 为了避免这块内存的缺页影响性能， 这里将该片内存提升为非分页池
	for ( i = 0; i < cnt; i++ ) {
		retval = allocate_packet( h, kProto_UDP, kRecv, PAGE_SIZE, kNonPagedPool, &pkt_array[i] );
		if ( retval < 0 ) {
			break;
		}
	}

	// 申请过程发生错误， 应该会滚并释放所有的包内存
	if ( retval < 0 ) {
		for ( i = 0; i < cnt; i++ ) {
			free_packet( pkt_array[i] );
		}
		free( pkt_array );
		return NULL;
	}

	return pkt_array;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void udp_shutdown( packet_t * packet )
{
	if ( packet ) {
		objclos(packet->link);
		free_packet( packet );
	}
}

void udp_dispatch_io_event( packet_t *packet, NTSTATUS status )
{
	// 交换字节数为0的情况， 只能是TCP ACCEPT完成， 其他情况认为是致命错误， 将关闭链接
	if ( 0 == packet->size_for_translation_ ) {
		udp_shutdown( packet );
		return;
	}

	// 进行IO结果判定， 如果IO失败， 应该通过回调的方式通告调用线程
	if ( !NT_SUCCESS( status ) ) {
		udp_dispatch_io_exception( packet, status );
		return;
	}

	// 收发事件处理分派
	switch ( packet->type_ ) {
		case kRecv:
			udp_dispatch_io_recv( packet );
			break;
		case kSend:
			udp_dispatch_io_send( packet );
			break;
		default:
			break;
	}
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int __stdcall udp_init()
{
	return so_init( kProto_UDP, 0 );
}

HUDPLINK __stdcall udp_create( udp_io_callback_t user_callback, const char* l_ipstr, uint16_t l_port, int flag )
{
	objhld_t h;
	udp_cinit_t ctx;
	packet_t **pkt_array;
	int cnts;
	int i;

	if ( !user_callback ) return INVALID_HUDPLINK;

	// 预先驱动可以投递给系统的 IRP 的恰当数量
	cnts = so_asio_count();
	if ( 0 == cnts ) return INVALID_HUDPLINK;

	if ( !l_ipstr ) {
		ctx.ipv4_ = INADDR_ANY;
	} else {
		IN_ADDR l_in_addr;
		/*
		The InetPton function returns a value of 0 if the pAddrBuf parameter points to a string that is not a valid IPv4 dotted-decimal string or a valid IPv6 address string.
		Otherwise, a value of -1 is returned, and a specific error code can be retrieved by calling the WSAGetLastError for extended error information.
		*/
		if ( inet_pton( AF_INET, l_ipstr, &l_in_addr ) <= 0 ) {
			return INVALID_HUDPLINK;
		}
		ctx.ipv4_ = l_in_addr.S_un.S_addr;
	}
	ctx.port_ = htons( l_port );
	ctx.f_user_callback_ = user_callback;
	ctx.ncb_flag_ = flag;

	h = udp_allocate_object( &ctx );
	if ( h < 0 ) {
		return -1;
	}

	pkt_array = udp_allocate_recv_array( h, cnts );
	if ( !pkt_array ) {
		objclos( h );
		return INVALID_HUDPLINK;
	}

	// 投递接收请求
	for ( i = 0; i < cnts; i++ ) {
		asio_udp_recv( pkt_array[i] );
	}
	free( pkt_array );
	return ( HUDPLINK ) h;
}

static int udp_maker( void *data, int cb, void *context ) {
	if ( data && cb > 0 && context ) {
		memcpy( data, context, cb );
		return 0;
	}
	return -1;
}

int __stdcall udp_write( HUDPLINK lnk, int cb, int( __stdcall *data_filler )( void *, int, void * ), void *par, const char* r_ipstr, uint16_t r_port )
{
	packet_t * packet;
	char *buffer;

	if ( cb > MTU || cb <= 0 || !data_filler || INVALID_HUDPLINK == lnk || !r_ipstr || 0 == r_port ) {
		return -1;
	}

	buffer = ( char * ) malloc( cb );
	if ( !buffer ) {
		return -1;
	}

	if ( data_filler ) {
		if ( data_filler( buffer, cb, par ) < 0 ) {
			free( buffer );
			return -1;
		}
	} else {
		if ( udp_maker( buffer, cb, par ) < 0 ) {
			free( buffer );
			return -1;
		}
	}

	if ( allocate_packet( ( objhld_t ) lnk, kProto_UDP, kSend, 0, kNoAccess, &packet ) >= 0 ) {
		packet->irp_ = packet->ori_buffer_ = buffer;
		packet->size_for_req_ = cb;
		return syio_udp_send( packet, r_ipstr, r_port );
	}

	free( buffer );
	return -1;
}

void __stdcall udp_destroy( HUDPLINK lnk )
{
	if ( INVALID_HUDPLINK != lnk ) {
		objclos( ( objhld_t ) lnk );
	}
}

void __stdcall udp_uninit()
{
	so_uninit( kProto_UDP );
}

int __stdcall udp_getaddr( HUDPLINK lnk, uint32_t* ipv4, uint16_t *port_output )
{
	ncb_t * ncb;

	if ( INVALID_HUDPLINK == lnk || !ipv4 || !port_output ) {
		return -1;
	}

	ncb = udprefr( lnk );
	if ( ncb ) {
		if ( kProto_UDP == ncb->proto_type_ ) {
			*ipv4 = htonl( ncb->l_addr_.sin_addr.S_un.S_addr );
			*port_output = htons( ncb->l_addr_.sin_port );
		} else {
			*ipv4 = 0;
			*port_output = 0;
		}

		objdefr( ncb->link );
		return 0;
	}

	os_dbg_warn("fail to reference ncb object:0x%08X", lnk );
	return -1;
}

int __stdcall udp_setopt( HUDPLINK lnk, int level, int opt, const char *val, int len )
{
	ncb_t * ncb;
	int retval = -1;

	if ( ( INVALID_HUDPLINK == lnk ) || ( !val ) ) {
		return -1;
	}

	ncb = udprefr( lnk );
	if ( !ncb ) {
		os_dbg_warn("fail to reference ncb object:0x%08X", lnk );
		return -1;
	}

	if ( kProto_UDP == ncb->proto_type_ ) {
		retval = ((kProto_UDP == ncb->proto_type_) ? setsockopt(ncb->sockfd, level, opt, val, len) : (-1));
	}
	objdefr( ncb->link );
	return retval;
}

int __stdcall udp_getopt( HUDPLINK lnk, int level, int opt, char *val, int *len )
{
	ncb_t *ncb;
	int retval = -1;

	if ( ( INVALID_HUDPLINK == lnk ) || ( !val ) || ( !len ) ) {
		return -1;
	}

	ncb = udprefr( lnk );
	if ( !ncb ) {
		os_dbg_warn("fail to reference ncb object:0x%08X", lnk );
		return -1;
	}

	if ( kProto_UDP == ncb->proto_type_ ) {
		retval = ((kProto_UDP == ncb->proto_type_) ? getsockopt(ncb->sockfd, level, opt, val, len) : (-1));
	}
	objdefr( ncb->link );
	return retval;
}

int __stdcall udp_initialize_grp( HUDPLINK lnk, packet_grp_t *grp )
{
	ncb_t *ncb;
	int retval = -1;
	int i;

	if ( !grp ) return -1;
	if ( grp->Count <= 0 ) return -1;

	ncb = udprefr( lnk );
	if ( !ncb ) {
		os_dbg_warn("fail to reference ncb object:0x%08X", lnk );
		return -1;
	}

	do {
		if ( kProto_UDP != ncb->proto_type_ ) break;
		if ( !ncb->connected_ ) break;

		for ( i = 0; i < grp->Count; i++ ) {
			if ( ( 0 == grp->Items[i].Length ) || ( grp->Items[i].Length > MTU ) ) break;
			if ( NULL == ( grp->Items[i].Data = ( char * ) malloc( grp->Items[i].Length ) ) ) break;
		}

		retval = ( ( i == grp->Count ) ? 0 : -1 );
		if ( 0 == retval ) break;

		// 错误回滚
		for ( i = 0; i < grp->Count; i++ ) {
			if ( !grp->Items[i].Data ) {
				break;
			}
			free( grp->Items[i].Data );
			grp->Items[i].Data = NULL;
		}

	} while ( FALSE );

	objdefr( ncb->link );
	return retval;
}

void __stdcall udp_release_grp( packet_grp_t *grp )
{
	int i;

	if ( !grp ) return;
	if ( grp->Count <= 0 ) return;

	for ( i = 0; i < grp->Count; i++ ) {
		if ( !grp->Items[i].Data ) break;
		free( grp->Items[i].Data );
		grp->Items[i].Data = NULL;
	}
}

int __stdcall udp_raise_grp( HUDPLINK lnk, const char *r_ipstr, uint16_t r_port )
{
	ncb_t *ncb;
	int retval;
	struct sockaddr_in r_addr;

	ncb = udprefr( lnk );
	if ( !ncb ) {
		os_dbg_warn("fail to reference ncb object:0x%08X", lnk );
		return -1;
	}

	do {
		retval = -1;

		if ( kProto_UDP != ncb->proto_type_ ) {
			break;
		}

		if ( inet_pton( AF_INET, r_ipstr, &r_addr.sin_addr ) <= 0 ) {
			break;
		}
		r_addr.sin_family = AF_INET;
		r_addr.sin_port = htons( r_port );

		if ( ncb->connected_ ) {
			if ( ncb->connected_ == (int)r_addr.sin_addr.S_un.S_addr ) {
				retval = 0;
			}
			break;
		}

		retval = syio_v_connect( ncb, &r_addr );
		if ( retval >= 0 ) {
			ncb->connected_ = (int)r_addr.sin_addr.S_un.S_addr;
		}

	} while ( FALSE );

	objdefr( ncb->link );
	return retval;
}

void __stdcall udp_detach_grp( HUDPLINK lnk )
{
	ncb_t * ncb;

	ncb = udprefr( lnk );
	if ( !ncb ) {
		os_dbg_warn("fail to reference ncb object:0x%08X", lnk );
		return;
	}

	if ( kProto_UDP == ncb->proto_type_ ) {
		if ( 0 != ncb->connected_ ) {
			syio_v_disconnect( ncb );
		}
	}

	objdefr( ncb->link );
}

int __stdcall udp_write_grp( HUDPLINK lnk, packet_grp_t *grp )
{
	ncb_t *ncb;
	packet_t *packet;
	int i;
	int retval = -1;

	if ( !grp ) return -1;
	if ( grp->Count <= 0 ) return -1;

	ncb = udprefr( lnk );
	if ( !ncb ) {
		os_dbg_warn("fail to reference ncb object:0x%08X", lnk );
		return -1;
	}

	do {
		if ( kProto_UDP != ncb->proto_type_ ) break;
		if ( !ncb->connected_ ) break;

		// 创建分组包
		retval = allocate_packet( ( objhld_t ) lnk, kProto_UDP, kSend, 0, kNoAccess, &packet );
		if ( retval < 0 ) {
			break;
		}

		packet->grp_packets_cnt_ = grp->Count;
		packet->grp_packets_ = ( PTRANSMIT_PACKETS_ELEMENT ) malloc( sizeof( TRANSMIT_PACKETS_ELEMENT ) * grp->Count );
		if ( packet->grp_packets_ ) {
			for ( i = 0; i < packet->grp_packets_cnt_; i++ ) {
				packet->grp_packets_[i].pBuffer = grp->Items[i].Data;
				packet->grp_packets_[i].cLength = grp->Items[i].Length;
				packet->grp_packets_[i].dwElFlags = TP_ELEMENT_MEMORY | TP_ELEMENT_EOP;
			}
			retval = syio_grp_send( packet );
		}

		// 同步完成后自动释放包， 但包中的 grp 只能交由 xx_release_grp 进行释放
		free_packet( packet );
	} while ( FALSE );

	objdefr( ncb->link );
	return retval;
}

int __stdcall udp_joingrp(HUDPLINK lnk, const char *g_ipstr, uint16_t g_port) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;
    int retval;

    if (lnk < 0 || !g_ipstr || 0 == g_port) {
        return -EINVAL;
    }

    ncb = objrefr(hld);
    if (!ncb) return -1;

    do {
        retval = -1;

        if (!(ncb->flag_ & UDP_FLAG_MULTICAST)) {
            break;
        }

        /*设置回环许可*/
        int loop = 1;
		retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&loop, sizeof (loop));
        if (retval < 0) {
            break;
        }
        
        /*加入多播组*/
        if (!ncb->mreq){
            ncb->mreq = (struct ip_mreq *)malloc(sizeof(struct ip_mreq));
		}
		if ( !ncb->mreq ) {
			break;
		}
        ncb->mreq->imr_multiaddr.s_addr = inet_addr(g_ipstr); 
        ncb->mreq->imr_interface.s_addr = ncb->l_addr_.sin_addr.s_addr; 
		retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const void *)ncb->mreq, sizeof(struct ip_mreq));
        if (retval < 0){
            break;
        }
        
    } while (0);

    objdefr(hld);
    return retval;
}

int __stdcall udp_dropgrp(HUDPLINK lnk){
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;
    int retval;
    
    if (lnk < 0){
        return -EINVAL;
    }
    
    ncb = objrefr(hld);
    if (!ncb) return -1;
    
    do{
        retval = -1;
        
        if (!(ncb->flag_ & UDP_FLAG_MULTICAST) || !ncb->mreq) {
            break;
        }

		/*还原回环许可*/
        int loop = 0;
		retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&loop, sizeof (loop));
        if (retval < 0) {
            break;
        }
        
        /*离开多播组*/
		retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const void *)ncb->mreq, sizeof(struct ip_mreq));
        
    }while(0);
    
    objdefr(hld);
    return retval;
}

int udp_set_boardcast(ncb_t *ncb, int enable) {
    if (ncb) {
        return setsockopt(ncb->sockfd, SOL_SOCKET, SO_BROADCAST, (const void *) &enable, sizeof (enable));
    }
    return -EINVAL;
}

int udp_get_boardcast(ncb_t *ncb, int *enabled) {
    if (ncb && enabled) {
        socklen_t optlen = sizeof (int);
        return getsockopt(ncb->sockfd, SOL_SOCKET, SO_BROADCAST, (void * __restrict)enabled, &optlen);
    }
    return -EINVAL;
}