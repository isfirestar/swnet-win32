#include "nis.h"
#include "posix_wait.h"
#include "posix_ifos.h"
#include "posix_naos.h"
#include "posix_atomic.h"
#include "posix_thread.h"
#include "logger.h"

#include "args.h"

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

void tcp_server_callback(const struct nis_event *event, const void *data)
{
	struct nis_tcp_data *tcpdata;
	HTCPLINK link;

	tcpdata = (struct nis_tcp_data *)data;
	link = event->Ln.Tcp.Link;
	switch(event->Event) {
		case EVT_RECEIVEDATA:
			//if (display(link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size) >= 0 ) {
			//	if (tcp_write(link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size, NULL) < 0) {
			//		tcp_destroy(link);
			//	}
			//} else {
			//	tcp_destroy(link);
			//}
			if (tcp_write(link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size, NULL) < 0) {
				tcp_destroy(link);
			}
			break;
		case EVT_TCP_ACCEPTED:
		case EVT_CLOSED:
		default:
			break;
	}
}

void tcp_client_callback(const struct nis_event *event, const void *data)
{
	struct nis_tcp_data *tcpdata = (struct nis_tcp_data *)data;

	switch(event->Event) {
		case EVT_RECEIVEDATA:
			if (!getloopstatus(NULL)) {
				display(event->Ln.Tcp.Link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size);
				printf("input:$ ");
			} else {
				tcp_write(event->Ln.Tcp.Link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size, NULL);
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

void nshost_ecr(const char *host_event, const char *reserved, int rescb)
{
	if (host_event) {
		ECHO("echo", "%s", host_event);
	}
}

int echo_server_startup()
{
	HTCPLINK server;

	server = tcp_create(&tcp_server_callback, gethost(), getport());
	if (INVALID_HTCPLINK == server) {
		return 1;
	}

	tcp_listen(server, 100);
	posix__hang();
	return 0;
}

int echo_client_startup()
{
	HTCPLINK *clients;
	char text[65535], *p;
	size_t n;
	int threads;
	int i;
	int loop;
	static const int MAX_LOOP_MESG = 2048;

	if (0 == (loop = getloopstatus(&threads)) ) {
		threads = 1;
	}

	clients = (HTCPLINK *)malloc(threads * sizeof(HTCPLINK));
	assert(clients);

	for (i = 0; i < threads; i++) {
		clients[i] = tcp_create(&tcp_client_callback, NULL, 0);
		if (INVALID_HTCPLINK != clients[i]) {
			if (tcp_connect(clients[i], gethost(), getport()) < 0) {
				tcp_destroy(clients[i]);
			} else {
				if (loop) {
					memset(text, 0, sizeof(text));
					tcp_write(clients[i], text, MAX_LOOP_MESG, NULL);
				}
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
	tcp_init();
	nis_checr(&nshost_ecr);

	if (type == SESS_TYPE_SERVER) {
		return echo_server_startup();
	}

	if (type == SESS_TYPE_CLIENT) {
		return echo_client_startup();
	}

	return 0;
}
