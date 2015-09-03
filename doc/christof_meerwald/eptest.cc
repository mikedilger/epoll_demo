#include <stdio.h>
#include <stdlib.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <pthread.h>


#define RETRY_EINTR_RC(rc, expr) \
    { \
        do \
        { \
            rc = (expr); \
        } while ((rc < 0) && (errno == EINTR)); \
    }


namespace
{
  pthread_mutex_t outstanding_sync = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t outstanding_cond = PTHREAD_COND_INITIALIZER;
  int nr_outstanding = 0;


  void print_duration(struct timeval const &start)
  {
    struct timeval end;
    gettimeofday(&end, NULL);

    end.tv_sec -= start.tv_sec;
    if (end.tv_usec < start.tv_usec)
    {
      end.tv_usec += 1000000;
      --end.tv_sec;
    }
    end.tv_usec -= start.tv_usec;

    printf("duration %ld.%06ld s\n", end.tv_sec, end.tv_usec);
  }

  void print_rusage()
  {
    struct rusage usage;
    if (!getrusage(RUSAGE_SELF, &usage))
    {
      printf("vol/invol cs %ld/%ld\n", usage.ru_nvcsw, usage.ru_nivcsw);
    }
  }

  int set_socket_nonblocking(int socket)
  {
    int rc_nonblock;
    int flags = ::fcntl(socket, F_GETFL, 0);
    if (flags >= 0)
    {
      rc_nonblock = ::fcntl(socket, F_SETFL, flags | O_NONBLOCK);
    }
    else
    {
      rc_nonblock = flags;
    }

    return rc_nonblock;
  }

  struct Thread_Data
  {
    int epollfd;
    int nr_iter;
  };

  extern "C" void *worker(void *arg)
  {
    Thread_Data *data = static_cast<Thread_Data *>(arg);
    const int epollfd = data->epollfd;
    const int nr_iter = data->nr_iter;

    union
    {
      char buf[4096];
      int seq_nr;
    };

    struct epoll_event events[16];
    bool done = false;

    while (!done)
    {
      int rc_epoll;
      RETRY_EINTR_RC(rc_epoll,
		     epoll_wait(epollfd, events, sizeof(events) / sizeof(*events), -1));
      if (rc_epoll < 0) break;

      for (int i = 0; i < rc_epoll; ++i)
      {
	if (events[i].events & EPOLLHUP)
	{
	  done = true;
	  break;
	}

	const int s = events[i].data.fd;

	while (true)
	{
	  int rc_recv;
	  RETRY_EINTR_RC(rc_recv, recv(s, buf, sizeof(buf), 0));
	  if ((rc_recv < 0) && ((errno == EWOULDBLOCK) || (errno == EAGAIN)))
	  {
	    break;
	  }

	  if (++seq_nr < nr_iter)
	  {
	    int rc_send;
	    RETRY_EINTR_RC(rc_send, send(s, buf, sizeof(int), 0));
	  }
	  else if (seq_nr == nr_iter)
	  {
	    pthread_mutex_lock(&outstanding_sync);
	    if (!--nr_outstanding)
	    {
	      pthread_cond_broadcast(&outstanding_cond);
	    }
	    pthread_mutex_unlock(&outstanding_sync);
	  }
	}
      }
    }

    return NULL;
  }
}


int main(int argc, const char * const argv[])
{
  const int nr_threads = (argc > 1) ? atoi(argv[1]) : 1;
  const int nr_iter = (argc > 2) ? atoi(argv[2]) : 1;
  const int nr_pairs = (argc > 3) ? atoi(argv[3]) : 1;

  const int epollfd(epoll_create(1024));

  int syncsockets[2];
  socketpair(AF_UNIX, SOCK_STREAM, 0, syncsockets);
  {
    struct epoll_event event = {
      EPOLLIN, 0
    };
    event.data.fd = syncsockets[1];
    epoll_ctl(epollfd, EPOLL_CTL_ADD, syncsockets[1], &event);
  }

  int *sockets = new int[2 * nr_pairs];
  for (int i = 0; i < nr_pairs; ++i)
  {
    socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets + 2*i);
    set_socket_nonblocking(sockets[2*i]);
    set_socket_nonblocking(sockets[2*i + 1]);

    struct epoll_event event = {
      EPOLLIN | EPOLLET, 0
    };

    event.data.fd = sockets[2*i];
    epoll_ctl(epollfd, EPOLL_CTL_ADD, sockets[2*i], &event);

    event.data.fd = sockets[2*i + 1];
    epoll_ctl(epollfd, EPOLL_CTL_ADD, sockets[2*i + 1], &event);

    ++nr_outstanding;
  }

  pthread_t *threads = new pthread_t[nr_threads];
  Thread_Data thread_arg = { epollfd, nr_iter };
  for (int i = 0; i < nr_threads; ++i)
  {
    pthread_create(threads + i, NULL, worker, &thread_arg);
  }

  struct timeval start;
  gettimeofday(&start, NULL);

  int buf = 0;
  for (int i = 0; i < nr_pairs; ++i)
  {
    send(sockets[2*i], &buf, sizeof(buf), 0);
  }

  pthread_mutex_lock(&outstanding_sync);
  while (nr_outstanding)
  {
    pthread_cond_wait(&outstanding_cond, &outstanding_sync);
  }
  pthread_mutex_unlock(&outstanding_sync);

  print_duration(start);
  print_rusage();

  close(syncsockets[0]);

  for (int i = 0; i < nr_threads; ++i)
  {
    pthread_join(threads[i], NULL);
  }

  for (int i = 0; i < nr_pairs; ++i)
  {
    close(sockets[2*i]);
    close(sockets[2*i + 1]);
  }
  close(syncsockets[1]);
  close(epollfd);

  delete [] threads;
  delete [] sockets;

  return 0;
}
