#if !defined NSHOST_ECHO_ARGS_H
#define NSHOST_ECHO_ARGS_H

#include "compiler.h"

#define SESS_TYPE_UNKNOWN       (-1)
#define SESS_TYPE_SERVER         ('s')
#define SESS_TYPE_CLIENT         ('c')

extern void display_usage();
extern int check_args(int argc, char **argv);

extern int gettype(); /* application run level in client or server */
extern const char *gethost(); /* get the host IP address which service on or client connect to */
extern uint16_t getport();
extern int getloopstatus(int *threads);

#endif
