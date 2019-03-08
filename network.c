#include "network.h"
#include "io.h"
#include "ncb.h"
#include "packet.h"
#include "mxx.h"

#define		MINIMUM_IRPS_PER_OBJECT		(4)
#define		MAXIMUM_IRPS_PER_OBJECT		(10)

extern void udp_dispatch_io_event( packet_t * packet, NTSTATUS status );
extern void udp_shutdown( packet_t * NccPacket );
extern void tcp_dispatch_io_event( packet_t * packet, NTSTATUS status );
extern void tcp_shutdwon_by_packet( packet_t * packet );

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static char __so_protocol_initialized[kProto_MaximumId] = { 0 };
static long __so_startup = 0;

////////////////////////////////////////////////////				������ؽӿ�ʵ��				/////////////////////////////////////////////////////////////////////////////
int so_init( enum proto_type_t proto_type, int th_cnt )
{
	struct WSAData wsd;

	if ( 1 == InterlockedIncrement( ( volatile long * ) &__so_startup )) {

		if ( WSAStartup( MAKEWORD( 2, 2 ), &wsd ) < 0 ) {
			nis_call_ecr( "[nshost.network.so_init] syscall WSAStartup failed,error code=%u", WSAGetLastError() );
			InterlockedDecrement( ( volatile long * ) &__so_startup );
			return -1;
		}

		// ���һ�γ�ʼ������ IOCP �������ĳ�ʼ��
		if (ioinit(th_cnt) < 0) {
			WSACleanup();
			InterlockedDecrement( ( volatile long * ) &__so_startup );
			return -1;
		}

		objinit();
	} else {
		InterlockedDecrement( ( volatile long * ) &__so_startup );
	}

	if ( ( kProto_TCP == proto_type ) && ( __so_protocol_initialized[kProto_TCP] ) ) {
		__so_protocol_initialized[kProto_TCP] = 1;
	}

	if ( ( kProto_UDP == proto_type ) && ( 0 == __so_protocol_initialized[kProto_UDP] ) ) {
		__so_protocol_initialized[kProto_UDP] = 1;
	}
	return 0;
}

void so_uninit( enum proto_type_t ProtoType )
{
	int i;

	if ( kProto_TCP != ProtoType && kProto_UDP != ProtoType )  return;

	__so_protocol_initialized[ProtoType] = FALSE;

	// ֻ������Э�鶼�Ѿ������ͷţ� �������շ���ʼ��
	for ( i = 0; i < kProto_MaximumId; i++ ) {
		if ( __so_protocol_initialized[i] ) {
			return;
		}
	}

	objuninit();
	iouninit();
	WSACleanup();
}

void so_dispatch_io_event( OVERLAPPED *pOvlp, int transfer_bytes )
{
	packet_t *	packet = ( packet_t * ) pOvlp;
	NTSTATUS status;

	if ( !packet ) return;

	// ���ʵ�ʵĽ������ݳ���
	packet->size_for_translation_ = transfer_bytes;

	// ���ڲ�������Ϣ�Բ�����ʽͶ�ݸ�Э�������д�����
	status = ( NTSTATUS ) pOvlp->Internal;

	// ��Э�����ͷַ��쳣
	switch ( packet->proto_type_ ) {
		case kProto_UDP:
			udp_dispatch_io_event( packet, status );
			break;
		case kProto_TCP:
			tcp_dispatch_io_event( packet, status );
			break;
		default:
			nis_call_ecr( "[nshost.network.so_dispatch_io_event] unknown packet protocol type %u dispatch to network.", packet->proto_type_ );
			break;
	}
}

int so_asio_count()
{
	return 1;
	//int iocp_th_cnt;
	//int io_pre_object;

	//iocp_th_cnt = iocp_thcnts();
	//if ( iocp_th_cnt <= 0 ) return -1;

	//if ( iocp_th_cnt < MINIMUM_IRPS_PER_OBJECT ) {
	//	io_pre_object = MINIMUM_IRPS_PER_OBJECT;
	//} else if ( iocp_th_cnt > MAXIMUM_IRPS_PER_OBJECT ) {
	//	io_pre_object = MAXIMUM_IRPS_PER_OBJECT;
	//} else {
	//	io_pre_object = iocp_th_cnt;
	//}

	//return io_pre_object;
}

SOCKET so_allocate_asio_socket( int type, int protocol )
{
	SOCKET s = WSASocket( PF_INET, type, protocol, NULL, 0, WSA_FLAG_OVERLAPPED );
	if ( s < 0 ) {
		nis_call_ecr( "[nshost.network.so_allocate_asio_socket] syscall WSASocket failed,error code=%u", WSAGetLastError() );
	}
	return s;
}

int so_bind( SOCKET *s, uint32_t ip, uint16_t port )
{
	int retval;
	struct sockaddr_in addr;

	if ( !s ) return -1;

	addr.sin_addr.S_un.S_addr = ip;
	addr.sin_port = port;
	addr.sin_family = AF_INET;

	retval = bind( *s, ( const struct sockaddr * )&addr, sizeof( struct sockaddr ) );
	if ( retval < 0 ) {
		nis_call_ecr( "[nshost.network.so_bind] syscall bind(2) failed,error code=%u", WSAGetLastError() );
	}
	return retval;
}