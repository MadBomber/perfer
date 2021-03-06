// Copyright 2016 by Peter Ohler, All Rights Reserved

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "dtime.h"
#include "drop.h"
#include "perfer.h"
#include "pool.h"

int
pool_init(Pool p, struct _Perfer *h, long num) {
    p->finished = false;
    p->perfer = h;
    p->num = num;
    p->dcnt = h->ccnt;
    p->max_pending = 0;
    p->sent_cnt = 0;
    p->err_cnt = 0;
    p->ok_cnt = 0;
    p->lat_sum = 0.0;
    p->lat_sq_sum = 0.0;
    p->actual_end = 0.0;
    if (NULL == (p->drops = (Drop)malloc(sizeof(struct _Drop) * p->dcnt))) {
	printf("-*-*- Failed to allocate %d connections.\n", p->dcnt);
	return -1;
    }
    Drop	d;
    int		i;

    for (d = p->drops, i = p->dcnt; 0 < i; i--, d++) {
	drop_init(d, h);
    }
    return 0;
}

void
pool_cleanup(Pool p) {
/*
    Drop	d;
    int		i;

    pthread_join(p->thread, NULL);
    for (d = p->drops, i = p->dcnt; 0 < i; i--, d++) {
	drop_cleanup(d);
    }
    free(p->drops);
*/
}

// Returns addrinfo for a host[:port] string with the default port of 80.
static struct addrinfo*
get_addr_info(const char *addr) {
    struct addrinfo	hints;
    struct addrinfo	*res;
    char		host[1024];
    const char		*port;
    int			err;
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (NULL == (port = strchr(addr, ':'))) {
	err = getaddrinfo(addr, "80", &hints, &res);
    } else if (sizeof(host) <= port - addr - 1) {
	printf("*-*-* Host name too long: %s.\n", addr);
	return NULL;
    } else {
	strncpy(host, addr, port - addr);
	host[port - addr] = '\0';
	port++;
	if (0 != (err = getaddrinfo(host, port, &hints, &res))) {
	    printf("*-*-* Failed to resolve %s. %s\n", addr, gai_strerror(err));
	    return NULL;
	}
    }
    if (0 != err) {
	printf("*-*-* Failed to resolve %s.\n", addr);
	return NULL;
    }
    return res;
}

static void*
loop(void *x) {
    Pool		p = (Pool)x;
    Perfer		h = p->perfer;
    Drop		d;
    int			i;
    int			pcnt = h->ccnt;
    struct pollfd	ps[pcnt];
    struct pollfd	*pp;
    struct addrinfo	*res = get_addr_info(h->addr);
    double		end_time;
    double		now;
    bool		enough = false;
    int			pending;
    
    if (NULL == res) {
	perfer_stop(h);
	return NULL;
    }
    if (h->tcnt - 1 == atomic_fetch_add(&h->ready_cnt, 1)) {
	h->start_time = dtime();
    }
    while (atomic_load(&h->ready_cnt) < h->tcnt) {
	dsleep(0.01);
    }
    end_time = h->start_time + h->duration;
    p->actual_end = end_time;
LOOP:
    while (!h->done) {
	now = dtime();
	if (0 < p->num) {
	    if (p->num <= p->sent_cnt) {
		enough = true;
	    }
	} else if (end_time <= now) {
	    enough = true;
	}
	// If sock is 0 then try to connect and add to pollfd else just add to
	// pollfd.
	for (d = p->drops, i = pcnt, pp = ps; 0 < i; i--, d++) {
	    if (0 == d->sock && !enough) {
		if (drop_connect(d, p, res)) {
		    // Failed to connect. Abort the test.
		    perfer_stop(h);
		    goto LOOP;
		}
	    }
	    if (0 < d->sock) {
		pp->fd = d->sock;
		d->pp = pp;
		pending = drop_pending(d);
		if (0 < pending) {
		    pp->events = POLLERR | POLLIN | POLLOUT;
		} else if (!enough) {
		    pp->events = POLLERR | POLLOUT;
		}
		pp->revents = 0;
		pp++;
	    }
	}
	if (pp == ps) {
	    break;
	}
	if (0 > (i = poll(ps, pp - ps, 10))) {
	    if (EAGAIN == errno) {
		continue;
	    }
	    printf("*-*-* polling error: %s\n", strerror(errno));
	    break;
	}
	if (0 == i) {
	    continue;
	}
	for (d = p->drops, i = pcnt; 0 < i; i--, d++) {
	    if (NULL == d->pp || 0 == d->pp->revents || 0 == d->sock) {
		continue;
	    }
	    if (0 != (d->pp->revents & POLLIN)) {
		if (drop_recv(d, p, enough)) {
		    continue;
		}
	    }
	    if (!enough && 0 != (d->pp->revents & POLLOUT)) {
		if (drop_send(d, p)) {
		    continue;
		}
	    }
	    if (0 != (d->pp->revents & POLLERR)) {
		p->err_cnt++;
		drop_cleanup(d);
	    }
	}
    }
    p->actual_end = dtime();
    int	cnt = 0;
    for (d = p->drops, i = pcnt; 0 < i; i--, d++) {
	if (0 < d->sock) {
	    cnt++;
	}
    }
    freeaddrinfo(res);
    p->finished = true;

    return NULL;
}

int
pool_start(Pool p) {
    return pthread_create(&p->thread, NULL, loop, p);
}

void
pool_wait(Pool p) {
    // Wait for thread to finish. Join is not used as we want to kill the thread
    // if it does not exit correctly.
    pthread_detach(p->thread); // cleanup thread resources when completed
    if (!p->finished) {
	double  late = dtime() + p->perfer->duration + 2.0;

	while (!p->finished && dtime() < late) {
	    dsleep(0.5);
        }
	pthread_cancel(p->thread);
    }
}
