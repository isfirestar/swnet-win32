#include "network.h"
#include "ncb.h"

#include "mxx.h"

#include <assert.h>

/*++
	��Ҫ:

	�޷�������쳣:
	����Զ���һ��socket�ɹ�accept����connect��ȥ��socket�� ������recv��ȡ���ݣ������ϣ����ط��ͻ�������������� ����[TCP WINDOW]���� ���ص���[WSASend]Ӧ�õõ�һ��ʧ��
	������ʵ֤�����ᣬ ����ϵͳ������ֹ�Ľ���WSASend���첽���� ��ȻIRP�޷�����ɣ� �����������޶ѻ��� ���յ���ϵͳ����
	�������صĺ���ǣ� ��Ϊ[TCP WINDOW]�Ѿ�Ϊ0�� ��ʹ��ʱ�Զ˶Ͽ����ӣ� ����Ҳ�޷��յ� fin ack ��Ӧ��  ����õ�IOCP��WSASend�����ߵ�֪ͨ�� ��ᵼ�³���ܿ������
	Ϊ�˱���������⣬ ��û�и���׼�ķ������ǰ�� ֻ������ÿ�����ӵĴ������� ��һ������ϱ�֤��ȫ��

	ʵ��֤���� ��ʹ�����������쳣����� ����closesocket�ĵ��ã� ���Իָ��쳣�� ���õ�һ��STATUS_LOCAL_DISCONNECT(0xC000013B)�Ľ���� ��ˣ� ֻҪ��֤�����ڷ����쳣�׶α���
	�ͺ���������

	�����κ�ԭ���µ�PENDING�������� NCB_MAXIMUM_TCP_SENDER_IRP_PEDNING_COUNT, tcp_write ���̵��ý���ֱ�ӷ���ʧ��

	(2016-05-26)
	�� TCP ���ͻ��������������ƣ��ڴﵽָ������֮ǰ�������Խ�������Ͷ�ݸ�����ϵͳ, ��� ncb_t::tcp_usable_sender_cache_ ��ʹ��
	--*/

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define NCB_UDP_ROOT_IDX							(0)
#define NCB_TCP_ROOT_IDX							(1)
#define NCB_MAXIMUM_PROTOCOL_ROOT					(3)

#define NCB_UDP_HASHMAP_SIZE						(59)
#define NCB_TCP_HASHMAP_SIZE						(599)

void ncb_callback( ncb_t * ncb, const nis_event_t *c_evet, const void * c_data )
{
	if ( ncb ) {
		if ( kProto_UDP == ncb->proto_type_ && ncb->udp_callback_) {
			ncb->udp_callback_( c_evet, c_data );
		}

		if ( kProto_TCP == ncb->proto_type_ && ncb->tcp_callback_) {
			ncb->tcp_callback_( c_evet, c_data );
		}
	}
}

void ncb_init( ncb_t * ncb, enum proto_type_t proto_type )
{
	if ( ncb ) {
		memset( ncb, 0, sizeof( ncb_t ) );
		ncb->sockfd = INVALID_SOCKET;
		ncb->proto_type_ = proto_type;
		InitializeCriticalSection( &ncb->tcp_lst_lock_ );
		INIT_LIST_HEAD( &ncb->tcp_waitting_list_head_ );
	}
}

int ncb_mark_lb( ncb_t *ncb, int cb, int current_size, void * source )
{
	if ( !ncb || ( cb < current_size ) ) return -1;

	ncb->lb_length_ = cb;
	ncb->lb_data_ = ( char * ) malloc( ncb->lb_length_ );
	if ( !ncb->lb_data_ ) {
		nis_call_ecr( "[nshost.ncb.ncb_mark_lb] fail to allocate memory for ncb->lb_data_, request size=%u", cb );
		return -1;
	}
	
	ncb->lb_cpy_offset_ = current_size;
	if (0 == current_size) {
		return 0;
	}
	
	/* �����ͬʱ��������Ҫ���� */
	memcpy( ncb->lb_data_, source, ncb->lb_cpy_offset_ );
	return 0;
}

void ncb_unmark_lb( ncb_t *ncb )
{
	if ( ncb ) {
		if ( ncb->lb_data_) {
			assert( ncb->lb_length_ > 0 );
			if ( ncb->lb_length_ > 0 ) {
				free( ncb->lb_data_ );
				ncb->lb_data_ = NULL;
			}
			ncb->lb_cpy_offset_ = 0;
		}
		
		ncb->lb_length_ = 0;
	}
}

int ncb_set_rcvtimeo(ncb_t *ncb, struct timeval *timeo){
    if (ncb && timeo > 0){
        return setsockopt(ncb->sockfd, SOL_SOCKET, SO_RCVTIMEO, (const void *)timeo, sizeof(struct timeval));
    }
    return -EINVAL;
}

int ncb_get_rcvtimeo(ncb_t *ncb){
    if (ncb){
         socklen_t optlen =sizeof(ncb->rcvtimeo);
        return getsockopt(ncb->sockfd, SOL_SOCKET, SO_RCVTIMEO, (void *__restrict)&ncb->rcvtimeo, &optlen);
    }
    return -EINVAL;
}

int ncb_set_sndtimeo(ncb_t *ncb, struct timeval *timeo){
    if (ncb && timeo > 0){
        return setsockopt(ncb->sockfd, SOL_SOCKET, SO_SNDTIMEO, (const void *)timeo, sizeof(struct timeval));
    }
    return -EINVAL;
}

int ncb_get_sndtimeo(ncb_t *ncb){
    if (ncb){
        socklen_t optlen =sizeof(ncb->sndtimeo);
        return getsockopt(ncb->sockfd, SOL_SOCKET, SO_SNDTIMEO, (void *__restrict)&ncb->sndtimeo, &optlen);
    }
    return -EINVAL;
}

int ncb_set_iptos(ncb_t *ncb, int tos){
    unsigned char type_of_service = (unsigned char )tos;
    if (ncb && type_of_service){
        return setsockopt(ncb->sockfd, IPPROTO_IP, IP_TOS, (const void *)&type_of_service, sizeof(type_of_service));
    }
    return -EINVAL;
}

int ncb_get_iptos(ncb_t *ncb){
    if (ncb){
        socklen_t optlen =sizeof(ncb->iptos);
        return getsockopt(ncb->sockfd, IPPROTO_IP, IP_TOS, (void *__restrict)&ncb->iptos, &optlen);
    }
    return -EINVAL;
}

int ncb_set_window_size(ncb_t *ncb, int dir, int size){
    if (ncb){
        return setsockopt(ncb->sockfd, SOL_SOCKET, dir, (const void *)&size, sizeof(size));
    }
    
     return -EINVAL;
}

int ncb_get_window_size(ncb_t *ncb, int dir, int *size){
    if (ncb && size){
        socklen_t optlen = sizeof(int);
        if (getsockopt(ncb->sockfd, SOL_SOCKET, dir, (void *__restrict)size, &optlen) < 0){
            return -1;
        }
    }
    
     return -EINVAL;
}

int ncb_set_linger(ncb_t *ncb, int onoff, int lin){
    struct linger lgr;
    
    if (!ncb){
        return -EINVAL;
    }
    
    lgr.l_onoff = onoff;
    lgr.l_linger = lin;
    return setsockopt(ncb->sockfd, SOL_SOCKET, SO_LINGER, (char *) &lgr, sizeof ( struct linger));
}

int ncb_get_linger(ncb_t *ncb, int *onoff, int *lin) {
    struct linger lgr;
    socklen_t optlen = sizeof (lgr);

    if (!ncb) {
        return -EINVAL;
    }

    if (getsockopt(ncb->sockfd, SOL_SOCKET, SO_KEEPALIVE, (void *__restrict) & lgr, &optlen) < 0) {
        return -1;
    }

    if (onoff){
        *onoff = lgr.l_onoff;
    }
    
    if (lin){
        *lin = lgr.l_linger;
    }
    
    return 0;
}

int ncb_set_keepalive(ncb_t *ncb, int enable) {
    if (ncb) {
        return setsockopt(ncb->sockfd, SOL_SOCKET, SO_KEEPALIVE, (const char *) &enable, sizeof ( enable));
    }
    return -EINVAL;
}

int ncb_get_keepalive(ncb_t *ncb, int *enabled){
    if (ncb && enabled) {
        socklen_t optlen = sizeof(int);
        return getsockopt(ncb->sockfd, SOL_SOCKET, SO_KEEPALIVE, (void *__restrict)enabled, &optlen);
    }
    return -EINVAL;
}