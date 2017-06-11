#include "network.h"
#include "ncb.h"

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
		ncb->sock_ = INVALID_SOCKET;
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
		ncb_report_debug_information( ncb, "fail to allocate memory for ncb->lb_data_, request size=%u", cb );
		return -1;
	}
	memcpy( ncb->lb_data_, source, current_size );
	ncb->lb_cpy_offset_ = current_size;
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

void ncb_report_debug_information(ncb_t *ncb, const char *fmt,...) {
    udp_data_t c_data;
    nis_event_t c_event;
    char logstr[128];
    va_list ap;
    int retval;
    
    if (!ncb || !fmt) {
        return;
    }

    c_event.Ln.Udp.Link = ncb->h_;
    c_event.Event = EVT_DEBUG_LOG;
   
    va_start(ap, fmt);
    retval = vsprintf_s(logstr, cchof(logstr), fmt, ap);
    va_end(ap);
    
    if (retval <= 0) {
        return;
    }
    logstr[retval] = 0;
    
    c_data.e.DebugLog.logstr = &logstr[0];
    if (ncb->tcp_callback_) {
        ncb->tcp_callback_(&c_event, &c_data);
    }
}