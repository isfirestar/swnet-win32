#include "network.h"
#include "ncb.h"
#include "packet.h"
#include "iocp.h"

#include <assert.h>

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
#define TCP_MAXIMUM_SENDER_CACHED_CNT				( 5120 ) // ��ÿ����64KB��, �����Խ��� 327MB �ķ��Ͷѻ�
static long __tcp_global_sender_cached_cnt = 0; // TCP ȫ�ֻ���ķ��Ͱ�����(δ������������)

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void tcp_shutdwon_by_packet( packet_t * packet );
static int tcp_save_info( ncb_t *ncb );
static int tcp_setmss( ncb_t *ncb, int mss );
static int tcp_getmss( ncb_t *ncb );
static int tcp_set_nodelay( ncb_t *ncb, int set );
static int tcp_get_nodelay( ncb_t *ncb, int *set );

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static
ncb_t *tcprefr( objhld_t handle ) {
	ncb_t *ncb;

	if ( handle < 0 ) {
		return NULL;
	}

	ncb = objrefr( handle );
	if ( ncb ) {
		if ( ncb->proto_type_ == kProto_TCP ) {
			return ncb;
		}
		objdefr( handle );
	}
	return NULL;
}

static 
void tcp_try_write( ncb_t * ncb )
{
	packet_t *next_packet;

	if ( !ncb ) return;

	EnterCriticalSection( &ncb->tcp_lst_lock_ );

	if ( list_empty( &ncb->tcp_waitting_list_head_ ) ) {
		LeaveCriticalSection( &ncb->tcp_lst_lock_ );
		return;
	}

	next_packet = list_first_entry( &ncb->tcp_waitting_list_head_, packet_t, pkt_lst_entry_ );

	//
	// ����(����ʹ�÷����ж�)
	// 1. ��ǰû���κ�δ���������¼�������, ������������һ�������(��������������ƴ���������)
	// 2. ��ǰ���Ŀ��û�����������һ�����ĳ��ȣ� ������������ϵͳͶ�ݰ�
	// 
	if ( ncb->tcp_pending_cnt_ > 0 ) {
		if ( next_packet->size_for_req_ > ncb->tcp_usable_sender_cache_ ) {
			LeaveCriticalSection( &ncb->tcp_lst_lock_ );
			return;
		}
	}

	list_del_init( &next_packet->pkt_lst_entry_ );

	/*	�ۼ�δ��������δ������	*/
	ncb->tcp_pending_cnt_++;
	ncb->tcp_usable_sender_cache_ -= next_packet->size_for_req_;

	/*
	������ PENDING ���󣬲������ IOCP ����
	��ʱ�϶�Ϊһ���޷�����Ĵ��� ֱ��ִ�жϿ�����
	ͬʱ, ������Ҫ������ע�������� ����Ҫ�ͷű����ڴ�����췢�ͻ�����
	*/
	if ( asio_tcp_send( next_packet ) < 0 ) {
		tcp_shutdwon_by_packet( next_packet );
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

	// ��ǰ���ó��ȶ���Ϊ�ѷ������ȼ��ϱ��ν�������
	current_usefule_size = packet->size_for_translation_;

	// �Ӵ���������У�ȡ�ô����Ҫ�ĳ���
	if ( ncb->tcp_tst_.parser_( ncb->lb_data_, ncb->tcp_tst_.cb_, &user_size ) < 0 ) {
		return -1;
	}

	// ����������󳤶� = �û����ݳ��� + �ײ�Э�鳤��
	logic_revise_acquire_size = user_size + ncb->tcp_tst_.cb_;

	// ����������� = ��Ҫ���������� - �Ѿ�����������ƫ��
	lb_acquire_size = logic_revise_acquire_size - ncb->lb_cpy_offset_;

	// ��ǰ�������ݳ��ȣ� ����������������, �������ݿ����� �������ƫ�ƣ� ������������
	if ( current_usefule_size < lb_acquire_size ) {
		assert(ncb->lb_cpy_offset_ + current_usefule_size <= ncb->lb_length_);
		memcpy( ncb->lb_data_ + ncb->lb_cpy_offset_, ( char * ) packet->ori_buffer_, current_usefule_size );
		ncb->lb_cpy_offset_ += current_usefule_size;
		return 0;
	}

	// ����������� �򽫴����������ص����û�����
	assert(ncb->lb_cpy_offset_ + lb_acquire_size <= ncb->lb_length_);
	memcpy( ncb->lb_data_ + ncb->lb_cpy_offset_, ( char * ) packet->ori_buffer_, lb_acquire_size );
	c_event.Ln.Tcp.Link = ( HTCPLINK ) ncb->link;
	c_event.Event = EVT_RECEIVEDATA;
	c_data.e.Packet.Size = user_size;
	c_data.e.Packet.Data = ( const char * ) ( ( char * ) ncb->lb_data_ + ncb->tcp_tst_.cb_ );
	ncb_callback( ncb, &c_event, &c_data );

	// һ�δ���Ľ����Ѿ���ɣ� ����ncb_t�еĴ���ֶ�
	ncb_unmark_lb( ncb );

	// ������Ǹպ���������� ��:��Ч���������࣬ ��Ӧ�ý���Ч����ʣ�೤�ȸ�֪�����̣߳� ���ʽ�����һ�ֵĽ������
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

	// ��ǰ���ó��ȶ���Ϊ�ѷ������ȼ��ϱ��ν�������
	current_usefule_size = packet->analyzed_offset_ + packet->size_for_translation_;
	current_parse_offset = 0;

	// û��ָ����ͷģ�壬 ֱ�ӻص�����TCP��
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

		// �����������ȣ� ����������ͷ�� �������м������ղ���
		if ( current_usefule_size < ncb->tcp_tst_.cb_ ) break;

		// �ײ�Э�齻����Э��ģ�崦�� ����ʧ�����������޷�����
		if ( ncb->tcp_tst_.parser_( ( char * ) packet->ori_buffer_ + current_parse_offset, ncb->tcp_tst_.cb_, &user_size ) < 0 ) {
			return -1;
		}

		// �ܰ����� = �û����ݳ��� + �ײ�Э�鳤��
		total_packet_length = user_size + ncb->tcp_tst_.cb_;

		// ����������ں��������� ֱ��ȫ��������Ϊ����ȴ�״̬�� ��ԭʼ��������ʼͶ��IRP
		if ( total_packet_length > TCP_RECV_BUFFER_SIZE ) {
			if ( ncb_mark_lb( ncb, total_packet_length, current_usefule_size, ( char * ) packet->ori_buffer_ + current_parse_offset ) < 0 ) {
				return -1;
			}
			packet->analyzed_offset_ = 0;
			return 0;
		}

		// ��ǰ�߼����������󳤶� = �ײ�Э���м��ص��û����ݳ��� + ��ͷ����
		logic_revise_acquire_size = total_packet_length;

		// ��ǰ���ó��Ȳ���������߼������ȣ� ��Ҫ������������
		if ( current_usefule_size < logic_revise_acquire_size ) break;

		// �ص����û�����, ʹ����ʵ��ַ�ۼӽ���ƫ�ƣ� ֱ�Ӹ���ص����̵Ľṹָ�룬 ��Ϊconst���ƣ� �����̲߳�Ӧ�ÿ����޸ĸô���ֵ
		c_event.Ln.Tcp.Link = (HTCPLINK)ncb->link;
		c_event.Event = EVT_RECEIVEDATA;
		c_data.e.Packet.Size = user_size;
		c_data.e.Packet.Data = ( const char * ) ( ( char * ) packet->ori_buffer_ + current_parse_offset + ncb->tcp_tst_.cb_ );
		ncb_callback( ncb, &c_event, &c_data );

		// ������ǰ��������
		current_usefule_size -= logic_revise_acquire_size;
		current_parse_offset += logic_revise_acquire_size;
	}

	packet->analyzed_offset_ = current_usefule_size;

	// �в��������޷��������� �򿽱���������ԭʼ��㣬 ���Բ��೤����Ϊ��һ���հ������ƫ��
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
    ncb_set_linger(ncb, 0, 1);
    ncb_set_keepalive(ncb, 1);
     
    tcp_set_nodelay(ncb, 1);   /* Ϊ��֤С��Ч�ʣ� ���� Nginx �㷨 */
    tcp_save_info(ncb);
    
    return 0;
}

static
int tcp_entry( objhld_t h, ncb_t * ncb, const void * ctx )
{
	tcp_cinit_t *init_ctx = ( tcp_cinit_t * ) ctx;;
	int retval;
	nis_event_t c_event;
	tcp_data_t c_data;

	if ( !ncb || h < 0|| !ctx ) return -1;

	memset( ncb, 0, sizeof( ncb_t ) );

	do {

		retval = -1;

		ncb_init( ncb, kProto_TCP );
		ncb->link = h;

		ncb->sockfd = so_allocate_asio_socket(SOCK_STREAM, IPPROTO_TCP);
		if (ncb->sockfd < 0) break;

		// �����Զ�����ӵõ���ncb_t, �����������
		if ( init_ctx->is_remote_ ) {
			retval = 0;
			break;
		}

		// setsockopt �����׽��ֲ���
		if ( tcp_update_opts( ncb) < 0 ) break;

		// �����׶Σ� �����Ƿ��������������˿ڰ󶨣� �����м��뱾�ص�ַ��Ϣ
		// ��ִ��accept, connect�� ���������˿ڰ󶨣� �����ȡ��ʵ����Ч�ĵ�ַ��Ϣ
		ncb->l_addr_.sin_family = PF_INET;
		ncb->l_addr_.sin_addr.S_un.S_addr = init_ctx->ip_;
		ncb->l_addr_.sin_port = init_ctx->port_;

		// ����ÿ�������ϵ�TCP�¼���������С
		ncb->tcp_usable_sender_cache_ = TCP_BUFFER_SIZE;

		// ������󶨵��첽IO����ɶ˿�
		if (iocp_bind(ncb->sockfd) < 0) break;

		retval = 0;

	} while ( FALSE );

	if ( retval < 0 ) {
		// �������̵�ʧ��, ��Ҫ����һ��ͨ���쳣
		c_event.Event = EVT_EXCEPTION;
		c_event.Ln.Tcp.Link = INVALID_HTCPLINK;
		c_data.e.Exception.SubEvent = EVT_CREATED;
		c_data.e.Exception.ErrorCode = ( long ) WSAGetLastError();
		init_ctx->callback_( ( const void * ) &c_event, ( const void * ) &c_data );
		if (ncb->sockfd > 0) so_close(&ncb->sockfd);
	} else {
		ncb_set_callback( ncb, init_ctx->callback_ );
		c_event.Event = EVT_CREATED;
		c_event.Ln.Tcp.Link = ( HTCPLINK ) ncb->link;
		c_data.e.LinkOption.OptionLink = ( HTCPLINK ) ncb->link;
		ncb_callback( ncb, &c_event, &c_data );
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

	// ����ر�ǰ�¼�
	c_event.Ln.Tcp.Link = ( HTCPLINK ) h;
	c_event.Event = EVT_PRE_CLOSE;
	c_data.e.LinkOption.OptionLink = ( HTCPLINK ) h;
	ncb->tcp_callback_( ( const void * ) &c_event, ( const void * ) &c_data );

	// �ر��ڲ��׽���
	so_close(&ncb->sockfd);

	// ����رպ��¼�
	c_event.Event = EVT_CLOSED;
	c_data.e.LinkOption.OptionLink = ( HTCPLINK ) h;
	ncb->tcp_callback_( ( const void * ) &c_event, ( const void * ) &c_data );

	// �����δ��ɵĴ���� �򽫴���ڴ��ͷ�
	ncb_unmark_lb( ncb );

	// ȡ�����еȴ����͵İ���
	EnterCriticalSection( &ncb->tcp_lst_lock_ );
	ncb->tcp_pending_cnt_ = 0;
	ncb->tcp_usable_sender_cache_ = 0;
	while (!list_empty(&ncb->tcp_waitting_list_head_)) {
		packet = list_first_entry( &ncb->tcp_waitting_list_head_, packet_t, pkt_lst_entry_ );
		list_del( &packet->pkt_lst_entry_ );
		if ( packet ) {
			free_packet( packet );
		}

		// �ݼ�ȫ�ֵķ��ͻ������
		InterlockedDecrement( &__tcp_global_sender_cached_cnt ); 
	}
	LeaveCriticalSection( &ncb->tcp_lst_lock_ );

	// �رհ�������
	DeleteCriticalSection( &ncb->tcp_lst_lock_ );

	// �ͷ��û�����������ָ��
	if ( ncb->ncb_ctx_ && 0 != ncb->ncb_ctx_size_ ) free( ncb->ncb_ctx_ );
}

static
objhld_t tcp_allocate_object(const tcp_cinit_t *ctx) {
	ncb_t *ncb;
	objhld_t h;
	int retval;

	h = objallo( (int)sizeof( ncb_t ), &objentry, &tcp_unload, NULL, 0 );
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

	if ( !ncb_listen ) return -1;

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

		// ���Ǽ������õ�ǰ���հ����ڴ�
		syn_packet->accepted_link = h;

		// �������ͽ�����������
		if ( asio_tcp_accept( syn_packet ) >= 0 ) return 0;

		// Ͷ�ݽ�������������Ҫ��֤�ɹ����
		// ���ʧ�ܣ� ��رձ���ʧ�ܵĶԶ����ӣ� ����Ͷ�ݽ�������
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
			ncb_report_debug_information(ncb_accepted, "syscall WSAIoctl for GUID_GET_ACCEPTEX_SOCK_ADDRS failed,NTSTATUS=0x%08X", status );
			return -1;
		}
	}

	retval = setsockopt(ncb_accepted->sockfd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&ncb_listen->sockfd, sizeof(SOCKET));
	if ( retval >= 0 ) {
		WSAGetAcceptExSockAddrs( packet->irp_, 0, sizeof( struct sockaddr_in ) + 16, sizeof( struct sockaddr_in ) + 16,
			&l_addr, &l_len, &r_addr, &r_len );

		// ��ȡ�õ��������Ը���accept������ncb_t
		pr = ( struct sockaddr_in * )r_addr;
		ncb_accepted->r_addr_.sin_family = pr->sin_family;
		ncb_accepted->r_addr_.sin_port = pr->sin_port;
		ncb_accepted->r_addr_.sin_addr.S_un.S_addr = pr->sin_addr.S_un.S_addr;

		pl = ( struct sockaddr_in * )l_addr;
		ncb_accepted->l_addr_.sin_family = pl->sin_family;
		ncb_accepted->l_addr_.sin_port = pl->sin_port;
		ncb_accepted->l_addr_.sin_addr.S_un.S_addr = pl->sin_addr.S_un.S_addr;

		if (iocp_bind(ncb_accepted->sockfd) >= 0) {
			c_event.Ln.Tcp.Link = (HTCPLINK)ncb_listen->link;
			c_event.Event = EVT_TCP_ACCEPTED;
			c_data.e.Accept.AcceptLink = (HTCPLINK)ncb_accepted->link;
			ncb_callback( ncb_accepted, &c_event, &c_data );
			return 0;
		}
	}

	ncb_report_debug_information(ncb_accepted, "syscall setsockopt failed( SO_UPDATE_ACCEPT_CONTEXT), error code=%u", WSAGetLastError() );
	return -1;
}

/*++
	�������Ӳ����������²��֣�

	1. ���ջ�õ�Զ���׽���Ӧ���������������
	2. ���ؼ����׽���Ӧ�������������Զ������
	3. ���ջ�õ�Զ���׽���Ӧ�ø�ֵ���ؼ����׽��ֵ�����
	4. ���ջ��������׽��ֹ����� ������Զ���׽���������Ψһ������Ψһ���ջ�������Ψһ�ӿڵ�
	--*/
static
void tcp_dispatch_io_syn( packet_t * packet )
{
	ncb_t * ncb_listen;
	ncb_t * ncb_accepted;
	packet_t *recv_packet;
	int retval;

	if ( !packet )  return;

	ncb_listen = tcprefr(packet->link);
	if ( !ncb_listen ) {
		os_dbg_warn("fail to reference ncb object:0x%08X", packet->link);
		return;
	}

	// ����������������Զ�˶�������óɹ��� ��Զ�˶���ʹ����ʧ�ܣ� Ҳ��Ӱ���¸�accept�����Ͷ��
	ncb_accepted = tcprefr(packet->accepted_link);
	if ( ncb_accepted ) {
		retval = tcp_syn_copy( ncb_listen, ncb_accepted, packet );
		if ( retval >= 0 ) {
			retval = allocate_packet(ncb_accepted->link, kProto_TCP, kRecv, TCP_RECV_BUFFER_SIZE, kNonPagedPool, &recv_packet);
			if ( retval >= 0 ) {
				retval = asio_tcp_recv( recv_packet );
			}
		}

		// �˴��������⴦�� ��ʹû�гɹ�Ͷ�ݽ������� Ҳ��Ӧ��Ӱ��Ͷ����һ��ACCEPT����
		// �������RECV�����޷���ȷͶ�ݣ� ���Թرյ�ǰACCEPT����������
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

	// �����ֽ���Ϊ0������� ֻ����TCP ACCEPT��ɣ� ���������Ϊ���������� ���ر�����
	if ( 0 == packet->size_for_translation_ ) {
		tcp_shutdwon_by_packet( packet );
		return;
	}

	ncb = tcprefr(packet->link);
	if ( !ncb ) {
		os_dbg_warn("fail to reference ncb object:0x%08X", packet->link);
		return;
	}

	//
	// �������ݼ� PENDING ����
	// �ڵݼ����������㰲ȫ PENDING ����, ���Լ��������Ͷ����еİ�
	// ��Լ����Ĳ�����ʹ��ԭ�ӽ���
	//
	EnterCriticalSection( &ncb->tcp_lst_lock_ );
	ncb->tcp_usable_sender_cache_ += packet->size_for_req_;
	ncb->tcp_pending_cnt_--;
	LeaveCriticalSection( &ncb->tcp_lst_lock_ );
	
	// �ݼ��ۻ��Ļ���������(ȫ��/���޹ص�)
	InterlockedDecrement( &__tcp_global_sender_cached_cnt );

	// ������Է��͹����з���ϵͳ����ʧ�ܣ� ����������������٣� ͬʱ���ӽ����ر�
	// �������Է���һ����
	tcp_try_write( ncb );

	h = packet->link;

	// �ͷű���
	free_packet( packet );
	objdefr( h );
}

static 
void tcp_dispatch_io_recv( packet_t * packet )
{
	ncb_t *ncb;
	int retval;

	if ( !packet )  return;

	// �����ֽ���Ϊ0������� ֻ����TCP ACCEPT��ɣ� ���������Ϊ���������� ���ر�����
	if ( 0 == packet->size_for_translation_ ) {
		tcp_shutdwon_by_packet( packet );
		return;
	}

	ncb = tcprefr(packet->link);
	if ( !ncb ) {
		os_dbg_warn("fail to reference ncb object:0x%08X", packet->link);
		return;
	}

	do {
		// 
		// �Ѿ������Ϊ����� ��Ӧ�ý��д���Ľ����ʹ洢
		// ����ֵС����: �������ʧ��, �����˳�
		// ����ֵ������: �����������Ա��������, ��պ���������δ�������κ����ݲ���
		// ����ֵ������: �����ɽ�����ʣ������ݳ���(����Ҫ��������һ����)
		// 
		if ( ncb_lb_marked( ncb ) ) {
			retval = tcp_lb_assemble( ncb, packet );
			if ( retval <= 0 ) {
				break;
			}
		}

		// ���� TCP ��Ϊ����Э��淶���߼���
		retval = tcp_prase_logic_packet( ncb, packet );
		if ( retval < 0 ) {
			break;
		}

		// ���ν����ɣ� ����һ������ȷ���´�Ͷ�������ƫ���ڻ�����ͷ���� ������Ҫ����
		packet->irp_ = ( void * ) ( ( char * ) packet->ori_buffer_ + packet->analyzed_offset_ );
		packet->size_for_req_ = TCP_RECV_BUFFER_SIZE - packet->analyzed_offset_;

	} while ( FALSE );

	if ( retval >= 0 ) {
		retval = asio_tcp_recv( packet );
	}

	if ( retval < 0 ) {
		objclos(ncb->link);
		free_packet( packet );
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

	ncb = tcprefr(packet->link);
	if (!ncb){
		return;
	}

	optval = -1;
	packet_rcv = NULL;

	do {
		// ������ز�ȡ�����ַ�ṹ��˿ڣ� ����Ҫȡ��Ψһ��Ч�ĵ�ַ�ṹ�Ͷ˿�
		if (0 == ncb->l_addr_.sin_port || 0 == ncb->l_addr_.sin_addr.S_un.S_addr) {
			struct sockaddr_in name;
			int name_length = sizeof(name);
			if (getsockname(ncb->sockfd, (struct sockaddr *)&name, &name_length) >= 0) {
				ncb->l_addr_.sin_port = name.sin_port;
				ncb->l_addr_.sin_addr.S_un.S_addr = ntohl(name.sin_addr.S_un.S_addr);/*Ϊ�˱��ּ����ԣ� ����ת����ַΪ���*/
			}
		}

		// �첽������ɺ󣬸������Ӷ��������������
		if (setsockopt(ncb->sockfd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) >= 0){
			int namelen = sizeof(ncb->r_addr_);
			getpeername(ncb->sockfd, (struct sockaddr *)&ncb->r_addr_, &namelen);
		}
		
		c_event.Ln.Tcp.Link = ncb->link;
		c_event.Event = EVT_TCP_CONNECTED;
		c_data.e.LinkOption.OptionLink = ncb->link;
		ncb_callback(ncb, &c_event, &c_data);

		// �ɹ����ӶԶˣ� Ӧ��Ͷ��һ���������ݵ�IRP�� ����������ӽ�������
		if (allocate_packet(ncb->link, kProto_TCP, kRecv, TCP_RECV_BUFFER_SIZE, kNonPagedPool, &packet_rcv) < 0) {
			break;
		}

		if (asio_tcp_recv(packet_rcv) < 0) {
			break;
		}

		optval = 0;
		
	} while ( 0 );

	if (packet){
		free_packet(packet);
	}
	objdefr(ncb->link);
	
	if (optval < 0){
		objclos(ncb->link);
		if (packet_rcv){
			free_packet(packet_rcv);
		}
	}
}

static 
void tcp_dispatch_io_exception( packet_t * packet, NTSTATUS status )
{
	ncb_t * ncb;

	if ( !packet ) return;

	ncb = tcprefr(packet->link);
	if ( !ncb ) {
		os_dbg_warn("fail to reference ncb object:0x%08X", packet->link);
		return;
	}

	os_dbg_warn("IO exception catched on lnk [0x%08X], NTSTATUS=0x%08X", packet->link, status);

	// �����쳣��Ҫ�ݼ�δ��������
	if ( kSend == packet->type_ ) {
		EnterCriticalSection( &ncb->tcp_lst_lock_ );
		ncb->tcp_pending_cnt_--;
		ncb->tcp_usable_sender_cache_ += packet->size_for_req_;
		LeaveCriticalSection( &ncb->tcp_lst_lock_ );

		// �ݼ�ȫ�ֵķ��ͻ������
		InterlockedDecrement( &__tcp_global_sender_cached_cnt );

		// ������һ�����Ͳ���
		tcp_try_write( ncb );
	} else {
		tcp_shutdwon_by_packet( packet );
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
	��Ҫ:
	tcp_shutdwon_by_packet ����һ��Ӧ�ñ����������õĹ���
	��������ĵ��ö������޷�������쳣�� ������������ĵ��ã� ���˻��ͷŰ����ڴ��⣬ ����رհ��ڵ�ncb_t����
	��Ҫ����ʹ��
	--*/
void tcp_shutdwon_by_packet( packet_t * packet )
{
	ncb_t * ncb;

	if ( !packet ) return;

	switch ( packet->type_ ) {

		case kRecv:
		case kSend:
		case kConnect: 
			{
				objclos(packet->link);
				free_packet( packet );
			}
			break;

			//
			// ACCEPT��Ҫ�������⴦��
			// 1. ���رռ����Ķ���
			// 2. �رճ���������У� accept�Ķ���
			// 3. �����ӳ�һ��accept����
			//
		case kSyn:
			{
				objclos(packet->accepted_link);
				free_packet( packet );

				ncb = tcprefr(packet->link);
				if ( ncb ) {
					tcp_syn( ncb );
					objdefr( ncb->link );
				}
			}
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

	if ( !tst ) return -1;

	ncb = tcprefr( lnk );
	if ( !ncb ) return -1;

	ncb->tcp_tst_.cb_ = tst->cb_;
	ncb->tcp_tst_.builder_ = tst->builder_;
	ncb->tcp_tst_.parser_ = tst->parser_;

	objdefr( ncb->link );
	return 0;
}

int __stdcall tcp_gettst( HTCPLINK lnk, tst_t *tst )
{
	ncb_t *ncb;

	if ( !tst ) return -1;

	ncb = tcprefr( lnk );
	if ( !ncb ) return -1;

	tst->cb_ = ncb->tcp_tst_.cb_;
	tst->builder_ = ncb->tcp_tst_.builder_;
	tst->parser_ = ncb->tcp_tst_.parser_;

	objdefr( ncb->link );
	return 0;
}

HTCPLINK __stdcall tcp_create( tcp_io_callback_t user_callback, const char* l_ipstr, uint16_t l_port )
{
	tcp_cinit_t ctx;

	if ( !user_callback ) return INVALID_HTCPLINK;

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
 * �ر���Ӧ���:
 * �������ٲ����п�����ϣ���ж�ĳЩ���������� �� connect
 * �ʽ�������Ϊ����Ϊֱ�ӹر��������� ͨ������ָ�����ٶ���
 */
void __stdcall tcp_destroy( HTCPLINK lnk )
{
	ncb_t *ncb;
    objhld_t hld = (objhld_t)lnk;
    
    ncb = objrefr(hld);
    if (ncb) {
		so_close(&ncb->sockfd);
        objdefr(hld);
    }
    
    objclos(hld);
}

int __stdcall tcp_connect( HTCPLINK lnk, const char* r_ipstr, uint16_t port )
{
	ncb_t *ncb;
	struct sockaddr_in r_addr;
	nis_event_t c_event;
	tcp_data_t c_data;
	packet_t * packet = NULL;

	if ( !r_ipstr || 0 == port ) return -EINVAL;

	ncb = tcprefr( lnk );
	if ( !ncb ) {
		os_dbg_warn( "fail to reference ncb object:0x%08X", lnk );
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
			os_dbg_warn( "syscall failed,target endpoint=%s:%u, error code=%u", r_ipstr, port, WSAGetLastError() );
			break;
		}

		// ������ز�ȡ�����ַ�ṹ��˿ڣ� ����Ҫȡ��Ψһ��Ч�ĵ�ַ�ṹ�Ͷ˿�
		if ( 0 == ncb->l_addr_.sin_port || 0 == ncb->l_addr_.sin_addr.S_un.S_addr ) {
			struct sockaddr_in name;
			int name_length = sizeof( name );
			if (getsockname(ncb->sockfd, (struct sockaddr *)&name, &name_length) >= 0) {
				ncb->l_addr_.sin_port = name.sin_port;
				ncb->l_addr_.sin_addr.S_un.S_addr = ntohl( name.sin_addr.S_un.S_addr );/*Ϊ�˱��ּ����ԣ� ����ת����ַΪ���*/
			}
		}

		c_event.Ln.Tcp.Link = lnk;
		c_event.Event = EVT_TCP_CONNECTED;
		c_data.e.LinkOption.OptionLink = lnk;
		ncb_callback( ncb, &c_event, &c_data );

		// �ɹ����ӶԶˣ� Ӧ��Ͷ��һ���������ݵ�IRP�� ����������ӽ�������
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

	free_packet( packet );
	objdefr( ncb->link );
	return -1;
}

int __stdcall tcp_connect2(HTCPLINK lnk, const char* r_ipstr, uint16_t port)
{
	ncb_t *ncb;
	packet_t * packet = NULL;

	if (!r_ipstr || 0 == port) return -EINVAL;

	ncb = tcprefr(lnk);
	if (!ncb) {
		os_dbg_warn("fail to reference ncb object:0x%08X", lnk);
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

	free_packet(packet);
	objdefr(ncb->link);
	return -1;
}

int __stdcall tcp_listen( HTCPLINK lnk, int block )
{
	ncb_t *ncb;
	int retval;
	int i;

	ncb = tcprefr( lnk );
	if ( !ncb ) {
		os_dbg_warn( "fail to reference ncb object:0x%08X", lnk );
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
			ncb_report_debug_information(ncb, "syscall listen failed,error code=%u", WSAGetLastError() );
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

static int tcp_maker( void *data, int cb, void *context ) {
	if ( data && cb > 0 && context ) {
		memcpy( data, context, cb );
		return 0;
	}
	return -1;
}

int __stdcall tcp_write( HTCPLINK lnk, int cb, int( __stdcall *data_filler )( void *, int, void * ), void *par )
{
	char *buffer;
	ncb_t *ncb;
	packet_t *packet;
	int total_packet_length;
	int retval;

	if ( INVALID_HTCPLINK == lnk || cb <= 0 || cb >= TCP_MAXIMUM_PACKET_SIZE || !data_filler ) return -1;

	// ȫ�ֶ��ӳٷ��͵����ݶ��г��Ƚ��б���
	if ( InterlockedIncrement( &__tcp_global_sender_cached_cnt ) >= TCP_MAXIMUM_SENDER_CACHED_CNT ) {
		InterlockedDecrement( &__tcp_global_sender_cached_cnt );
		return -1;
	}

	ncb = NULL;
	retval = -1;
	buffer = NULL;
	packet = NULL;
	do {
		
		ncb = tcprefr( lnk );
		if ( !ncb ) {
			break;
		}

		// δ�ƶ������͹����²�Э��Ĵ�������
		if ( !ncb->tcp_tst_.builder_ || !ncb->tcp_tst_.parser_ ) {
			break;
		}
		total_packet_length = ncb->tcp_tst_.cb_ + cb;


		if ( NULL == ( buffer = ( char * ) malloc( total_packet_length ) ) ) {
			break;
		}

		// Э�鹹�����̸��𹹽�Э��ͷ
		if ( ncb->tcp_tst_.builder_( buffer, cb ) < 0 ) {
			break;
		}

		// �û�������䱾�η��͵�������
		if ( data_filler ) {
			if ( data_filler( buffer + ncb->tcp_tst_.cb_, cb, par ) < 0 ) {
				break;
			}
		} else {
			if ( tcp_maker( buffer + ncb->tcp_tst_.cb_, cb, par ) < 0 ) {
				break;
			}
		}

		// �����
		if ( allocate_packet( ( objhld_t ) lnk, kProto_TCP, kSend, 0, kNoAccess, &packet ) < 0 ) {
			break;
		}

		packet->ori_buffer_ = packet->irp_ = buffer;
		packet->size_for_req_ = total_packet_length;

		// ������������Ͷ�����
		EnterCriticalSection( &ncb->tcp_lst_lock_ );
		list_add_tail( &packet->pkt_lst_entry_, &ncb->tcp_waitting_list_head_ );
		LeaveCriticalSection( &ncb->tcp_lst_lock_ );

		// �����ж��Ƿ�Ͷ���첽����ĺ���ʱ��
		tcp_try_write( ncb );

		retval = 0;
	} while ( FALSE );

	if ( retval < 0 ) {
		InterlockedDecrement( &__tcp_global_sender_cached_cnt ); // ̧�߼�����δ����ȷ���뻺�����
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

	if ( INVALID_HTCPLINK == lnk || !ipv4 || !port ) {
		return -1;
	}

	if ( LINK_ADDR_LOCAL == nType ) {
		ncb = tcprefr( lnk );
		if ( !ncb ) {
			os_dbg_warn( "fail to reference ncb object:0x%08X", lnk );
			return -1;
		}

		*ipv4 = htonl( ncb->l_addr_.sin_addr.S_un.S_addr );
		*port = htons( ncb->l_addr_.sin_port );

		objdefr( ncb->link );
		return 0;
	}

	if ( LINK_ADDR_REMOTE == nType ) {
		ncb = tcprefr( lnk );
		if ( !ncb ) {
			os_dbg_warn( "fail to reference ncb object:0x%08X", lnk );
			return -1;
		}

		*ipv4 = htonl( ncb->r_addr_.sin_addr.S_un.S_addr );
		*port = htons( ncb->r_addr_.sin_port );

		objdefr( ncb->link );
		return 0;
	}

	return -1;
}

int __stdcall tcp_setopt( HTCPLINK lnk, int level, int opt, const char *val, int len )
{
	ncb_t * ncb;
	int retval = -1;

	if ( ( INVALID_HTCPLINK == lnk ) || ( !val ) ) 	return -1;

	ncb = tcprefr( lnk );
	if ( !ncb ) {
		os_dbg_warn( "fail to reference ncb object:0x%08X", lnk );
		return -1;
	}

	if ( kProto_TCP == ncb->proto_type_ ) {
		retval = setsockopt(ncb->sockfd, level, opt, val, len);
		if ( retval < 0 ) {
			ncb_report_debug_information(ncb, "syscall setsockopt failed,error code=%u", WSAGetLastError() );
		}
	}

	objdefr( ncb->link );
	return retval;
}

int __stdcall tcp_getopt( HTCPLINK lnk, int level, int opt, char *OptVal, int *len )
{
	ncb_t * ncb;
	int retval = -1;

	if ( ( INVALID_HTCPLINK == lnk ) || ( !OptVal ) || ( !len ) ) return -1;

	ncb = tcprefr( lnk );
	if ( !ncb ) {
		os_dbg_warn( "fail to reference ncb object:0x%08X", lnk );
		return -1;
	}

	if ( kProto_TCP == ncb->proto_type_ ) {
		retval = getsockopt(ncb->sockfd, level, opt, OptVal, len);
		if ( retval < 0 ) {
			ncb_report_debug_information(ncb, "syscall failed getsockopt ,error code=%u", WSAGetLastError() );
		}
	}

	objdefr( ncb->link );
	return retval;
}

int tcp_save_info(ncb_t *ncb) {
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