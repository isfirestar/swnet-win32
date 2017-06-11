#if !defined (IOCP_COMMON_LIBRARY_20120804_1)
#define IOCP_COMMON_LIBRARY_20120804_1

int iocp_init( int io_thread_count );
void iocp_uninit();
int iocp_bind( const UINT_PTR file_handle );
int iocp_thcnts();

#endif