#if !defined (NETWORK_CONTROL_BLOCK_HEADER_20120824_1)
#define NETWORK_CONTROL_BLOCK_HEADER_20120824_1

#include "object.h"
#include "clist.h"
#include "nis.h"

#include <ws2ipdef.h>

typedef int( *object_entry_t )( objhld_t h, void * user_buffer, void * ncb_ctx );
typedef void( *object_unload_t )( objhld_t h, void * user_buffer );

typedef struct _NCC_NETWORK_BASIC_CONTROL_BLCOK
{
	objhld_t					link;								
	SOCKET						sockfd;
	enum proto_type_t			proto_type_;			
	union	{											
		udp_io_callback_t		udp_callback_;
		tcp_io_callback_t		tcp_callback_;
	};
	struct {											
		struct sockaddr_in		l_addr_;				// 本地地址信息记录
		struct sockaddr_in		r_addr_;				// 对端地址信息记录
	};

	int							flag_;						// 标记， 目前TCP未使用， UDP可以指定为 UDP_FLAG_BROADCAST
	int							connected_;					// 是否已经建立连接
	IP_MREQ					   *mreq;						// UDP 多播设定
    struct timeval				rcvtimeo;					// 接收超时
    struct timeval				sndtimeo;					// 发送超时

	/* IP头的 tos 项
     * Differentiated Services Field: Dirrerentiated Services Codepoint/Explicit Congestion Not fication 指定TOS段
     *  */
    int iptos;

	int optmask;

	struct {												// 大包解读(大于64KB但是不足50MB的TCP数据包)
		char*					lb_data_;					// large block data
		int						lb_cpy_offset_;				// 当前已经赋值的大包数据段偏移
		int						lb_length_;					// 当前大包缓冲区长度
	};
	struct {											
		struct list_head		tcp_waitting_list_head_;	// TCP等待发送包链
		CRITICAL_SECTION		tcp_lst_lock_;				// TCP的包链锁
		int						tcp_usable_sender_cache_;	// (这个链上的) 下层可用缓冲区字节数
		int						tcp_pending_cnt_;			// (这个链上的) 未决IO请求个数
		tst_t					tcp_tst_;					// TCP(属于这个链的)协议解析模板
        int						mss;						/* MSS of tcp link */
	};
	struct {											
		void *					ncb_ctx_;					// 用户上下文
		int						ncb_ctx_size_;				// 用户上下文长度
	};

}ncb_t;

void ncb_init( ncb_t * ncb, enum proto_type_t proto_type );

#define ncb_set_callback(ncb, fn)		( ncb->tcp_callback_ = ncb->udp_callback_ = ( nis_callback_t )( void * )fn )
extern
void ncb_callback( ncb_t * ncb, const nis_event_t * c_event, const void * c_data );

#define ncb_lb_marked(ncb)	((ncb) ? ((NULL != ncb->lb_data_) && (ncb->lb_length_ > 0)) : (FALSE))
extern
int ncb_mark_lb( ncb_t *ncb, int Size, int CurrentSize, void * SourceData );
extern
void ncb_unmark_lb( ncb_t *ncb );


extern
int ncb_set_rcvtimeo(ncb_t *ncb, struct timeval *timeo);
extern
int ncb_get_rcvtimeo(ncb_t *ncb);
extern
int ncb_set_sndtimeo(ncb_t *ncb, struct timeval *timeo);
extern
int ncb_get_sndtimeo(ncb_t *ncb);

extern
int ncb_set_iptos(ncb_t *ncb, int tos);
extern
int ncb_get_iptos(ncb_t *ncb);

extern
int ncb_set_window_size(ncb_t *ncb, int dir, int size);
extern
int ncb_get_window_size(ncb_t *ncb, int dir, int *size);

extern
int ncb_set_linger(ncb_t *ncb, int onoff, int lin);
extern
int ncb_get_linger(ncb_t *ncb, int *onoff, int *lin);

extern
int ncb_set_keepalive(ncb_t *ncb, int enable);
extern
int ncb_get_keepalive(ncb_t *ncb, int *enabled);

#endif