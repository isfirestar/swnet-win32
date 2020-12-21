#if !defined NSPTST_HEAD
#define NSPTST_HEAD

#include "compiler.h"

#include <string.h>

#pragma pack(push, 1)

typedef struct {
	uint32_t op_;
	uint32_t cb_;
} nsp__tst_head_t;

#pragma pack(pop)

const unsigned char NSPDEF_OPCODE[4] = { 'N', 's', 'p', 'd' };

#if _WIN32
__forceinline int STDCALL nsp__tst_parser(void *dat, int cb, int *pkt_cb)
#else
static inline int STDCALL nsp__tst_parser(void *dat, int cb, int *pkt_cb)
#endif
{
	nsp__tst_head_t *head = (nsp__tst_head_t *)dat;

	if (!head) return -1;

	if (0 != memcmp(NSPDEF_OPCODE, &head->op_, sizeof(NSPDEF_OPCODE))) {
		return -1;
	}

	*pkt_cb = head->cb_;
	return 0;
}

#if _WIN32
__forceinline int STDCALL nsp__tst_builder(void *dat, int cb)
#else
static inline int STDCALL nsp__tst_builder(void *dat, int cb)
#endif
{
	nsp__tst_head_t *head = (nsp__tst_head_t *)dat;

	if (!dat || cb <= 0) {
		return -1;
	}

	memcpy(&head->op_, NSPDEF_OPCODE, sizeof(NSPDEF_OPCODE));
	head->cb_ = cb;
	return 0;
}

#endif
