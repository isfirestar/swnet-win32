#include "network.h"
#include "ncb.h"
#include "packet.h"

////////////////////////////////////////////////////////////////////// PKT //////////////////////////////////////////////////////////////////////
int allocate_packet( objhld_t h, enum proto_type_t proto_type, enum pkt_type_t type, int cbSize, enum page_style_t page_style, packet_t **output )
{
	packet_t *packet;
	int retval = -1;
	void *p_buffer = NULL;

	if ( !output || h < 0 ) return -1;

	packet = ( packet_t * ) malloc( sizeof( packet_t ) );
	if ( !packet ) {
		os_dbg_error("fail to allocate memory for packet." );
		return -1;
	}

	do {
		// 如果不需要填写数据长度， 则由调用线程自行安排包中的数据指针指向
		// 这种方式适用于浅拷贝的发送操作
		if ( cbSize > 0 ) {
			if ( page_style == kNonPagedPool ) {
#pragma warning(suppress: 6387)
				p_buffer = os_lock_virtual_pages( NULL, cbSize );
				if ( !p_buffer ) {
					break;
				}
			} else if ( page_style == kVirtualHeap ) {
				p_buffer = ( char * ) malloc( cbSize );
				if ( !p_buffer ) {
					break;
				}
			} else {
				os_dbg_error("unknown page style [%u] specified.", page_style );
				break;
			}
		} else {
			if ( page_style != kNoAccess ) {
				os_dbg_error("page style [%u] specified and size is zero", page_style );
				break;
			}
		}

		memset( &packet->overlapped_, 0, sizeof( packet->overlapped_ ) );
		packet->type_ = type;
		packet->proto_type_ = proto_type;
		packet->page_style_ = page_style;
		packet->flag_ = 0;
		packet->from_length_ = sizeof( struct sockaddr_in );
		packet->link = h;
		packet->accepted_link = -1;
		if ( kProto_TCP == proto_type ) {
			INIT_LIST_HEAD( &packet->pkt_lst_entry_ );
		} else if ( kProto_UDP == proto_type ) {
			memset( &packet->r_addr_, 0, sizeof( struct sockaddr_in ) );
			memset( &packet->l_addr_, 0, sizeof( struct sockaddr_in ) );
		}
		packet->size_for_req_ = cbSize;
		packet->size_for_translation_ = 0;
		packet->size_completion_ = 0;
		packet->analyzed_offset_ = 0;
		packet->ori_buffer_ = p_buffer;
		packet->grp_packets_ = NULL;
		packet->grp_packets_cnt_ = 0;
		packet->irp_ = p_buffer;

		*output = packet;
		return 0;

	} while ( FALSE );

	free( packet );
	return -1;
}

void free_packet( packet_t * packet )
{
	if ( !packet ) return;

	switch ( packet->page_style_ ) {
		case kNoAccess:
			if ( packet->grp_packets_cnt_ > 0 && packet->grp_packets_ ) {
				free( packet->grp_packets_ );
				packet->grp_packets_cnt_ = 0;
			} else {
				free( packet->ori_buffer_ );
			}
			break;
		case kNonPagedPool:
			os_unlock_and_free_virtual_pages( packet->ori_buffer_, packet->size_for_req_ );
			break;
		case kVirtualHeap:
			free( packet->ori_buffer_ );
			break;
		default:
			break;
	}

	packet->irp_ = NULL;
	free( packet );
}

////////////////////////////////////////////////////////////////////// TCP //////////////////////////////////////////////////////////////////////
int asio_tcp_accept( packet_t * packet )
{
	NTSTATUS status;
	ncb_t * ncb_listen;
	ncb_t * ncb_income;
	static GUID GUID_ACCEPTEX = WSAID_ACCEPTEX;
	LPFN_ACCEPTEX WSAAcceptEx = NULL;
	uint32_t bytes_return = 0;
	int retval;

	if ( !packet ) return -1;

	ncb_listen = objrefr(packet->link);
	if ( !ncb_listen ) return -1;

	status = (NTSTATUS)WSAIoctl(ncb_listen->sockfd, SIO_GET_EXTENSION_FUNCTION_POINTER, &GUID_ACCEPTEX, sizeof(GUID_ACCEPTEX),
		&WSAAcceptEx, sizeof(WSAAcceptEx), &bytes_return, NULL, NULL);
	if (!NT_SUCCESS(status)) {
		ncb_report_debug_information(ncb_listen, "syscall WSAIoctl for WSAID_ACCEPTEX failed,NTSTATUS=0x%08X", status);
		objdefr(ncb_listen->link);
		return -1;
	}

	retval = 0;
	ncb_income = objrefr(packet->accepted_link);
	if ( ncb_income ) {
		if ( !WSAAcceptEx( ncb_listen->sockfd, ncb_income->sockfd, packet->irp_, 0,
			sizeof( struct sockaddr_in ) + 16, sizeof( struct sockaddr_in ) + 16, &packet->size_for_translation_, &packet->overlapped_ ) ) {
			if ( ERROR_IO_PENDING != WSAGetLastError() ) {
				ncb_report_debug_information(ncb_listen, "syscall WSAAcceptEx failed,error code=%u", WSAGetLastError() );
				retval = -1;
			}
		}
		objdefr( ncb_income->link );
	}
	objdefr( ncb_listen->link );
	return retval;
}

int asio_tcp_send( packet_t *packet )
{
	WSABUF wsb[1];
	int retval;
	ncb_t *ncb;

	if ( !packet ) return -1;

	ncb = objrefr(packet->link);
	if ( !ncb ) return -1;

	wsb[0].len = packet->size_for_req_;
	wsb[0].buf = ( CHAR * ) packet->irp_;

	retval = WSASend( ncb->sockfd, wsb, 1, &packet->size_completion_,
#if USES_LOCAL_ROUTE_TABLE
		0,
#else
		MSG_DONTROUTE,
#endif
		&packet->overlapped_, NULL );
	if ( retval < 0 ) {
		if ( ERROR_IO_PENDING == WSAGetLastError() ) {
			retval = 0;
		} else {
			ncb_report_debug_information(ncb, "syscall WSASend failed,error code=%u", WSAGetLastError() );
		}
	}

	objdefr( ncb->link );
	return retval;
}

int asio_tcp_recv( packet_t * packet )
{
	WSABUF wsb[1];
	int retval;
	ncb_t *ncb;

	if ( !packet ) return -1;

	ncb = objrefr(packet->link);
	if ( !ncb ) return -1;

	wsb[0].len = packet->size_for_req_;
	wsb[0].buf = ( CHAR * ) packet->irp_;

	retval = WSARecv( ncb->sockfd, wsb, 1, &packet->size_completion_, &packet->flag_, &packet->overlapped_, NULL );
	if ( retval < 0 ) {
		if ( ERROR_IO_PENDING == WSAGetLastError() ) {
			retval = 0;
		} else {
			ncb_report_debug_information(ncb, "syscall WSARecv failed,error code=%u", WSAGetLastError() );
		}
	}

	objdefr( ncb->link );
	return retval;
}

int asio_tcp_connect(packet_t *packet){
	NTSTATUS status;
	static GUID GUID_CONNECTEX = WSAID_CONNECTEX;
	LPFN_CONNECTEX WSAConnectEx = NULL;
	uint32_t bytes_return = 0;
	ncb_t *ncb;
	int retval;

	if (!packet) return -1;

	ncb = objrefr(packet->link);
	if (!ncb) return -1;

	retval = -1;
	do {
		status = (NTSTATUS)WSAIoctl(ncb->sockfd, SIO_GET_EXTENSION_FUNCTION_POINTER, &GUID_CONNECTEX, sizeof(GUID_CONNECTEX),
			&WSAConnectEx, sizeof(WSAConnectEx), &bytes_return, NULL, NULL);
		if (!NT_SUCCESS(status)) {
			ncb_report_debug_information(ncb, "syscall WSAIoctl for WSAID_CONNECTEX failed,NTSTATUS=0x%08X", status);
			break;
		}

		if (!WSAConnectEx(ncb->sockfd, (const struct sockaddr *)&packet->r_addr_,
			sizeof(struct sockaddr), NULL, 0, NULL, &packet->overlapped_))
		{
			uint32_t errcode = WSAGetLastError();
			if (ERROR_IO_PENDING != errcode) {
				ncb_report_debug_information(ncb, "syscall ConnectEx failed,errcode=%u", WSAGetLastError());
				break;
			}
		}

		retval = 0;
	} while ( 0 );

	objdefr(ncb->link);
	return retval;
}

////////////////////////////////////////////////////////////////////// UDP //////////////////////////////////////////////////////////////////////
int asio_udp_recv( packet_t * packet )
{
	WSABUF wsb[1];
	int retval = 0;
	ncb_t *ncb;

	if ( !packet ) return -1;

	ncb = objrefr(packet->link);
	if ( !ncb ) return -1;

	wsb[0].len = packet->size_for_req_;
	wsb[0].buf = ( CHAR * ) packet->irp_;

	retval = WSARecvFrom(ncb->sockfd, wsb, 1, &packet->size_completion_, &packet->flag_,
		( struct sockaddr * )&packet->r_addr_, &packet->from_length_, &packet->overlapped_, NULL );
	if ( retval < 0 ) {
		if ( ERROR_IO_PENDING == WSAGetLastError() ) {
			retval = 0;
		} else {
			ncb_report_debug_information(ncb, "syscall WSARecvFrom failed,error code=%u", WSAGetLastError() );
		}
	}

	objdefr( ncb->link );
	return retval;
}

int syio_udp_send( packet_t * packet, const char *r_ipstr, uint16_t r_port )
{
	WSABUF wsb[1];
	int retval;
	ncb_t * ncb;

	if ( !packet ) {
		return -1;
	}

	ncb = objrefr(packet->link);
	if ( !ncb ) {
		return -1;
	}

	if ( inet_pton( AF_INET, r_ipstr, &packet->r_addr_.sin_addr ) <= 0 ) {
		objdefr(packet->link);
		return -1;
	}
	packet->r_addr_.sin_family = AF_INET;
	packet->r_addr_.sin_port = htons( r_port );

	wsb[0].len = packet->size_for_req_;
	wsb[0].buf = ( CHAR * ) packet->irp_;

	retval = WSASendTo(ncb->sockfd, wsb, 1, &packet->size_completion_, 0,//MSG_DONTROUTE,
		( const struct sockaddr * )&packet->r_addr_, sizeof( struct sockaddr ), NULL, NULL );
	if ( retval < 0 ) {
		ncb_report_debug_information(ncb, "syscall WSASendTo failed,error code=%u", WSAGetLastError() );
	}

	free_packet( packet );
	objdefr( ncb->link );
	return retval;
}

////////////////////////////////////////////////////////////////////// common/GPM //////////////////////////////////////////////////////////////////////
int syio_v_disconnect( ncb_t * ncb )
{
	struct sockaddr_in addr;
	int retval;

	// UDP的协议对象需要处理伪连接
	if ( kProto_UDP != ncb->proto_type_ ) return -1;

	if ( 0 == ncb->connected_ ) return 0;

	ncb->connected_ = 0;
	memset( &addr, 0, sizeof( addr ) );

	// 面向无连接的0地址结构连接操作， 即为反向设置伪连接， 反向设置伪连接后， UDP对象可以继续处理源连接以外的地址所得的包
	retval = WSAConnect(ncb->sockfd, (const struct sockaddr *)&addr, sizeof(addr), NULL, NULL, NULL, NULL);
	if ( retval < 0 ) {
		ncb_report_debug_information(ncb, "syscall WSAConnect failed,error code=%u", WSAGetLastError() );
	}
	return retval;
}

int syio_v_connect( ncb_t * ncb, const struct sockaddr_in *r_addr )
{
	int retval;

	retval = WSAConnect(ncb->sockfd, (const struct sockaddr *)r_addr, sizeof(struct sockaddr_in), NULL, NULL, NULL, NULL);
	if ( retval < 0 ) {
		ncb_report_debug_information(ncb, "syscall WSAConnect failed,error code=%u", WSAGetLastError() );
		return -1;
	}

	// 使用伪连接后， UDP的本地地址可以被确定
	if ( 0 == ncb->l_addr_.sin_port || 0 == ncb->l_addr_.sin_addr.S_un.S_addr ) {
		struct sockaddr_in s_name;
		int name_length = sizeof( s_name );
		if (getsockname(ncb->sockfd, (struct sockaddr *)&s_name, &name_length) >= 0) {
			ncb->l_addr_.sin_port = s_name.sin_port;
			ncb->l_addr_.sin_addr.S_un.S_addr = htonl( s_name.sin_addr.S_un.S_addr );/*为了保持兼容性， 这里转换地址为大端*/
		}
	}

	return 0;
}

int syio_grp_send( packet_t * packet )
{
	static GUID GUID_TRANSMIT_PACKETS = WSAID_TRANSMITPACKETS;
	static LPFN_TRANSMITPACKETS WSATransmitPackets = NULL;
	NTSTATUS status;
	int bytes_return;
	ncb_t *ncb;
	int retval = -1;

	ncb = objrefr(packet->link);
	if ( !ncb ) {
		return -1;
	}

	do {
		if ( !WSATransmitPackets ) {
			status = WSAIoctl(ncb->sockfd, SIO_GET_EXTENSION_FUNCTION_POINTER, &GUID_TRANSMIT_PACKETS, sizeof(GUID_TRANSMIT_PACKETS), &WSATransmitPackets,
				sizeof( WSATransmitPackets ), &bytes_return, NULL, NULL );
			if ( !NT_SUCCESS( status ) ) {
				ncb_report_debug_information(ncb, "syscall WSAIoctl for GUID_TRANSMIT_PACKETS failed,NTSTATUS=0x%08X", status );
				break;
			}
		}

		if (!WSATransmitPackets(ncb->sockfd, packet->grp_packets_, packet->grp_packets_cnt_, MAXDWORD, NULL, 0)) {
			ncb_report_debug_information( ncb, "syscall WSATransmitPackets failed,error code=%u", WSAGetLastError() );
			break;
		}

		retval = 0;
	} while ( FALSE );

	objdefr( ncb->link );
	return retval;
}