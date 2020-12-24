#include "nis.h"
#include "posix_wait.h"
#include "posix_ifos.h"
#include "posix_naos.h"
#include "posix_atomic.h"
#include "logger.h"

#include "args.h"
#include "tst.h"

static const unsigned char NSPDEF_OPCODE[4] = { 'N', 's', 'p', 'd' };

int STDCALL nsp__tst_parser(void *dat, int cb, int *pkt_cb)
{
	nsp__tst_head_t *head = (nsp__tst_head_t *)dat;

	if (!head) return -1;

	if (0 != memcmp(NSPDEF_OPCODE, &head->op_, sizeof(NSPDEF_OPCODE))) {
		return -1;
	}

	*pkt_cb = head->cb_;
	return 0;
}

int STDCALL nsp__tst_builder(void *dat, int cb)
{
	nsp__tst_head_t *head = (nsp__tst_head_t *)dat;

	if (!dat || cb <= 0) {
		return -1;
	}

	memcpy(&head->op_, NSPDEF_OPCODE, sizeof(NSPDEF_OPCODE));
	head->cb_ = cb;
	return 0;
}


int display(HTCPLINK link, const unsigned char *data, int size)
{
	char output[1024];
	uint32_t ip;
	uint16_t port;
	char ipstr[INET_ADDRSTRLEN];
	int offset;

	tcp_getaddr(link, LINK_ADDR_REMOTE, &ip, &port);
	posix__ipv4tos(ip, ipstr, sizeof(ipstr));
	offset = sprintf(output, "[income %s:%u] ", ipstr, port);

	if (size < (sizeof(output) - offset - 1) && size > 0) {
		memcpy(&output[offset], data, size);
		output[offset + size] = 0;
		printf("%s\n", output);
		return 0;
	}

	return -1;
}


void STDCALL nshost_ecr(const char *host_event, const char *reserved, int rescb)
{
	if (host_event) {
		log__save("echo", kLogLevel_Trace, kLogTarget_Filesystem, "%s", host_event);
	}
}


extern int nstest_server_startup();
extern int nstest_client_startup();
int main(int argc, char **argv)
{
	int type;

	if (check_args(argc, argv) < 0) {
		return -1;
	}

	if ((type = gettype()) < 0 ) {
		return 1;
	}

	log__init();
	nis_checr(&nshost_ecr);
	tcp_init();

	if (type == SESS_TYPE_SERVER) {
		return nstest_server_startup();
	}

	if (type == SESS_TYPE_CLIENT) {
		return nstest_client_startup();
	}

	return 0;
}
