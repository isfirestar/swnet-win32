#if !defined NSHOST_ECHO_ARGS_H
#define NSHOST_ECHO_ARGS_H

#include "compiler.h"

#define SESS_TYPE_UNKNOWN       (-1)
#define SESS_TYPE_SERVER         ('s')
#define SESS_TYPE_CLIENT         ('c')

extern void display_usage();
extern int check_args(int argc, char **argv);

extern int gettype(); /* application run level in client or server */
extern void getservercontext(int *echo, int *mute, uint16_t *port);
extern void getclientcontext(char **host, uint16_t *port, int *echo, int *interval, int *threads, int *length);

#endif
