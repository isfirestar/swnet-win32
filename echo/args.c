#include "args.h"

#if _WIN32
#include "getopt.h"
#else
#include <getopt.h>
#endif

#include "posix_string.h"

static struct {
    int type;
    char host[128];
    uint16_t port;
    int echo;
    int mute;
    int interval;
    int threads;
    int length;
} __startup_parameters;

enum ope_index {
    kOptIndex_GetHelp = 'h',
    kOptIndex_GetVersion = 'v',

    kOptIndex_SetPort = 'P',
    kOptIndex_EchoModel = 'e',

    kOptIndex_Server = 's',
    kOptIndex_MuteServer = 'm',

    kOptIndex_Client = 'c',
    kOptIndex_MultiThreadingCount = 't',
    kOptIndex_Interval = 'i',
    kOptIndex_DataLength = 'l',
};

static const struct option long_options[] = {
    {"help", no_argument, NULL, kOptIndex_GetHelp},
    {"version", no_argument, NULL, kOptIndex_GetVersion},
    {"port", required_argument, NULL, kOptIndex_SetPort},
    {"echo", no_argument, NULL, kOptIndex_EchoModel},
    {"server", no_argument, NULL, kOptIndex_Server},
    {"mute", no_argument, NULL, kOptIndex_MuteServer},
    {"client", required_argument, NULL, kOptIndex_Client},
    {"multi-threading", required_argument, NULL, kOptIndex_MultiThreadingCount},
    {"interval", required_argument, NULL, kOptIndex_Interval},
    {"data-length", required_argument, NULL, kOptIndex_DataLength},
    {NULL, 0, NULL, 0}
};

void display_usage()
{
    static const char *usage_context =
            "usage: nstest {-v|--version|-h|--help}\n"
            "\nbelow options are effective both server and client:\n"
            "[-P | --port [port]]\tto specify the connect target in client or change the local listen port in server\n"
            "[-e | --echo]\trun program in echo model\n"
            "\nbelow options are only effective in server model:\n"
            "[-m | --mute]\tonly effective in server model\n"
            "\t\twhen this argument has been specified, server are in silent model, nothing response to client\n"
            "\t\totherwise in default, all packages will completely consistent response to client\n"
            "[-s | --server]\trun program as a server\n"
            "\nbelow options are only effective in client model:\n"
            "[-c | --client [target]]\trun program as a client, and target host must specified\n"
            "[-t | --multi-threading [count]]\tspecify count of threads to send data to server\n"
            "[-i | --interval [milliseconds]]\tspecifies the interval at which the sending thread sends data to server\n"
            "[-l | --data-length [bytes]]\tspecify the size of each packets to send to server\n"
            ;

    printf("%s", usage_context);
}

#define VERSION_STRING "VersionOfProgramDefinition-ChangeInMakefile"
static void display_author_information()
{
    static char author_context[512];
    sprintf(author_context, "nstest\n%s\n"
            "Copyright (C) 2017 Jerry.Anderson\n"
            "For bug reporting instructions, please see:\n"
            "<http://www.nsplibrary.com.cn/>.\n"
            "For help, type \"help\".\n", VERSION_STRING);
    printf("%s", author_context);
}

int check_args(int argc, char **argv)
{
    int opt_index;
    int opt;
    int retval = 0;
    char shortopts[128];

    memset(&__startup_parameters, 0, sizeof(__startup_parameters));
    __startup_parameters.type = SESS_TYPE_SERVER;
    strcpy(__startup_parameters.host, "0.0.0.0");
    __startup_parameters.port = 10256;
    __startup_parameters.echo = 0;
    __startup_parameters.mute = 0;
    __startup_parameters.interval = 100;
    __startup_parameters.threads = 1;
    __startup_parameters.length = 1024;

    /* double '::' meat option may have argument or not,
        one ':' meat option MUST have argument,
        no ':' meat option MUST NOT have argument */
    strcpy(shortopts, "hvP:esmc:t:i:l:");
    opt = getopt_long(argc, argv, shortopts, long_options, &opt_index);
    while (opt != -1) {
        switch (opt) {
            case 'h':
                display_usage();
                return -1;
            case 'v':
                display_author_information();
                return -1;
            case 'P':
                assert(optarg);
                __startup_parameters.port = (uint16_t) strtoul(optarg, NULL, 10);
                break;
            case 'e':
                __startup_parameters.echo = 1;
                break;
            case 's':
                __startup_parameters.type = opt;
                break;
            case 'm':
                __startup_parameters.mute = 1;
                break;
            case 'c':
                assert (optarg);
                strcpy(__startup_parameters.host, optarg);
                __startup_parameters.type = opt;
                break;
            case 't':
                assert(optarg);
                __startup_parameters.threads = atoi(optarg);
                break;
            case 'i':
                assert(optarg);
                __startup_parameters.interval = atoi(optarg);
                break;
            case 'l':
                assert(optarg);
                __startup_parameters.length = atoi(optarg);
                break;
            case '?':
                printf("?\n");
            case 0:
                printf("0\n");
            default:
                display_usage();
                return -1;
        }
        opt = getopt_long(argc, argv, shortopts, long_options, &opt_index);
    }

	if ( __startup_parameters.type == SESS_TYPE_CLIENT && 0 == posix__strcasecmp( __startup_parameters.host, "0.0.0.0" ) ) {
		display_usage();
		return -1;
	}

    return retval;
}

int gettype()
{
    return __startup_parameters.type;
}

void getservercontext(int *echo, int *mute, uint16_t *port)
{
    if (echo) {
        *echo = __startup_parameters.echo;
    }

    if (mute) {
        *mute = __startup_parameters.mute;
    }

    if (port) {
        *port = __startup_parameters.port;
    }
}

void getclientcontext(char **host, uint16_t *port, int *echo, int *interval, int *threads, int *length)
{
    if (host) {
        *host = __startup_parameters.host;
    }

    if (echo) {
        *echo = __startup_parameters.echo;
    }

    if (port) {
        *port = __startup_parameters.port;
    }

    if (interval) {
        *interval = __startup_parameters.interval;
    }

    if (threads) {
        *threads = __startup_parameters.threads;
    }

    if (length) {
        *length = __startup_parameters.length;
    }
}
