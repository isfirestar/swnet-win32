#include "network.h"
#include "ncb.h"
#include "packet.h"
#include "io.h"
#include "mxx.h"

#include <assert.h>
#include <mstcpip.h>

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define TCP_ACCEPT_EXTENSION_SIZE					( 1024 )
#define TCP_RECV_BUFFER_SIZE						( 17 * PAGE_SIZE )
#define TCP_LISTEN_BLOCK_COUNT						( 5 )
#define TCP_SYN_REQ_TIMES							( 150 )

#define TCP_BUFFER_SIZE								( 0x11000 )
#define TCP_MAXIMUM_PACKET_SIZE						( 50 << 20 )

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
typedef struct _TCP_INIT_CONTEXT {
	uint32_t ip_;
	uint16_t port_;
	tcp_io_callback_t callback_;
	int is_remote_;
}tcp_cinit_t;

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define TCP_MAXIMUM_SENDER_CACHED_CNT				( 5120 ) // 以每个包64KB计, 最多可以接受 327MB 的发送堆积
static long __tcp_global_sender_cached_cnt = 0; // TCP 全局缓存的发送包个数(未发出的链表长度)

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
typedef enum _TCPSTATE {
	TCPSTATE_CLOSED,
	TCPSTATE_LISTEN,
	TCPSTATE_SYN_SENT,
	TCPSTATE_SYN_RCVD,
	TCPSTATE_ESTABLISHED,
	TCPSTATE_FIN_WAIT_1,
	TCPSTATE_FIN_WAIT_2,
	TCPSTATE_CLOSE_WAIT,
	TCPSTATE_CLOSING,
	TCPSTATE_LAST_ACK,
	TCPSTATE_TIME_WAIT,
	TCPSTATE_MAX
} TCPSTATE;

typedef struct _TCP_INFO_v0 {
	TCPSTATE State;
	ULONG    Mss;
	ULONG64  ConnectionTimeMs;
	BOOLEAN  TimestampsEnabled;
	ULONG    RttUs;
	ULONG    MinRttUs;
	ULONG    BytesInFlight;
	ULONG    Cwnd;
	ULONG    SndWnd;
	ULONG    RcvWnd;
	ULONG    RcvBuf;
	ULONG64  BytesOut;
	ULONG64  BytesIn;
	ULONG    BytesReordered;
	ULONG    BytesRetrans;
	ULONG    FastRetrans;
	ULONG    DupAcksIn;
	ULONG    TimeoutEpisodes;
	UCHAR    SynRetrans;
} TCP_INFO_v0, *PTCP_INFO_v0;

void tcp_shutdown_by_packet( packet_t * packet );
static int tcp_save_info(ncb_t *ncb, TCP_INFO_v0 *ktcp);
static int tcp_setmss( ncb_t *ncb, int mss );
static int tcp_getmss( ncb_t *ncb );
static int tcp_set_nodelay( ncb_t *ncb, int set );
static int tcp_get_nodelay( ncb_t *ncb, int *set );

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static
int tcprefr(objhld_t hld, ncb_t **ncb) {
	if (hld < 0 || !ncb) {
		return -ENOENT;
	}

	*ncb = objrefr(hld);
	if (NULL != (*ncb)) {
		if ((*ncb)->proto_type_ == kProto_TCP) {
			return 0;
		}

		objdefr(hld);
		*ncb = NULL;
		return -EPROTOTYPE;
	}

	return -ENOENT;
}


static 
void tcp_try_write( ncb_t * ncb )
{
	packet_t *next_packet;

	if (!ncb) {
		return;
	}

	EnterCriticalSection( &ncb->tcp_lst_lock_ );

	if ( list_empty( &ncb->tcp_waitting_list_head_ ) ) {
		LeaveCriticalSection( &ncb->tcp_lst_lock_ );
		return;
	}

	next_packet = list_first_entry( &ncb->tcp_waitting_list_head_, packet_t, pkt_lst_entry_ );

	//
	// 规则：(这里使用反向判定)
	// 1. 当前没有任何未决请求在下级缓冲区, 无条件发送下一个缓冲包(这条规则可以妥善处理大包发出)
	// 2. 当前链的可用缓冲区大于下一个包的长度， 允许继续向操作系统投递包
	// 
	if ( ncb->tcp_pending_cnt_ > 0 ) {
		if ( next_packet->size_for_req_ > ncb->tcp_usable_sender_cache_ ) {
			LeaveCriticalSection( &ncb->tcp_lst_lock_ );
			return;
		}
	}

	list_del_init( &next_packet->pkt_lst_entry_ );

	/*	累加未决计数和未决长度	*/
	ncb->tcp_pending_cnt_++;
	ncb->tcp_usable_sender_cache_ -= next_packet->size_for_req_;

	/*
	发生非 PENDING 错误，不会出发 IOCP 例程
	此时认定为一种无法处理的错误， 直接执行断开链接
	同时, 不再需要继续关注后续处理， 不需要释放本包内存和拉伸发送缓冲区
	*/
	if ( asio_tcp_send( next_packet ) < 0 ) {
		tcp_shutdown_by_packet( next_packet );
	}

	LeaveCriticalSection( &ncb->tcp_lst_lock_ );
}

static
int tcp_lb_assemble( ncb_t * ncb, packet_t * packet )
{
	int current_usefule_size;
	int logic_revise_acquire_size;
	nis_event_t c_event;
	tcp_data_t c_data;
	int lb_acquire_size;
	int user_size;

	if ( !packet || !ncb ) return -1;
	if ( 0 == packet->size_for_translation_ ) return -1;
	if ( !ncb->tcp_tst_.parser_ ) return -1;

	// 当前可用长度定义为已分析长度加上本次交换长度
	current_usefule_size = packet->size_for_translation_;

	// 从大包缓冲区中，取得大包需要的长度
	if ( ncb->tcp_tst_.parser_( ncb->lb_data_, ncb->tcp_tst_.cb_, &user_size ) < 0 ) {
		return -1;
	}

	// 大包修正请求长度 = 用户数据长度 + 底层协议长度
	logic_revise_acquire_size = user_size + ncb->tcp_tst_.cb_;

	// 大包填满长度 = 需要的修正长度 - 已经拷贝的数据偏移
	lb_acquire_size = logic_revise_acquire_size - ncb->lb_cpy_offset_;

	// 当前可用数据长度， 不足以填充整个大包, 进行数据拷贝， 调整大包偏移， 继续接收数据
	if ( current_usefule_size < lb_acquire_size ) {
		assert(ncb->lb_cpy_offset_ + current_usefule_size <= ncb->lb_length_);
		memcpy( ncb->lb_data_ + ncb->lb_cpy_offset_, ( char * ) packet->ori_buffer_, current_usefule_size );
		ncb->lb_cpy_offset_ += current_usefule_size;
		return 0;
	}

	// 足以填充大包， 则将大包填充满并回调到用户例程
	assert(ncb->lb_cpy_offset_ + lb_acquire_size <= ncb->lb_length_);
	memcpy( ncb->lb_data_ + ncb->lb_cpy_offset_, ( char * ) packet->ori_buffer_, lb_acquire_size );
	c_event.Ln.Tcp.Link = ( HTCPLINK ) ncb->link;
	c_event.Event = EVT_RECEIVEDATA;
	c_data.e.Packet.Size = user_size;
	c_data.e.Packet.Data = ( const char * ) ( ( char * ) ncb->lb_data_ + ncb->tcp_tst_.cb_ );
	ncb_callback( ncb, &c_event, &c_data );

	// 一次大包的解析已经完成， 销毁ncb_t中的大包字段
	ncb_unmark_lb( ncb );

	// 如果不是刚好填满大包， 即:有效数据有冗余， 则应该将有效数据剩余长度告知调用线程， 以资进行下一轮的解包操作
	if ( current_usefule_size != lb_acquire_size ) {
		memmove( packet->ori_buffer_, ( char * ) packet->ori_buffer_ + lb_acquire_size, current_usefule_size - lb_acquire_size );
		packet->size_for_translation_ -= lb_acquire_size;
	}

	return ( current_usefule_size - lb_acquire_size );
}

static 
int tcp_prase_logic_packet( ncb_t * ncb, packet_t * packet )
{
	int current_usefule_size;
	int logic_revise_acquire_size;
	nis_event_t c_event;
	tcp_data_t c_data;
	int current_parse_offset;
	int user_size;
	int total_packet_length;

	if ( !packet || !ncb ) {
		return -1;
	}
	if ( 0 == packet->size_for_translation_ ) {
		return -1;
	}

	// 当前可用长度定义为已分析长度加上本次交换长度
	current_usefule_size = packet->analyzed_offset_ + packet->size_for_translation_;
	current_parse_offset = 0;

	// 没有指定包头模板， 直接回调整个TCP包
	if ( 0 == ncb->tcp_tst_.cb_ ) {
		c_event.Ln.Tcp.Link = ( HTCPLINK ) ncb->link;
		c_event.Event = EVT_RECEIVEDATA;
		c_data.e.Packet.Size = current_usefule_size;
		c_data.e.Packet.Data = ( const char * ) ( ( char * ) packet->ori_buffer_ + current_parse_offset + ncb->tcp_tst_.cb_ );
		ncb_callback( ncb, &c_event, &c_data );
		packet->analyzed_offset_ = 0;
		return 0;
	}

	while ( TRUE ) {

		// 如果整体包长度， 不足以填充包头， 则必须进行继续接收操作
		if ( current_usefule_size < ncb->tcp_tst_.cb_ ) break;

		// 底层协议交互给协议模板处理， 处理失败则解包操作无法继续
		if ( ncb->tcp_tst_.parser_( ( char * ) packet->ori_buffer_ + current_parse_offset, ncb->tcp_tst_.cb_, &user_size ) < 0 ) {
			return -1;
		}

		/* 如果用户数据长度超出最大容忍长度，则直接报告为错误, 有可能是恶意攻击 */
		if (user_size > TCP_MAXIMUM_PACKET_SIZE || user_size < 0) {
			return -1;
		}

		// 总包长度 = 用户数据长度 + 底层协议长度
		total_packet_length = user_size + ncb->tcp_tst_.cb_;
		
		// 大包，不存在后续解析， 直接全拷贝后标记为大包等待状态， 从原始缓冲区开始投递IRP
		if ( total_packet_length > TCP_RECV_BUFFER_SIZE ) {
			if ( ncb_mark_lb( ncb, total_packet_length, current_usefule_size, ( char * ) packet->ori_buffer_ + current_parse_offset ) < 0 ) {
				return -1;
			}
			packet->analyzed_offset_ = 0;
			return 0;
		}

		// 当前逻辑包修正请求长度 = 底层协议中记载的用户数据长度 + 包头长度
		logic_revise_acquire_size = total_packet_length;

		// 当前可用长度不足以填充逻辑包长度， 需要继续接收数据
		if ( current_usefule_size < logic_revise_acquire_size ) break;

		// 回调到用户例程, 使用其实地址累加解析偏移， 直接赋予回调例程的结构指针， 因为const限制， 调用线程不应该刻意修改该串的值
		c_event.Ln.Tcp.Link = (HTCPLINK)ncb->link;
		c_event.Event = EVT_RECEIVEDATA;
		if (ncb->optmask & LINKATTR_TCP_FULLY_RECEIVE) {
			c_data.e.Packet.Size = user_size + ncb->tcp_tst_.cb_;
			c_data.e.Packet.Data = (const char *)((char *)packet->ori_buffer_ + current_parse_offset);
		} else {
			c_data.e.Packet.Size = user_size;
			c_data.e.Packet.Data = (const char *)((char *)packet->ori_buffer_ + current_parse_offset + ncb->tcp_tst_.cb_);
		}
		
		ncb_callback( ncb, &c_event, &c_data );

		// 调整当前解析长度
		current_usefule_size -= logic_revise_acquire_size;
		current_parse_offset += logic_revise_acquire_size;
	}

	packet->analyzed_offset_ = current_usefule_size;

	// 有残余数据无法完成组包， 则拷贝到缓冲区原始起点， 并以残余长度作为下一个收包请求的偏移
	if ( ( 0 != current_usefule_size ) && ( 0 != current_parse_offset ) ) {
		memmove( packet->ori_buffer_, ( void * ) ( ( char * ) packet->ori_buffer_ + current_parse_offset ), current_usefule_size );
	}

	return 0;
}

int tcp_update_opts(ncb_t *ncb) {
    if (!ncb) {
        return -1;
    }

    ncb_set_window_size(ncb, SO_RCVBUF, TCP_BUFFER_SIZE );
    ncb_set_window_size(ncb, SO_SNDBUF, TCP_BUFFER_SIZE );
    ncb_set_linger(ncb, 1, 0);
    ncb_set_keepalive(ncb, 1);
     
    tcp_set_nodelay(ncb, 1);   /* 为保证小包效率， 禁用 Nginx 算法 */
    
    return 0;
}

static
int tcp_entry( objhld_t h, ncb_t * ncb, const void * ctx )
{
	tcp_cinit_t *init_ctx = ( tcp_cinit_t * ) ctx;;
	int retval;

	if (!ncb || h < 0 || !ctx) {
		return -1;
	}

	retval = -1;
	memset( ncb, 0, sizeof( ncb_t ) );

	do {
		ncb_init( ncb, kProto_TCP );
		ncb_set_callback(ncb, init_ctx->callback_);
		ncb->link = h;

		ncb->sockfd = so_allocate_asio_socket(SOCK_STREAM, IPPROTO_TCP);
		if (ncb->sockfd < 0) {
			break;
		}

		// 如果是远程连接得到的ncb_t, 操作到此完成
		if ( init_ctx->is_remote_ ) {
			retval = 0;
			break;
		}

		// setsockopt 设置套接字参数
		if (tcp_update_opts(ncb) < 0) {
			break;
		}

		// 创建阶段， 无论是否随机网卡，随机端口绑定， 都先行计入本地地址信息
		// 在执行accept, connect后， 如果是随机端口绑定， 则可以取到实际生效的地址信息
		ncb->l_addr_.sin_family = PF_INET;
		ncb->l_addr_.sin_addr.S_un.S_addr = init_ctx->ip_;
		ncb->l_addr_.sin_port = init_ctx->port_;

		// 描述每个链接上的TCP下级缓冲区大小
		ncb->tcp_usable_sender_cache_ = TCP_BUFFER_SIZE;

		// 将对象绑定到异步IO的完成端口
		if (ioatth(ncb) < 0) {
			break;
		}

		retval = 0;

	} while ( FALSE );

	if ( retval < 0 ) {
		if (ncb->sockfd > 0)  {
			ioclose(ncb);
		}
	}

	return retval;
}

static
void tcp_unload( objhld_t h, void * user_buffer )
{
	nis_event_t c_event;
	tcp_data_t c_data;
	ncb_t *ncb = ( ncb_t * ) user_buffer;
	packet_t *packet;

	if ( !user_buffer ) return;

	// 处理关闭前事件
	c_event.Ln.Tcp.Link = ( HTCPLINK ) h;
	c_event.Event = EVT_PRE_CLOSE;
	c_data.e.LinkOption.OptionLink = ( HTCPLINK ) h;
	if (ncb->tcp_callback_) {
		ncb->tcp_callback_((const void *)&c_event, (const void *)&c_data);
	}

	// 关闭内部套接字
	ioclose(ncb);

	// 处理关闭后事件
	c_event.Event = EVT_CLOSED;
	c_data.e.LinkOption.OptionLink = ( HTCPLINK ) h;
	if (ncb->tcp_callback_) {
		ncb->tcp_callback_((const void *)&c_event, (const void *)&c_data);
	}

	// 如果有未完成的大包， 则将大包内存释放
	ncb_unmark_lb( ncb );

	// 取消所有等待发送的包链
	EnterCriticalSection( &ncb->tcp_lst_lock_ );
	ncb->tcp_pending_cnt_ = 0;
	ncb->tcp_usable_sender_cache_ = 0;
	while (!list_empty(&ncb->tcp_waitting_list_head_)) {
		packet = list_first_entry( &ncb->tcp_waitting_list_head_, packet_t, pkt_lst_entry_ );
		list_del( &packet->pkt_lst_entry_ );
		if ( packet ) {
			freepkt( packet );
		}

		// 递减全局的发送缓冲个数
		InterlockedDecrement( &__tcp_global_sender_cached_cnt ); 
	}
	LeaveCriticalSection( &ncb->tcp_lst_lock_ );

	// 关闭包链的锁
	DeleteCriticalSection( &ncb->tcp_lst_lock_ );

	// 释放用户上下文数据指针
	if ( ncb->ncb_ctx_ && 0 != ncb->ncb_ctx_size_ ) free( ncb->ncb_ctx_ );
}

static
objhld_t tcp_allocate_object(const tcp_cinit_t *ctx) {
	ncb_t *ncb;
	objhld_t h;
	int retval;

	h = objallo( (int)sizeof( ncb_t ), NULL, &tcp_unload, NULL, 0 );
	if ( h < 0 ) {
		return -1;
	}
	ncb = objrefr( h );
	retval = tcp_entry( h, ncb, ctx );
	objdefr( h );
	ncb = NULL;

	if ( retval < 0 ) {
		objclos( h );
		return -1;
	}

	return h;
}

static 
int tcp_syn( ncb_t * ncb_listen )
{
	packet_t *syn_packet;
	objhld_t h;
	tcp_cinit_t ctx;
	int i = 0;

	if (!ncb_listen) {
		return -1;
	}

	if ( allocate_packet( ncb_listen->link, kProto_TCP, kSyn, TCP_ACCEPT_EXTENSION_SIZE, kVirtualHeap, &syn_packet ) < 0 ) {
		return -1;
	}

	do {
		ctx.is_remote_ = TRUE;
		ctx.ip_ = 0;
		ctx.port_ = 0;
		ctx.callback_ = ncb_listen->tcp_callback_;
		h = tcp_allocate_object( &ctx );
		if ( h < 0 ) {
			break;
		}

		// 还是继续沿用当前接收包的内存
		syn_packet->accepted_link = h;

		// 继续抛送接收链接请求
		if (asio_tcp_accept(syn_packet) >= 0) {
			return 0;
		}

		// 投递接受链接请求需要保证成功完成
		// 如果失败， 则关闭本次失败的对端链接， 继续投递接受请求
		objclos( h );

	} while ( i++ < TCP_SYN_REQ_TIMES );

	objclos( ncb_listen->link );
	return -1;
}

static
int tcp_syn_copy( ncb_t * ncb_listen, ncb_t * ncb_accepted, packet_t * packet )
{
	static GUID GUID_GET_ACCEPTEX_SOCK_ADDRS = WSAID_GETACCEPTEXSOCKADDRS;
	static LPFN_GETACCEPTEXSOCKADDRS WSAGetAcceptExSockAddrs = NULL;
	int retval;
	NTSTATUS status;
	int cb_ioctl = 0;
	struct sockaddr *r_addr;
	struct sockaddr_in *pr, *pl;
	struct sockaddr *l_addr;
	int r_len;
	int l_len;
	nis_event_t c_event;
	tcp_data_t c_data;

	if ( !ncb_listen || !ncb_accepted || !packet ) return -1;

	if ( !WSAGetAcceptExSockAddrs ) {
		status = (NTSTATUS)WSAIoctl(ncb_accepted->sockfd, SIO_GET_EXTENSION_FUNCTION_POINTER, &GUID_GET_ACCEPTEX_SOCK_ADDRS,
			sizeof( GUID_GET_ACCEPTEX_SOCK_ADDRS ), &WSAGetAcceptExSockAddrs, sizeof( WSAGetAcceptExSockAddrs ), &cb_ioctl, NULL, NULL );
		if ( !NT_SUCCESS( status ) ) {
			nis_call_ecr("[nshost.tcp.tcp_syn_copy] syscall WSAIoctl for GUID_GET_ACCEPTEX_SOCK_ADDRS failed,NTSTATUS=0x%08X, link:%I64d", status, ncb_accepted->link);
			return -1;
		}
	}

	retval = setsockopt(ncb_accepted->sockfd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&ncb_listen->sockfd, sizeof(SOCKET));
	if ( retval >= 0 ) {
		WSAGetAcceptExSockAddrs( packet->irp_, 0, sizeof( struct sockaddr_in ) + 16, sizeof( struct sockaddr_in ) + 16,
			&l_addr, &l_len, &r_addr, &r_len );

		// 将取得的链接属性赋予accept上来的ncb_t
		pr = ( struct sockaddr_in * )r_addr;
		ncb_accepted->r_addr_.sin_family = pr->sin_family;
		ncb_accepted->r_addr_.sin_port = pr->sin_port;
		ncb_accepted->r_addr_.sin_addr.S_un.S_addr = pr->sin_addr.S_un.S_addr;

		pl = ( struct sockaddr_in * )l_addr;
		ncb_accepted->l_addr_.sin_family = pl->sin_family;
		ncb_accepted->l_addr_.sin_port = pl->sin_port;
		ncb_accepted->l_addr_.sin_addr.S_un.S_addr = pl->sin_addr.S_un.S_addr;

		if (ioatth(ncb_accepted) >= 0) {
			c_event.Ln.Tcp.Link = (HTCPLINK)ncb_listen->link;
			c_event.Event = EVT_TCP_ACCEPTED;
			c_data.e.Accept.AcceptLink = (HTCPLINK)ncb_accepted->link;
			ncb_callback( ncb_accepted, &c_event, &c_data );
			return 0;
		}
	}

	nis_call_ecr("[nshost.tcp.tcp_syn_copy] syscall setsockopt(2) failed(SO_UPDATE_ACCEPT_CONTEXT), error code:%u, link:%I64d", WSAGetLastError(), ncb_accepted->link);
	return -1;
}

/*++
	接收连接操作包含如下部分：

	1. 接收获得的远端套接字应该允许其接收数据
	2. 本地监听套接字应该允许继续接收远端连接
	3. 接收获得的远端套接字应该赋值本地监听套接字的属性
	4. 接收缓冲区与套接字关联， 这里是远端套接字生成其唯一窗口中唯一接收缓冲区的唯一接口点
	--*/
static
void tcp_dispatch_io_syn( packet_t * packet )
{
	ncb_t * ncb_listen;
	ncb_t * ncb_accepted;
	packet_t *recv_packet;
	int retval;

	if (!packet)  {
		return;
	}

	ncb_listen = NULL;
	ncb_accepted = NULL;
	recv_packet = NULL;

	if (tcprefr(packet->link, &ncb_listen) < 0) {
		nis_call_ecr("[nshost.tcp.tcp_dispatch_io_syn] fail to reference link:%I64d", packet->link);
		return;
	}

	// 后续操作不依赖于远端对象的引用成功， 即远端对象即使引用失败， 也不影响下个accept请求的投递
	if (tcprefr(packet->accepted_link, &ncb_accepted) >= 0) {
		retval = tcp_syn_copy( ncb_listen, ncb_accepted, packet );
		if ( retval >= 0 ) {
			retval = allocate_packet(ncb_accepted->link, kProto_TCP, kRecv, TCP_RECV_BUFFER_SIZE, kNonPagedPool, &recv_packet);
			if ( retval >= 0 ) {
				retval = asio_tcp_recv( recv_packet );
			}
		}

		// 此处增加特殊处理， 即使没有成功投递接收请求， 也不应该影响投递下一个ACCEPT请求
		// 但是如果RECV请求无法正确投递， 可以关闭当前ACCEPT上来的链接
		if ( retval < 0 ) {
			objclos( ncb_accepted->link );
		}

		objdefr(ncb_accepted->link);
	}

	if ( tcp_syn( ncb_listen ) < 0 ) {
		objclos(ncb_listen->link);
	}

	objdefr(ncb_listen->link);
}

static
void tcp_dispatch_io_send( packet_t *packet )
{
	ncb_t *ncb;
	objhld_t h;

	if ( !packet ) return;

	// 交换字节数为0的情况， 只能是TCP ACCEPT完成， 其他情况认为是致命错误， 将关闭链接
	if ( 0 == packet->size_for_translation_ ) {
		tcp_shutdown_by_packet( packet );
		return;
	}

	if (tcprefr(packet->link, &ncb) < 0) {
		nis_call_ecr("[nshost.tcp.tcp_dispatch_io_send] fail to reference link:%I64d", packet->link);
		return;
	}

	//
	// 无条件递减 PENDING 计数
	// 在递减计数后满足安全 PENDING 条件, 则尝试继续处理发送队列中的包
	// 针对计数的操作都使用原子进行
	//
	EnterCriticalSection( &ncb->tcp_lst_lock_ );
	ncb->tcp_usable_sender_cache_ += packet->size_for_req_;
	ncb->tcp_pending_cnt_--;
	LeaveCriticalSection( &ncb->tcp_lst_lock_ );
	
	// 递减累积的缓冲区个数(全局/链无关的)
	InterlockedDecrement( &__tcp_global_sender_cached_cnt );

	// 如果尝试发送过程中发生系统调用失败， 则包缓冲区将被销毁， 同时链接将被关闭
	// 继续尝试发下一个包
	tcp_try_write( ncb );

	h = packet->link;

	// 释放本包
	freepkt( packet );
	objdefr( h );
}

static 
void tcp_dispatch_io_recv( packet_t * packet )
{
	ncb_t *ncb;
	int retval;

	if ( !packet )  return;

	// 交换字节数为0的情况， 只能是TCP ACCEPT完成， 其他情况认为是致命错误， 将关闭链接
	if ( 0 == packet->size_for_translation_ ) {
		tcp_shutdown_by_packet( packet );
		return;
	}

	if (tcprefr(packet->link, &ncb) < 0) {
		nis_call_ecr("[nshost.tcp.tcp_dispatch_io_recv] fail to reference link:%I64d", packet->link);
		return;
	}

	do {
		// 
		// 已经被标记为大包， 则应该进行大包的解析和存储
		// 返回值小于零: 大包解析失败, 错误退出
		// 返回值等于零: 数据量不足以本次填充大包, 或刚好填充满本次大包后无任何数据残留
		// 返回值大于零: 大包完成解析后剩余的数据长度(还需要继续解下一个包)
		// 
		if ( ncb_lb_marked( ncb ) ) {
			retval = tcp_lb_assemble( ncb, packet );
			if ( retval <= 0 ) {
				break;
			}
		}

		// 解析 TCP 包为符合协议规范的逻辑包
		retval = tcp_prase_logic_packet( ncb, packet );
		if ( retval < 0 ) {
			break;
		}

		// 单次解包完成， 并不一定可以确认下次投递请求的偏移在缓冲区头部， 所以需要调整
		packet->irp_ = ( void * ) ( ( char * ) packet->ori_buffer_ + packet->analyzed_offset_ );
		packet->size_for_req_ = TCP_RECV_BUFFER_SIZE - packet->analyzed_offset_;

	} while ( FALSE );

	if ( retval >= 0 ) {
		retval = asio_tcp_recv( packet );
	}

	if ( retval < 0 ) {
		objclos(ncb->link);
		freepkt( packet );
	}

	objdefr( ncb->link );
}

static
void tcp_dispatch_io_connected(packet_t * packet){
	ncb_t *ncb;
	nis_event_t c_event;
	tcp_data_t c_data;
	packet_t *packet_rcv;
	int optval;

	if (tcprefr(packet->link, &ncb) < 0) {
		return;
	}

	optval = -1;
	packet_rcv = NULL;

	do {
		// 如果本地采取随机地址结构或端口， 则需要取得唯一生效的地址结构和端口
		if (0 == ncb->l_addr_.sin_port || 0 == ncb->l_addr_.sin_addr.S_un.S_addr) {
			struct sockaddr_in name;
			int name_length = sizeof(name);
			if (getsockname(ncb->sockfd, (struct sockaddr *)&name, &name_length) >= 0) {
				ncb->l_addr_.sin_port = name.sin_port;
				ncb->l_addr_.sin_addr.S_un.S_addr = ntohl(name.sin_addr.S_un.S_addr);/*为了保持兼容性， 这里转换地址为大端*/
			}
		}

		// 异步连接完成后，更新连接对象的上下文属性
		if (setsockopt(ncb->sockfd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) >= 0){
			int namelen = sizeof(ncb->r_addr_);
			getpeername(ncb->sockfd, (struct sockaddr *)&ncb->r_addr_, &namelen);
		}
		
		c_event.Ln.Tcp.Link = ncb->link;
		c_event.Event = EVT_TCP_CONNECTED;
		c_data.e.LinkOption.OptionLink = ncb->link;
		ncb_callback(ncb, &c_event, &c_data);

		// 成功连接对端， 应该投递一个接收数据的IRP， 允许这个连接接收数据
		if (allocate_packet(ncb->link, kProto_TCP, kRecv, TCP_RECV_BUFFER_SIZE, kNonPagedPool, &packet_rcv) < 0) {
			break;
		}

		if (asio_tcp_recv(packet_rcv) < 0) {
			break;
		}

		optval = 0;
		
	} while ( 0 );

	if (packet){
		freepkt(packet);
	}
	objdefr(ncb->link);
	
	if (optval < 0){
		objclos(ncb->link);
		if (packet_rcv){
			freepkt(packet_rcv);
		}
	}
}

static 
void tcp_dispatch_io_exception( packet_t * packet, NTSTATUS status )
{
	ncb_t * ncb;

	if (!packet) {
		return;
	}

	if (tcprefr(packet->link, &ncb) < 0) {
		nis_call_ecr("[nshost.tcp.tcp_dispatch_io_exception] fail to reference link:%I64d", packet->link);
		return;
	}

	nis_call_ecr("[nshost.tcp.tcp_dispatch_io_exception] IO exception catched, NTSTATUS:0x%08X, lnk:%I64d", status, packet->link );

	// 发送异常需要递减未决请求量
	if ( kSend == packet->type_ ) {
		EnterCriticalSection( &ncb->tcp_lst_lock_ );
		ncb->tcp_pending_cnt_--;
		ncb->tcp_usable_sender_cache_ += packet->size_for_req_;
		LeaveCriticalSection( &ncb->tcp_lst_lock_ );

		// 递减全局的发送缓冲个数
		InterlockedDecrement( &__tcp_global_sender_cached_cnt );

		// 尝试下一个发送操作
		tcp_try_write( ncb );
	} else {
		tcp_shutdown_by_packet( packet );
	}

	objdefr( ncb->link );
	
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////

void tcp_dispatch_io_event( packet_t *packet, NTSTATUS status )
{
	if ( !packet ) return;

	if ( !NT_SUCCESS( status ) ) {
		tcp_dispatch_io_exception( packet, status );
		return;
	}

	switch ( packet->type_ ) {
		case kSyn:
			tcp_dispatch_io_syn( packet );
			break;
		case kRecv:
			tcp_dispatch_io_recv( packet );
			break;
		case kSend:
			tcp_dispatch_io_send( packet );
			break;
		case kConnect:
			tcp_dispatch_io_connected(packet);
			break;
		default:
			break;
	}
}

/*++
	重要:
	tcp_shutdown_by_packet 不是一个应该被经常被调用的过程
	这个函数的调用多用于无法处理的异常， 而且这个函数的调用， 除了会释放包的内存外， 还会关闭包内的ncb_t对象
	需要谨慎使用
	--*/
void tcp_shutdown_by_packet( packet_t * packet )
{
	ncb_t * ncb;

	if (!packet) {
		return;
	}

	switch ( packet->type_ ) {
		case kRecv:
		case kSend:
		case kConnect:
			nis_call_ecr("[nshost.tcp.tcp_shutdown_by_packet] type:%d link:%I64d", packet->type_, packet->link);
			objclos(packet->link);
			freepkt( packet );
			break;

			//
			// ACCEPT需要作出特殊处理
			// 1. 不关闭监听的对象
			// 2. 关闭出错的请求中， accept的对象
			// 3. 重新扔出一个accept请求
			//
		case kSyn:
			nis_call_ecr("[nshost.tcp.tcp_shutdown_by_packet] accept link:%I64d, listen link:%I64d", packet->type_, packet->accepted_link, packet->link);
			
			if (tcprefr(packet->link, &ncb) >= 0) {
				tcp_syn( ncb );
				objdefr( ncb->link );
			} else {
				nis_call_ecr("[nshost.tcp.tcp_shutdown_by_packet] fail reference listen object link:%I64d", packet->link);
			}

			objclos(packet->accepted_link);
			freepkt(packet);
			break;

		default:
			break;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
int __stdcall tcp_init()
{
	return so_init( kProto_TCP, 0 );
}

void __stdcall tcp_uninit()
{
	so_uninit( kProto_TCP );
}

int __stdcall tcp_settst( HTCPLINK lnk, const tst_t *tst )
{
	ncb_t *ncb;

	if (!tst) {
		return -1;
	}

	if (tcprefr(lnk, &ncb) < 0) {
		return -1;
	}

	ncb->tcp_tst_.cb_ = tst->cb_;
	ncb->tcp_tst_.builder_ = tst->builder_;
	ncb->tcp_tst_.parser_ = tst->parser_;

	objdefr( ncb->link );
	return 0;
}

int __stdcall tcp_gettst( HTCPLINK lnk, tst_t *tst )
{
	ncb_t *ncb;

	if (tcprefr(lnk, &ncb) < 0) {
		return -1;
	}

	tst->cb_ = ncb->tcp_tst_.cb_;
	tst->builder_ = ncb->tcp_tst_.builder_;
	tst->parser_ = ncb->tcp_tst_.parser_;

	objdefr( ncb->link );
	return 0;
}

HTCPLINK __stdcall tcp_create( tcp_io_callback_t user_callback, const char* l_ipstr, uint16_t l_port )
{
	tcp_cinit_t ctx;

	if (!user_callback) {
		return INVALID_HTCPLINK;
	}

	if ( !l_ipstr ) {
		ctx.ip_ = INADDR_ANY;
	} else {
		struct in_addr l_in_addr;
		if ( inet_pton( AF_INET, l_ipstr, &l_in_addr ) <= 0 ) {
			return INVALID_HTCPLINK;
		}
		ctx.ip_ = l_in_addr.S_un.S_addr;
	}
	ctx.port_ = htons( l_port );
	ctx.callback_ = user_callback;
	ctx.is_remote_ = FALSE;

	return ( HTCPLINK ) tcp_allocate_object( &ctx );
}

/*
 * 关闭响应变更:
 * 对象销毁操作有可能是希望中断某些阻塞操作， 如 connect
 * 故将销毁行为调整为直接关闭描述符后， 通过智能指针销毁对象
 */
void __stdcall tcp_destroy( HTCPLINK lnk )
{
	ncb_t *ncb;

	/* it should be the last reference operation of this object, no matter how many ref-count now. */
	ncb = objreff(lnk);
	if (ncb) {
		nis_call_ecr("[nshost.tcp.tcp_destroy] order to destroy,link:%I64d", ncb->link);
		ioclose(ncb);
		objdefr(lnk);
	}
}

int __stdcall tcp_connect( HTCPLINK lnk, const char* r_ipstr, uint16_t port )
{
	ncb_t *ncb;
	struct sockaddr_in r_addr;
	nis_event_t c_event;
	tcp_data_t c_data;
	packet_t * packet = NULL;

	if (!r_ipstr || 0 == port) {
		return -EINVAL;
	}

	if (tcprefr(lnk, &ncb) < 0) {
		nis_call_ecr( "[nshost.tcp.tcp_connect] fail to reference link:%I64d", lnk );
		return -1;
	}

	if ( inet_pton( AF_INET, r_ipstr, &r_addr.sin_addr ) <= 0 ) {
		objdefr( ncb->link );
		return - 1;
	}
	r_addr.sin_family = AF_INET;
	r_addr.sin_port = htons( port );

	do {
		if (so_bind(&ncb->sockfd, ncb->l_addr_.sin_addr.S_un.S_addr, ncb->l_addr_.sin_port) < 0) {
			break;
		}

		if (connect(ncb->sockfd, (const struct sockaddr *)&r_addr, sizeof(r_addr)) < 0) {
			nis_call_ecr( "[nshost.tcp.tcp_connect] syscall connect(2) failed,target endpoint=%s:%u, error:%u, link:%I64d", r_ipstr, port, WSAGetLastError(), ncb->link );
			break;
		}

		// 如果本地采取随机地址结构或端口， 则需要取得唯一生效的地址结构和端口
		if ( 0 == ncb->l_addr_.sin_port || 0 == ncb->l_addr_.sin_addr.S_un.S_addr ) {
			struct sockaddr_in name;
			int name_length = sizeof( name );
			if (getsockname(ncb->sockfd, (struct sockaddr *)&name, &name_length) >= 0) {
				ncb->l_addr_.sin_port = name.sin_port;
				ncb->l_addr_.sin_addr.S_un.S_addr = ntohl( name.sin_addr.S_un.S_addr );/*为了保持兼容性， 这里转换地址为大端*/
			}
		}

		c_event.Ln.Tcp.Link = lnk;
		c_event.Event = EVT_TCP_CONNECTED;
		c_data.e.LinkOption.OptionLink = lnk;
		ncb_callback( ncb, &c_event, &c_data );

		// 成功连接对端， 应该投递一个接收数据的IRP， 允许这个连接接收数据
		if (allocate_packet(ncb->link, kProto_TCP, kRecv, TCP_RECV_BUFFER_SIZE, kNonPagedPool, &packet) < 0) {
			break;
		}

		if ( asio_tcp_recv( packet ) < 0 ) {
			break;
		}

		inet_pton( AF_INET, r_ipstr, &ncb->r_addr_.sin_addr );
		ncb->r_addr_.sin_port = htons( port );

		objdefr( ncb->link );

		return 0;

	} while ( FALSE );

	freepkt( packet );
	objdefr( ncb->link );
	return -1;
}

int __stdcall tcp_connect2(HTCPLINK lnk, const char* r_ipstr, uint16_t port)
{
	ncb_t *ncb;
	packet_t * packet = NULL;

	if (!r_ipstr || 0 == port) {
		return -EINVAL;
	}

	if (tcprefr(lnk, &ncb) < 0) {
		nis_call_ecr("[nshost.tcp.tcp_connect2] fail to reference link:%I64d", lnk);
		return -1;
	}

	do {
		if (allocate_packet(ncb->link, kProto_TCP, kConnect, 0, kNoAccess, &packet) < 0) {
			break;
		}

		if (inet_pton(AF_INET, r_ipstr, &packet->r_addr_.sin_addr) <= 0) {
			break;
		}
		packet->r_addr_.sin_family = AF_INET;
		packet->r_addr_.sin_port = htons(port);

		if (so_bind(&ncb->sockfd, ncb->l_addr_.sin_addr.S_un.S_addr, ncb->l_addr_.sin_port) < 0) {
			break;
		}

		if (asio_tcp_connect(packet) < 0){
			break;
		}

		objdefr(ncb->link);
		return 0;

	} while (FALSE);

	freepkt(packet);
	objdefr(ncb->link);
	return -1;
}

int __stdcall tcp_listen( HTCPLINK lnk, int block )
{
	ncb_t *ncb;
	int retval;
	int i;

	if (tcprefr(lnk, &ncb) < 0 ) {
		nis_call_ecr( "[nshost.tcp.tcp_listen] fail to reference link:%I64d", lnk );
		return -1;
	}

	do {

		retval = -1;

		if ( 0 == block ) block = TCP_LISTEN_BLOCK_COUNT;

		if (so_bind(&ncb->sockfd, ncb->l_addr_.sin_addr.S_un.S_addr, ncb->l_addr_.sin_port) < 0) {
			break;
		}

		retval = listen(ncb->sockfd, block);
		if ( retval < 0 ) {
			nis_call_ecr("syscall listen failed,error code=%u", WSAGetLastError());
			break;
		}

		i = 0;
		do {
			i++;
			retval = tcp_syn( ncb );
		} while ( ( retval >= 0 ) && ( i < block ) );

	} while ( FALSE );

	objdefr( ncb->link );
	return retval;
}

int __stdcall tcp_write(HTCPLINK lnk, const void *origin, int cb, const nis_serializer_t serializer)
{
	char *buffer;
	ncb_t *ncb;
	packet_t *packet;
	int total_packet_length;
	int retval;

	if (INVALID_HTCPLINK == lnk || cb <= 0 || cb >= TCP_MAXIMUM_PACKET_SIZE || !origin) {
		return -1;
	}

	// 全局对延迟发送的数据队列长度进行保护
	if ( InterlockedIncrement( &__tcp_global_sender_cached_cnt ) >= TCP_MAXIMUM_SENDER_CACHED_CNT ) {
		InterlockedDecrement( &__tcp_global_sender_cached_cnt );
		nis_call_ecr("[nshost.tcp.tcp_write] pre-sent cache overflow.");
		return -1;
	}

	ncb = NULL;
	retval = -1;
	buffer = NULL;
	packet = NULL;

	do {
		retval = tcprefr(lnk, &ncb);
		if (retval < 0) {
			nis_call_ecr("[nshost.tcp.tcp_write] failed reference object, link:%I64d", lnk);
			break;
		}

		if ((!ncb->tcp_tst_.builder_) || (ncb->optmask & LINKATTR_TCP_NO_BUILD)) {
			total_packet_length = cb;
			// 没有指定下层协议的构建例程，则认为传入数据已经完成了下层协议的构建
			if (NULL == (buffer = (char *)malloc(total_packet_length))) {
				break;
			}

			if (serializer) {
				if (serializer(buffer, origin, cb) < 0) {
					break;
				}
			} else {
				memcpy(buffer, origin, cb);
			}
		} else {
			total_packet_length = ncb->tcp_tst_.cb_ + cb;
			if (NULL == (buffer = (char *)malloc(total_packet_length))) {
				break;
			}

			// 协议构建例程负责构建协议头
			if (ncb->tcp_tst_.builder_(buffer, cb) < 0) {
				break;
			}

			if (serializer) {
				if (serializer(buffer + ncb->tcp_tst_.cb_, origin, cb - ncb->tcp_tst_.cb_) < 0) {
					break;
				}
			} else {
				memcpy(buffer + ncb->tcp_tst_.cb_, origin, cb);
			}
		}

		// 分配包
		if ( allocate_packet( ( objhld_t ) lnk, kProto_TCP, kSend, 0, kNoAccess, &packet ) < 0 ) {
			break;
		}

		packet->ori_buffer_ = packet->irp_ = buffer;
		packet->size_for_req_ = total_packet_length;

		// 将包加入待发送队列中
		EnterCriticalSection( &ncb->tcp_lst_lock_ );
		list_add_tail( &packet->pkt_lst_entry_, &ncb->tcp_waitting_list_head_ );
		LeaveCriticalSection( &ncb->tcp_lst_lock_ );

		// 自由判断是否投递异步请求的合适时机
		tcp_try_write( ncb );

		retval = 0;
	} while ( FALSE );

	if ( retval < 0 ) {
		InterlockedDecrement( &__tcp_global_sender_cached_cnt ); // 抬高计数后未能正确插入缓冲队列
		if ( buffer ) {
			free( buffer );
		}
	}

	if ( ncb ) {
		objdefr( ncb->link );
	}
	return retval;
}

int __stdcall tcp_getaddr( HTCPLINK lnk, int nType, uint32_t *ipv4, uint16_t *port )
{
	ncb_t * ncb;
	int retval;

	if ( INVALID_HTCPLINK == lnk || !ipv4 || !port ) {
		return -1;
	}

	if (tcprefr(lnk, &ncb) < 0) {
		nis_call_ecr("[nshost.tcp.tcp_getaddr] fail to reference link:%I64d", lnk);
		return -1;
	}

	retval = 0;

	if ( LINK_ADDR_LOCAL == nType ) {
		*ipv4 = htonl( ncb->l_addr_.sin_addr.S_un.S_addr );
		*port = htons( ncb->l_addr_.sin_port );
	} else {
		if (LINK_ADDR_REMOTE == nType) {
			*ipv4 = htonl(ncb->r_addr_.sin_addr.S_un.S_addr);
			*port = htons(ncb->r_addr_.sin_port);
		} else {
			retval = -1;
		}
	}

	objdefr(ncb->link);

	return retval;
}

int __stdcall tcp_setopt( HTCPLINK lnk, int level, int opt, const char *val, int len )
{
	ncb_t * ncb;
	int retval = -1;

	if ((INVALID_HTCPLINK == lnk) || (!val)) 	{
		return -1;
	}

	if (tcprefr(lnk, &ncb) < 0) {
		nis_call_ecr( "[nshost.tcp.tcp_getaddr] fail to reference link:%I64d", lnk );
		return -1;
	}

	if ( kProto_TCP == ncb->proto_type_ ) {
		retval = setsockopt(ncb->sockfd, level, opt, val, len);
		if ( retval < 0 ) {
			nis_call_ecr("[nshost.tcp.tcp_getaddr]  syscall setsockopt(2) failed,error code:%u, link:%I64d", WSAGetLastError(), ncb->link);
		}
	}

	objdefr( ncb->link );
	return retval;
}

int __stdcall tcp_getopt( HTCPLINK lnk, int level, int opt, char *OptVal, int *len )
{
	ncb_t * ncb;
	int retval = -1;

	if ((INVALID_HTCPLINK == lnk) || (!OptVal) || (!len)) {
		return -1;
	}

	if (tcprefr(lnk, &ncb) < 0) {
		nis_call_ecr( "[nshost.tcp.tcp_getopt] fail to reference link:%I64d", lnk );
		return -1;
	}

	if ( kProto_TCP == ncb->proto_type_ ) {
		retval = getsockopt(ncb->sockfd, level, opt, OptVal, len);
		if ( retval < 0 ) {
			nis_call_ecr("[nshost.tcp.tcp_getopt] syscall failed getsockopt ,error code:%u,link:%I64d", WSAGetLastError(), ncb->link);
		}
	}

	objdefr( ncb->link );
	return retval;
}

//  Minimum supported client
//	Windows 10, version 1703[desktop apps only]
//	Minimum supported server
//	Windows Server 2016[desktop apps only]
int tcp_save_info(ncb_t *ncb, TCP_INFO_v0 *ktcp) {
	//WSAIoctl(ncb->sockfd, SIO_TCP_INFO,
	//	(LPVOID)lpvInBuffer,   // pointer to a DWORD 
	//	(DWORD)cbInBuffer,    // size, in bytes, of the input buffer
	//	(LPVOID)lpvOutBuffer,         // pointer to a TCP_INFO_v0 structure
	//	(DWORD)cbOutBuffer,       // size of the output buffer
	//	(LPDWORD)lpcbBytesReturned,    // number of bytes returned
	//	(LPWSAOVERLAPPED)lpOverlapped,   // OVERLAPPED structure
	//	(LPWSAOVERLAPPED_COMPLETION_ROUTINE)lpCompletionRoutine,  // completion routine
	//	);
	return -1;
}

int tcp_setmss(ncb_t *ncb, int mss) {
    if (ncb && mss > 0) {
        return setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_MAXSEG, (const void *) &mss, sizeof(mss));
    }
    
    return -EINVAL;
}

int tcp_getmss(ncb_t *ncb){
    if (ncb){
        socklen_t lenmss = sizeof(ncb->mss);
        return getsockopt(ncb->sockfd, IPPROTO_TCP, TCP_MAXSEG, (void *__restrict)&ncb->mss, &lenmss);
    }
    return -EINVAL;
}

int tcp_set_nodelay(ncb_t *ncb, int set){
    if (ncb ){
        return setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_NODELAY, (const void *) &set, sizeof ( set));
    }
    
    return -EINVAL;
}

int tcp_get_nodelay(ncb_t *ncb, int *set) {
    if (ncb && set) {
        socklen_t optlen = sizeof(int);
        return getsockopt(ncb->sockfd, IPPROTO_TCP, TCP_NODELAY, (void *__restrict)set, &optlen);
    }
    return -EINVAL;
}

int __stdcall tcp_setattr(HTCPLINK lnk, int attr, int enable) {
	ncb_t *ncb;
	int retval;

	retval = tcprefr(lnk, &ncb);
	if (retval < 0) {
		return retval;
	}

	switch (attr) {
		case LINKATTR_TCP_FULLY_RECEIVE:
		case LINKATTR_TCP_NO_BUILD:
		case LINKATTR_TCP_UPDATE_ACCEPT_CONTEXT:
			(enable > 0) ? (ncb->optmask |= attr) : (ncb->optmask &= ~attr);
			retval = 0;
			break;
		default:
			retval = -EINVAL;
			break;
	}

	objdefr(lnk);
	return retval;
}

int __stdcall tcp_getattr(HTCPLINK lnk, int attr, int *enabled) {
	ncb_t *ncb;
	int retval;

	retval = tcprefr(lnk, &ncb);
	if (retval < 0) {
		return retval;
	}

	if (ncb->optmask & attr) {
		*enabled = 1;
	} else {
		*enabled = 0;
	}

	objdefr(lnk);
	return retval;
}