#if !defined (NETWORK_BASICE_PACKET_HEADER_20120824_1)
#define NETWORK_BASICE_PACKET_HEADER_20120824_1

#include "object.h"

#if !defined USES_LOCAL_ROUTE_TABLE
#define USES_LOCAL_ROUTE_TABLE									(1)		// ����, ��������·�ɱ�
#endif

#define MAXIMUM_TRANSMIT_BYTES_PER_GROUP						(0x7FFFFFFE)				// �����鷢���������ֽ���
#define MAXIMUM_GPM_ITEM_COUNT									(32)						// GPM��ʽ��󵥰�����

enum pkt_type_t {
	kSend = 1,
	kRecv,
	kSyn,
	kConnect,
	kUnknown,
};

enum page_style_t {
	kNoAccess = 0,
	kVirtualHeap,
	kNonPagedPool,
};

typedef struct _NCC_PACKET {
	OVERLAPPED overlapped_;
	enum pkt_type_t type_;		// �����հ�����
	enum proto_type_t proto_type_;// Э������
	enum page_style_t page_style_;				// ������ҳ����
	uint32_t flag_;				// �����첽�����е�flag
	int from_length_;				// ����fromlen
	objhld_t link;					// ���ƿ�ľ��
	objhld_t accepted_link;			// ���� tcp accept �ĶԶ˶�����
	union {							
		struct {						// �� UDP ���� �洢����Ŀ���ַ
			struct sockaddr_in r_addr_;
			struct sockaddr_in l_addr_;
		};
		struct list_head pkt_lst_entry_;	// �� TCP ���Ͷ���� ncb_t::tcp_waitting_list_head_ ����(ÿ��������һ���ڵ�)
	};
	int size_for_req_;				// Ͷ������ǰ�ģ� ����������
	int size_for_translation_;		// �����ֽ���
	int size_completion_;			// Ͷ�ݸ�ϵͳ���ڽ�����ɳ��ȵ��ֶΣ� ������ size_for_translation_, ���ֶβ�������ʹ��
	int analyzed_offset_;			// ���� TCP ��������е�ԭʼ��ַƫ�ƽ���
	void *ori_buffer_;				// ԭʼ����ָ�룬 ��Ϊirp_�ֶο�����ΪͶ�ݸ�ϵͳ��ָ���ƶ����仯�� ���ԭʼ��ַ��Ҫ��¼
	union {							
		struct {
			PTRANSMIT_PACKETS_ELEMENT grp_packets_; // ��ʹ�� grp ��ʽ���з��Ͳ����� �򻺳���λ�ڴ�������, ����packetģ�鲻������Щ�����ڴ�Ĺ�����
			int grp_packets_cnt_;
		};
		void *irp_;			// �û�����ָ��, ʵ�ʵ�IRP�ڴ��ַ
	};
}packet_t;

int allocate_packet( objhld_t h, enum proto_type_t proto_type, enum pkt_type_t type, int cbSize, enum page_style_t page_style, packet_t **output_packet );
void freepkt( packet_t * packet );

int asio_udp_recv( packet_t * packet );
int syio_udp_send( packet_t * packet, const char *r_ipstr, uint16_t r_port );

int syio_v_connect( ncb_t * ncb, const struct sockaddr_in *r_addr );
int syio_v_disconnect( ncb_t * ncb );
int syio_grp_send( packet_t * packet );

int asio_tcp_send( packet_t * packet );
int asio_tcp_accept( packet_t * packet );
int asio_tcp_recv( packet_t * packet );
int asio_tcp_connect(packet_t *packet);

#endif