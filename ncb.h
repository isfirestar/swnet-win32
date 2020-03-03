#if !defined (NETWORK_CONTROL_BLOCK_HEADER_20120824_1)
#define NETWORK_CONTROL_BLOCK_HEADER_20120824_1

#include "object.h"
#include "clist.h"
#include "nis.h"

#include <ntstatus.h>

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

	/* tos item in IP layer:
     * Differentiated Services Field: Dirrerentiated Services Codepoint/Explicit Congestion Not fication 
     *  */
    int iptos;

	int optmask;

	struct {												/* large block(size in range (64KB, 50MB]) */
		char*					lb_data_;					/* large block data memory */
		int						lb_cpy_offset_;				/* the data offset which has been copied into large packet buffer */
		int						lb_length_;					/* current length of large packet in bytes */
	};
	struct {											
		struct list_head		tcp_sender_cache_head_;		/* TCP sender control and traffic manager */
		int						tcp_sender_cached_count_;	/* the count of packet pending in @tcp_sender_cache_head_ */
		CRITICAL_SECTION		tcp_sender_locker_;			/* lock element: @tcp_sender_cache_head_ and @tcp_sender_cached_count_*/
		int						tcp_sender_pending_count_;	/* the total pending count on this link */
		tst_t					tcp_tst_;					/* protocol template on this link */
        int						mss;						/* MSS of tcp link */
	};
	struct {											
		void *					ncb_ctx_;					/* user context set on this link */
		int						ncb_ctx_size_;				/* size of @ncb_ctx_ in bytes */
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