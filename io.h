#if !defined (IOCP_COMMON_LIBRARY_20120804_1)
#define IOCP_COMMON_LIBRARY_20120804_1

extern
int ioinit();
extern
void iouninit();
extern
int ioatth(void *ncbptr);
extern
void ioclose(void *ncbptr);

#endif
