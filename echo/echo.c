#include "nis.h"
#include "posix_wait.h"
#include "posix_ifos.h"
#include "posix_naos.h"
#include "posix_atomic.h"
#include "posix_thread.h"
#include "logger.h"

#include "args.h"
#include "tst.h"

#define MAX_LOOP_MESG 2048

int display(HTCPLINK link, const unsigned char *data, int size)
{
	char output[1024];
	uint32_t ip;
	uint16_t port;
	char ipstr[INET_ADDRSTRLEN];
	int offset;

	memset(output, 0, sizeof(output));

	tcp_getaddr(link, LINK_ADDR_REMOTE, &ip, &port);
	posix__ipv4tos(ip, ipstr, sizeof(ipstr));
	offset = sprintf(output, "[income %s:%u] ", ipstr, port);

	if (size < (int)(sizeof(output) - offset) && size > 0) {
		memcpy(&output[offset], data, size);
		printf("%s\n", output);
		return 0;
	}

	return -1;
}

void on_server_receive_data(HTCPLINK link, const unsigned char *data, int size)
{
	do {
		if (size <= 1024) {
			if (display(link, data, size) <= 0) {
				break;
			}
		}

		if (tcp_write(link, data, size, NULL) < 0) {
			//break;
		}

		return;
	} while (0);

	tcp_destroy(link);
}

void STDCALL tcp_server_callback(const struct nis_event *event, const void *data)
{
	struct nis_tcp_data *tcpdata;
	HTCPLINK link;

	tcpdata = (struct nis_tcp_data *)data;
	link = event->Ln.Tcp.Link;
	switch (event->Event) {
	case EVT_RECEIVEDATA:
		on_server_receive_data(link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size);
		break;
	case EVT_TCP_ACCEPTED:
	case EVT_CLOSED:
	default:
		break;
	}
}

void STDCALL tcp_client_callback(const struct nis_event *event, const void *data)
{
	struct nis_tcp_data *tcpdata = (struct nis_tcp_data *)data;

	switch(event->Event) {
		case EVT_RECEIVEDATA:
			if (!getloopstatus(NULL)) {
				display(event->Ln.Tcp.Link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size);
				printf("input:$ ");
			} else {
				//tcp_write(event->Ln.Tcp.Link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size, NULL);
			}
			break;
		case EVT_TCP_CONNECTED:
			if (!getloopstatus(NULL)) {
				printf("input:$ ");
			}
			break;
		case EVT_TCP_ACCEPTED:
		case EVT_CLOSED:
		default:
			break;
	}
}

void STDCALL nshost_ecr(const char *host_event, const char *reserved, int rescb)
{
	if (host_event) {
		ECHO("echo", "%s", host_event);
	}
}

int echo_server_startup()
{
	HTCPLINK server;
	tst_t tst;
	int attr;

	server = tcp_create(&tcp_server_callback, gethost(), getport());
	if (INVALID_HTCPLINK == server) {
		return 1;
	}

	tst.parser_ = &nsp__tst_parser;
	tst.builder_ = &nsp__tst_builder;
	tst.cb_ = sizeof(nsp__tst_head_t);
	nis_cntl(server, NI_SETTST, &tst);

	attr = nis_cntl(server, NI_GETATTR);
	if (attr >= 0) {
		attr |= LINKATTR_TCP_UPDATE_ACCEPT_CONTEXT;
		attr = nis_cntl(server, NI_SETATTR, attr);
	}

	tcp_listen(server, 100);
	posix__hang();
	return 0;
}

void *start_routine(void *p)
{
	HTCPLINK client;
	char text[65535];

	client = *(HTCPLINK *)p;

	ECHO("echo", "independence WLP:%d", GetCurrentThreadId());

	memset(text, 0, sizeof(text));
	while (tcp_write(client, text, MAX_LOOP_MESG, NULL) >= 0) {
		tcp_write(client, text, MAX_LOOP_MESG, NULL);
		posix__delay_execution(100 * 1000);
	}
	return NULL;
}

int echo_client_startup()
{
	HTCPLINK *clients;
	char text[65535], *p;
	size_t n;
	int threads;
	int i;
	int loop;
	posix__pthread_t *tids;
	tst_t tst;

	if (0 == (loop = getloopstatus(&threads)) ) {
		threads = 1;
	}

	clients = (HTCPLINK *)malloc(threads * sizeof(HTCPLINK));
	assert(clients);
	tids = (posix__pthread_t *)malloc(threads * sizeof(posix__pthread_t));
	assert(tids);

	tst.parser_ = &nsp__tst_parser;
	tst.builder_ = &nsp__tst_builder;
	tst.cb_ = sizeof(nsp__tst_head_t);

	for (i = 0; i < threads; i++) {
		clients[i] = tcp_create(&tcp_client_callback, NULL, 0);
		if (INVALID_HTCPLINK == clients[i]) {
			continue;
		}

		nis_cntl(clients[i], NI_SETTST, &tst);
		if (tcp_connect(clients[i], gethost(), getport()) < 0) {
			tcp_destroy(clients[i]);
			continue;
		}

		if (loop) {
			if ( getindependence() ) {
				posix__pthread_create(&tids[i], &start_routine, &clients[i]);
			} else {
				memset(text, 0, sizeof(text));
				tcp_write(clients[i], text, MAX_LOOP_MESG, NULL);
			}
		}
	}

	if (!loop) {
		assert(1 == threads);
		while (NULL != (p = fgets(text, sizeof(text), stdin))) {
			n = strlen(text);
			if (n > 0) {
				if (tcp_write(clients[0], text, n, NULL) < 0) {
					break;
				}
			}
		}
	} else {
		posix__hang();
	}

	return 1;
}

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
		return echo_server_startup();
	}

	if (type == SESS_TYPE_CLIENT) {
		return echo_client_startup();
	}

	return 0;
}
