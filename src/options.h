
#ifndef OPTIONS_H
#define OPTIONS_H

typedef struct {
  char *port;          /* port where the server listens */
  int socket_backlog;  /* number of clients allowed to queue up */
  int threads;         /* number of threads. Set to 0 for automatic */
} Options;

#endif
