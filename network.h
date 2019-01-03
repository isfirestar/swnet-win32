#if !defined (NETWORK_COMMON_LIBRARY_20120803_1)
#define NETWORK_COMMON_LIBRARY_20120803_1

#include "os.h"


//////////////////////////////////////////////////// �׽��ֶ���(so, socket object) ��ؽӿ�/////////////////////////////////////////////////////////////////////////////
enum proto_type_t {
	kProto_Unknown = -1,
	kProto_IP,
	kProto_UDP,
	kProto_TCP,
	kProto_MaximumId
};

#if !defined STATUS_HOST_UNREACHABLE
#define STATUS_HOST_UNREACHABLE          ((NTSTATUS)0xC000023DL)
#endif

#if !defined STATUS_PROTOCOL_UNREACHABLE
#define STATUS_PROTOCOL_UNREACHABLE      ((NTSTATUS)0xC000023EL)
#endif

#if !defined STATUS_PORT_UNREACHABLE
#define STATUS_PORT_UNREACHABLE          ((NTSTATUS)0xC000023FL)
#endif

#if !defined UDP_MAXIMUM_USER_DATA_SIZE
#define UDP_MAXIMUM_USER_DATA_SIZE	(1472)		/* MTU - UDP_P_SIZE - IP_P_SIZE */
#endif

void so_close( SOCKET *s );
int so_init( enum proto_type_t proto_type, int th_cnt );
void so_uninit( enum proto_type_t ProtoType );
SOCKET so_allocate_asio_socket( int type, int protocol );
int so_asio_count();
int so_bind( SOCKET *s, uint32_t ipv4, uint16_t Port);
void so_dispatch_io_event( OVERLAPPED *o, int size_for_translation );

#endif