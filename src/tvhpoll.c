/*
 *  TVheadend - poll/select wrapper
 *
 *  Copyright (C) 2013 Adam Sutton
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tvheadend.h"
#include "tvhpoll.h"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#if ENABLE_EPOLL
#include <sys/epoll.h>
#elif ENABLE_KQUEUE
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#endif

#if ENABLE_EPOLL
#define EV_SIZE sizeof(struct epoll_event)
#elif ENABLE_KQUEUE
#define EV_SIZE sizeof(struct kevent)
#endif

struct tvhpoll
{
#if ENABLE_EPOLL
  int fd;
  struct epoll_event *ev;
  int nev;
#elif ENABLE_KQUEUE
  int fd;
  struct kevent *ev;
  int nev;
#else
#endif
};

static void
tvhpoll_alloc ( tvhpoll_t *tp, int n )
{
#if ENABLE_EPOLL || ENABLE_KQUEUE
  if (n > tp->nev) {
    tp->ev  = realloc(tp->ev, n * EV_SIZE);
    tp->nev = n;
  }
#else
#endif
}

tvhpoll_t *
tvhpoll_create ( size_t n )
{
  int fd;
#if ENABLE_EPOLL
  if ((fd = epoll_create(n)) < 0) {
    tvhlog(LOG_ERR, "tvhpoll", "failed to create epoll [%s]",
           strerror(errno));
    return NULL;
  }
#elif ENABLE_KQUEUE
  if ((fd = kqueue()) < 0) {
    tvhlog(LOG_ERR, "tvhpoll", "failed to create kqueue [%s]",
           strerror(errno));
    return NULL;
  }
#else
#endif
  tvhpoll_t *tp = calloc(1, sizeof(tvhpoll_t));
  tp->fd = fd;
  tvhpoll_alloc(tp, n);
  return tp;
}

void tvhpoll_destroy ( tvhpoll_t *tp )
{
#if ENABLE_EPOLL || ENABLE_KQUEUE
  free(tp->ev);
  close(tp->fd);
#endif
  free(tp);
}

int tvhpoll_add
  ( tvhpoll_t *tp, tvhpoll_event_t *evs, size_t num )
{
#if ENABLE_EPOLL
  int i;
  struct epoll_event ev;
  for (i = 0; i < num; i++) {
    memset(&ev, 0, sizeof(ev));
    ev.data.u64 = evs[i].data.u64;
    if (evs[i].events & TVHPOLL_IN)  ev.events |= EPOLLIN;
    if (evs[i].events & TVHPOLL_OUT) ev.events |= EPOLLOUT;
    if (evs[i].events & TVHPOLL_PRI) ev.events |= EPOLLPRI;
    if (evs[i].events & TVHPOLL_ERR) ev.events |= EPOLLERR;
    if (evs[i].events & TVHPOLL_HUP) ev.events |= EPOLLHUP;
    if (epoll_ctl(tp->fd, EPOLL_CTL_ADD, evs[i].fd, &ev) != 0)
      return -1;
  }
  return 0;
#elif ENABLE_KQUEUE
  int i;
  uint32_t fflags;
  tvhpoll_alloc(tp, num);
  for (i = 0; i < num; i++) {
    fflags = 0;
    if (evs[i].events & TVHPOLL_OUT) fflags |= EVFILT_WRITE;
    if (evs[i].events & TVHPOLL_IN)  fflags |= EVFILT_READ;
    EV_SET(tp->ev+i, evs[i].fd, fflags, EV_ADD, 0, 0, (void*)evs[i].data.u64);
  }
  return kevent(tp->fd, tp->ev, num, NULL, 0, NULL);
#else
#endif
}

int tvhpoll_rem
  ( tvhpoll_t *tp, tvhpoll_event_t *evs, size_t num )
{
  tvhpoll_alloc(tp, num);
#if ENABLE_EPOLL
  int i;
  for (i = 0; i < num; i++)
    epoll_ctl(tp->fd, EPOLL_CTL_DEL, evs[i].fd, NULL);
#elif ENABLE_KQUEUE
  int i;
  for (i = 0; i < num; i++)
    EV_SET(tp->ev+i, evs[i].fd, 0, EV_DELETE, 0, 0, NULL);
  kevent(tp->fd, tp->ev, num, NULL, 0, NULL);
#else
#endif
  return 0;
}

int tvhpoll_wait
  ( tvhpoll_t *tp, tvhpoll_event_t *evs, size_t num, int ms )
{
  int nfds = 0, i;
  tvhpoll_alloc(tp, num);
#if ENABLE_EPOLL
  nfds = epoll_wait(tp->fd, tp->ev, num, ms);
  for (i = 0; i < nfds; i++) {
    evs[i].data.u64 = tp->ev[i].data.u64;
    evs[i].events   = 0;
    if (tp->ev[i].events & EPOLLIN)  evs[i].events |= TVHPOLL_IN;
    if (tp->ev[i].events & EPOLLOUT) evs[i].events |= TVHPOLL_OUT;
    if (tp->ev[i].events & EPOLLERR) evs[i].events |= TVHPOLL_ERR;
    if (tp->ev[i].events & EPOLLPRI) evs[i].events |= TVHPOLL_PRI;
    if (tp->ev[i].events & EPOLLHUP) evs[i].events |= TVHPOLL_HUP;
  }
#elif ENABLE_KQUEUE
  struct timespec tm, *to = NULL;
  if (ms) {
    tm.tv_sec  = ms / 1000;
    tm.tv_nsec = (ms % 1000) * 1000000LL;
    to = &tm;
  }
  nfds = kevent(tp->fd, NULL, 0, tp->ev, num, to);
  for (i = 0; i < nfds; i++) {
    evs[i].fd       = tp->ev[i].ident;
    evs[i].events   = 0;
    evs[i].data.u64 = (uint64_t)tp->ev[i].udata;
    if (tp->ev[i].fflags & EVFILT_WRITE) evs[i].events |= TVHPOLL_OUT;
    if (tp->ev[i].fflags & EVFILT_READ)  evs[i].events |= TVHPOLL_IN;
    if (tp->ev[i].flags  & EV_EOF)       evs[i].events |= TVHPOLL_HUP;
  }
#else
#endif
  return nfds;
}
