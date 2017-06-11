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

	ncb = udprefr( packet->h_ );
	if ( !ncb ) {
		os_dbg_warn("fail to reference ncb object:0x%08X", packet->h_ );
		return;
	}

	// �����������¼�
	c_event.Ln.Udp.Link = ( HUDPLINK ) packet->h_;
	c_event.Event = EVT_RECEIVEDATA;

	// ֪ͨ�����¼�
	c_data.e.Packet.Size = packet->size_for_translation_;
	c_data.e.Packet.Data = ( const char * ) packet->irp_;
	c_data.e.Packet.RemotePort = ntohs( packet->r_addr_.sin_port );
	if ( !inet_ntop( AF_INET, &packet->r_addr_.sin_addr, c_data.e.Packet.RemoteAddress, _countof( c_data.e.Packet.RemoteAddress ) ) ) {
		RtlZeroMemory( c_data.e.Packet.RemoteAddress, _countof( c_data.e.Packet.RemoteAddress ) );
	}
	ncb_callback( ncb, &c_event, ( void * ) &c_data );

	// ���ʱ���Ѿ�������˴���, ������һ���첽�� IRP Ͷ����ȥ
	// ���β����� �������������ڵ�һ�ж��������Ժ�����
	packet->size_for_translation_ = 0;
	asio_udp_recv( packet );

	objdefr( ncb->h_ );
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

	ncb = udprefr( packet->h_ );
	if ( !ncb ) {
		os_dbg_warn("fail to reference ncb object:0x%08X", packet->h_ );
		return;
	}

	os_dbg_warn( "IO exception catched on lnk [0x%08X], NTSTATUS=0x%08X", packet->h_, status );

	// �ص�֪ͨ�쳣�¼�
	c_event.Ln.Udp.Link = ( HUDPLINK ) packet->h_;
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

	// ���հ��ڴ�
	udp_shutdown( packet );
}

static int udp_setopt_i( SOCKET *s, int boardcast_enabled )
{
	static const int RECV_BUFFER_SIZE = SO_MAX_MSG_SIZE;
	static const int SEND_BUFFER_SIZE = MAXWORD;
	int retval;
	int enable;
	int reuse_addr;

	if ( !s ) return -1;

	do {

		enable = boardcast_enabled;

		// UDP �ĵײ�Э�黺����ʹ�ñ�׼ǿ��Ϊ SO_MAX_MSG_SIZE
		retval = setsockopt( *s, SOL_SOCKET, SO_RCVBUF, ( const char * ) &RECV_BUFFER_SIZE, sizeof( RECV_BUFFER_SIZE ) );
		if ( retval < 0 ) {
			break;
		}
		retval = setsockopt( *s, SOL_SOCKET, SO_SNDBUF, ( const char * ) &SEND_BUFFER_SIZE, sizeof( SEND_BUFFER_SIZE ) );
		if ( retval < 0 ) {
			break;
		}

		// �˿ڸ���
		reuse_addr = 1;
		retval = setsockopt( *s, SOL_SOCKET, SO_REUSEADDR, ( const char * ) &reuse_addr, sizeof( reuse_addr ) );
		if ( retval < 0 ) {
			break;
		}

		// ����� UDP �Ӷ���ϣ����Ϊ�㲥ʹ��, ��Ӧ�ô��Ϲ㲥���
		if ( enable ) {
			retval = setsockopt( *s, SOL_SOCKET, SO_BROADCAST, ( const char * ) &enable, sizeof( enable ) );
			if ( retval < 0 ) {
				break;
			}
		}

		return 0;

	} while ( FALSE );

	os_dbg_error("syscall failed,error code=%u", WSAGetLastError() );
	return retval;
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
	ncb->h_ = h;
	ncb->sock_ = so_allocate_asio_socket( SOCK_DGRAM, IPPROTO_UDP );
	if ( ncb->sock_ < 0 ) {
		return -1;
	}

	do {
		if ( so_bind( &ncb->sock_, ctx->ipv4_, ctx->port_ ) < 0 ) {
			break;
		}

		// ֱ����д��ַ�ṹ
		ncb->l_addr_.sin_family = AF_INET;
		ncb->l_addr_.sin_addr.S_un.S_addr = ctx->ipv4_;
		ncb->l_addr_.sin_port = ctx->port_;

		// �����������˿ڰ󶨣� ��Ӧ�û�ȡ��ʵ�İ󶨶˿�
		if ( 0 == ctx->port_ ) {
			int len = sizeof( conn_addr );
			if ( getsockname( ncb->sock_, ( SOCKADDR* )&conn_addr, &len ) >= 0 ) {
				ncb->l_addr_.sin_port = ntohs( conn_addr.sin_port );
			}
		}

		// UDP���
		ncb->flag_ = ctx->ncb_flag_;

		// ����һЩ�׽��ֲ���
		if ( udp_setopt_i( &ncb->sock_, ( ncb->flag_ & UDP_FLAG_BROADCAST ) ) < 0 ) {
			ncb_report_debug_information( ncb, "syscall setsockopt failed,error code=%u",WSAGetLastError()  );
			break;
		}

		// �ر���Զ˱�ǿ����Ч�������µı��� io ����
		// �������Դ������»ᵼ�� WSAECONNRESET ���󷵻�
		behavior = -1;
		if ( WSAIoctl( ncb->sock_, SIO_UDP_CONNRESET, &behavior, sizeof( behavior ), NULL, 0, &bytes_returned, NULL, NULL ) < 0 ) {
			ncb_report_debug_information(ncb, "syscall WSAIoctl failed to control SIO_UDP_CONNRESET,error cdoe=%u ", WSAGetLastError() );
			break;
		}

		// ������󶨵��첽IO����ɶ˿�
		if ( iocp_bind( ncb->sock_ ) < 0 ) break;

		// �ص��û���ַ�� ֪ͨ�����̣߳� UDP �Ӷ����Ѿ��������
		ncb_set_callback( ncb, ctx->f_user_callback_ );
		c_event.Event = EVT_CREATED;
		c_event.Ln.Udp.Link = ( HUDPLINK ) ncb->h_;
		c_data.e.LinkOption.OptionLink = ( HUDPLINK ) ncb->h_;
		ncb_callback( ncb, ( const void * ) &c_event, ( const void * ) ncb->h_ );

		return 0;

	} while ( FALSE );

	so_close( &ncb->sock_ );
	return -1;
}

static void udp_unload( objhld_t h, void * user_buffer )
{
	nis_event_t c_event;
	udp_data_t c_data;
	ncb_t * ncb = ( ncb_t * ) user_buffer;

	if ( !user_buffer ) return;

	// �ر�ǰ�¼�
	c_event.Ln.Udp.Link = ( HUDPLINK ) h;
	c_event.Event = EVT_PRE_CLOSE;
	c_data.e.LinkOption.OptionLink = ( HUDPLINK ) h;
	ncb_callback( ncb, &c_event, &h );

	// �ر��ڲ��׽���
	so_close( &ncb->sock_ );

	// �رպ��¼�
	c_event.Event = EVT_CLOSED;
	ncb_callback( ncb, &c_event, &h );

	// �ͷ��û�����������ָ��
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

	// �ö��������¼�����ڴ���̿��ܴ��ڵ��쳣
	pkt_array = ( packet_t ** ) malloc( a_size );
	if ( !pkt_array ) return NULL;
	memset( pkt_array, 0, a_size );

	// ����ÿ�� UDP ���� ������һ�� 8KB ���������첽�ȴ���
	// Ϊ�˱�������ڴ��ȱҳӰ�����ܣ� ���ｫ��Ƭ�ڴ�����Ϊ�Ƿ�ҳ��
	for ( i = 0; i < cnt; i++ ) {
		retval = allocate_packet( h, kProto_UDP, kRecv, PAGE_SIZE, kNonPagedPool, &pkt_array[i] );
		if ( retval < 0 ) {
			break;
		}
	}

	// ������̷������� Ӧ�û�����ͷ����еİ��ڴ�
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
		objclos( packet->h_ );
		free_packet( packet );
	}
}

void udp_dispatch_io_event( packet_t *packet, NTSTATUS status )
{
	// �����ֽ���Ϊ0������� ֻ����TCP ACCEPT��ɣ� ���������Ϊ���������� ���ر�����
	if ( 0 == packet->size_for_translation_ ) {
		udp_shutdown( packet );
		return;
	}

	// ����IO����ж��� ���IOʧ�ܣ� Ӧ��ͨ���ص��ķ�ʽͨ������߳�
	if ( !NT_SUCCESS( status ) ) {
		udp_dispatch_io_exception( packet, status );
		return;
	}

	// �շ��¼��������
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

	// Ԥ����������Ͷ�ݸ�ϵͳ�� IRP ��ǡ������
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

	// Ͷ�ݽ�������
	for ( i = 0; i < cnts; i++ ) {
		asio_udp_recv( pkt_array[i] );
	}
	free( pkt_array );
	return ( HUDPLINK ) h;
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

	if ( data_filler( buffer, cb, par ) < 0 ) {
		free( buffer );
		return -1;
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

		objdefr( ncb->h_ );
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
		retval = ( ( kProto_UDP == ncb->proto_type_ ) ? setsockopt( ncb->sock_, level, opt, val, len ) : ( -1 ) );
	}
	objdefr( ncb->h_ );
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
		retval = ( ( kProto_UDP == ncb->proto_type_ ) ? getsockopt( ncb->sock_, level, opt, val, len ) : ( -1 ) );
	}
	objdefr( ncb->h_ );
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

		// ����ع�
		for ( i = 0; i < grp->Count; i++ ) {
			if ( !grp->Items[i].Data ) {
				break;
			}
			free( grp->Items[i].Data );
			grp->Items[i].Data = NULL;
		}

	} while ( FALSE );

	objdefr( ncb->h_ );
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

	objdefr( ncb->h_ );
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

	objdefr( ncb->h_ );
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

		// ���������
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

		// ͬ����ɺ��Զ��ͷŰ��� �����е� grp ֻ�ܽ��� xx_release_grp �����ͷ�
		free_packet( packet );
	} while ( FALSE );

	objdefr( ncb->h_ );
	return retval;
}