// Copyright 2016 by Peter Ohler, All Rights Reserved

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arg.h"
#include "drop.h"
#include "pool.h"
#include "perfer.h"

#ifndef OSX_OS
// this is gnu
extern int asprintf(char **strp, const char *fmt, ...);
#endif

#define VERSION	"1.3.0"

static struct _Perfer	perfer = {
    .inited = false,
    .done = false,
    .pools = NULL,
    .addr = NULL,
    .path = "index.html",
    .tcnt = 1,
    .ccnt = 1,
    .duration = 1.0,
    .start_time = 0.0,
    .ready_cnt = 0,
    .seq = 0,
    .req_file = NULL,
    .req_body = NULL,
    .req_len = 0,
    .backlog = PIPELINE_SIZE - 1,
    .keep_alive = false,
    .verbose = false,
    .replace = false,
    .headers = NULL,
};

static const char	*help_lines[] = {
    "saturate a web server with HTTP requests while tracking the number of requests,",
    "latency, and throughput. After the duration specified no more requests are sent",
    "and the application terminates when all responses have been received or 10",
    "seconds, which ever is sooner.",
    "",
    "  -h                      This help page.",
    "  --help",
    "",
    "  -v                      Verbose mode.",
    "",
    "  --version               Version of this application.",
    "",
    "  -d <duration>           Duration in seconds for the run. Positive decimal",
    "  --duration <duration>   values are accepted.",
    "",
    "  -n <count>              Number of requests to send. The value will be",
    "  --number <count>        rounded to the lowest multiple of the number of",
    "                          threads.",
    "",
    "  -t <number>             Number of threads to use for sending requests.",
    "  --threads <number>      (default: 1)",
    "",
    "  -c <number>             Number of connection to use for sending requests",
    "  --connections <number>  for each thread (default: 1)",
    "",
    "  -b <number>             Maximum backlog for pipeline on a connection.",
    "  --backlog <number>      (default: 15)",
    "",
    "  -p <path>               URL path of the HTTP request. (default: /)",
    "  --path <path>",
    "",
    "  -r <file>               File with the full content of the HTTP request.",
    "  --request <file>        The -keep-alive option is ignored.",
    "",
    "  -k                      Keep connections alive instead of closing.",
    "  --keep-alive",
    "",
    "  -a <name: value>        Add an HTTP header field with name and value.",
    "  --add <name: value>",
    "",
    "  <server-address>        IP address of the server to send requests to.",
    ""
};

static void
help(const char *app_name) {
    printf("\nusage: %s [options] <server-address>\n\n", app_name);
    for (const char **lp = help_lines; NULL != *lp; lp++) {
	printf("%s\n", *lp);
    }
}

static void
build_req(Perfer p) {
    int		size = snprintf(NULL, 0, "GET /%s HTTP/1.1\r\nHost: %s\r\nConnection: Keep-Alive\r\n\r\n", p->path, p->addr);
    char	*end;

    for (Header h = p->headers; NULL != h; h = h->next) {
	size += strlen(h->line) + 2;
    }
    p->req_body = (char*)malloc(size + 1);
    p->req_len = size;
    end = p->req_body;
    if (p->keep_alive) {
	end += sprintf(end, "GET /%s HTTP/1.1\r\nHost: %s\r\nConnection: Keep-Alive\r\n", p->path, p->addr);
    } else {
	end += sprintf(end, "GET /%s HTTP/1.1\r\nHost: %s\r\nConnection: Close\r\n", p->path, p->addr);
    }
    for (Header h = p->headers; NULL != h; h = h->next) {
	end = stpcpy(end, h->line);
	*end++ = '\r';
	*end++ = '\n';
    }
    *end++ = '\r';
    *end++ = '\n';
    p->req_body[size] = '\0';
}

static int
perfer_init(Perfer p, int argc, const char **argv) {
    const char	*app_name = *argv;
    const char	*opt_val = NULL;
    char	*end;
    int		cnt;
    long	num = 0;
    
    pthread_mutex_init(&p->print_mutex, 0);
    argv++;
    argc--;
    for (; 0 < argc; argc -= cnt, argv += cnt) {
	if (0 != (cnt = arg_match(argc, argv, &opt_val, "h", "-help"))) {
	    help(app_name);
	    return 1;
	}
	switch (cnt = arg_match(argc, argv, NULL, "-version", "-version")) {
	case 0: // no match
	    break;
	case 1:
	case 2:
	    printf("perfer version %s\n", VERSION);
	    return 1;
	    break;
	default: // match but something went wrong
	    help(app_name);
	    return -1;
	}
	switch (cnt = arg_match(argc, argv, NULL, "v", "-verbose")) {
	case 0: // no match
	    break;
	case 1:
	    p->verbose = true;
	    continue;
	    break;
	default: // match but something went wrong
	    help(app_name);
	    return -1;
	}
	switch (cnt = arg_match(argc, argv, &opt_val, "d", "-duration")) {
	case 0: // no match
	    break;
	case 1:
	case 2:
	    p->duration = strtod(opt_val, &end);
	    if ('\0' != *end || 0.0 > p->duration) {
		printf("'%s' is not a valid duration.\n", opt_val);
		help(app_name);
		return -1;
	    }
	    continue;
	    break;
	default: // match but something went wrong
	    help(app_name);
	    return -1;
	}
	switch (cnt = arg_match(argc, argv, &opt_val, "n", "-number")) {
	case 0: // no match
	    break;
	case 1:
	case 2:
	    num = strtol(opt_val, &end, 10);
	    if ('\0' != *end || 0 >= num) {
		printf("'%s' is not a valid number.\n", opt_val);
		help(app_name);
		return -1;
	    }
	    continue;
	    break;
	default: // match but something went wrong
	    help(app_name);
	    return -1;
	}
	switch (cnt = arg_match(argc, argv, &opt_val, "t", "-threads")) {
	case 0: // no match
	    break;
	case 1:
	case 2:
	    p->tcnt = strtol(opt_val, &end, 10);
	    if ('\0' != *end || 1 > p->tcnt) {
		printf("'%s' is not a valid thread count.\n", opt_val);
		help(app_name);
		return -1;
	    }
	    continue;
	    break;
	default: // match but something went wrong
	    help(app_name);
	    return -1;
	}
	switch (cnt = arg_match(argc, argv, &opt_val, "c", "-connection")) {
	case 0: // no match
	    break;
	case 1:
	case 2:
	    p->ccnt = strtol(opt_val, &end, 10);
	    if ('\0' != *end || 1 > p->ccnt) {
		printf("'%s' is not a valid connection number.\n", opt_val);
		help(app_name);
		return -1;
	    }
	    continue;
	    break;
	default: // match but something went wrong
	    help(app_name);
	    return -1;
	}
	switch (cnt = arg_match(argc, argv, &opt_val, "b", "-backlog")) {
	case 0: // no match
	    break;
	case 1:
	case 2:
	    p->backlog = strtol(opt_val, &end, 10);
	    if ('\0' != *end || 1 > p->ccnt || PIPELINE_SIZE <= p->backlog) {
		printf("'%s' is not a valid backlog number.\n", opt_val);
		help(app_name);
		return -1;
	    }
	    continue;
	    break;
	default: // match but something went wrong
	    help(app_name);
	    return -1;
	}
	switch (cnt = arg_match(argc, argv, &p->path, "p", "-path")) {
	case 0: // no match
	    break;
	case 1:
	case 2:
	    continue;
	    break;
	default: // match but something went wrong
	    help(app_name);
	    return -1;
	}
	switch (cnt = arg_match(argc, argv, &p->req_file, "r", "-request")) {
	case 0: // no match
	    break;
	case 1:
	case 2:
	    continue;
	    break;
	default: // match but something went wrong
	    help(app_name);
	    return -1;
	}
	switch (cnt = arg_match(argc, argv, NULL, "k", "-keep-alive")) {
	case 0: // no match
	    break;
	case 1:
	    p->keep_alive = true;
	    continue;
	    break;
	default: // match but something went wrong
	    help(app_name);
	    return -1;
	}
	switch (cnt = arg_match(argc, argv, &opt_val, "a", "-add")) {
	case 0: // no match
	    break;
	case 1:
	case 2: {
	    Header	h = (Header)malloc(sizeof(struct _Header));

	    h->next = p->headers;
	    p->headers = h;
	    h->line = opt_val;
	    continue;
	    break;
	}
	default: // match but something went wrong
	    help(app_name);
	    return -1;
	}
	if (NULL == p->addr) {
	    p->addr = *argv;
	    cnt = 1;
	} else {
	    printf("Only one server address is allowed.\n");
	    help(app_name);
	    return -1;
	}
    }
    if (NULL == p->addr) {
	printf("A server address is required.\n");
	help(app_name);
	return -1;
    }
    if (NULL == p->req_file) {
	if ('/' == *p->path) {
	    p->path = p->path + 1;
	}	
	build_req(p);
	if (0 > p->req_len) {
	    printf("*-*-* Failed to allocate memory for GET request.\n");
	    return -1;
	}
    } else {
	FILE	*f = fopen(p->req_file, "r");

	if (NULL == f) {
	    printf("-*-*- Failed to open '%s'. %s\n", p->req_file, strerror(errno));
	    return -1;
	}
	fseek(f, 0, SEEK_END);
	p->req_len = (int)ftell(f);
	rewind(f);
	if (NULL == (p->req_body = (char*)malloc(p->req_len + 1))) {
	    printf("-*-*- Failed to allocate memory for request.\n");
	    return -1;
	}
	if (p->req_len != fread(p->req_body, 1, p->req_len, f)) {
	    printf("-*-*- Failed to read %s.\n", p->req_file);
	    return -1;
	}
	p->req_body[p->req_len] = '\0';
	fclose(f);
	p->replace = (NULL != strstr(p->req_body, "${sequence}"));
    }
    p->inited = true;
    if (NULL == (p->pools = (Pool)malloc(sizeof(struct _Pool) * p->tcnt))) {
	printf("-*-*- Failed to allocate %ld threads.\n", p->tcnt);
	return -1;
    }
    Pool	pool;
    int		i;
    int		err;
    long	n = num / p->tcnt;

    if (!p->keep_alive) {
	p->backlog = 1;
    }
    for (pool = p->pools, i = p->tcnt; 0 < i; i--, pool++) {
	if (0 != (err = pool_init(pool, p, n))) {
	    return err;
	}
    }
    return 0;
}

static void
perfer_cleanup(Perfer p) {
    if (!p->inited) {
	return;
    }
    p->inited = false;
    Pool	pool;
    int		i;

    for (pool = p->pools, i = p->tcnt; 0 < i; i--, pool++) {
	pool_cleanup(pool);
    }
    free(p->pools);
    free(p->req_body);
}

void
perfer_stop(Perfer p) {
    p->done = true;
    
    Pool	pool;
    int		i;

    for (pool = p->pools, i = p->tcnt; 0 < i; i--, pool++) {
	pool_wait(pool);
    }
    perfer_cleanup(p);
}

static int
perfer_start(Perfer p) {
    Pool	pool;
    int		i;
    int		err;
    long	sent_cnt = 0;
    long	ok_cnt = 0;
    long	err_cnt = 0;
    double	lat_sum = 0.0;
    double	psum = 0.0;
    double	lat = 0.0;
    double	rate = 0.0;
    double	stdev = 0.0;
    
    for (pool = p->pools, i = p->tcnt; 0 < i; i--, pool++) {
	if (0 != (err = pool_start(pool))) {
	    perfer_stop(p);
	    return err;
	}
    }
    for (pool = p->pools, i = p->tcnt; 0 < i; i--, pool++) {
	pool_wait(pool);
	sent_cnt += pool->sent_cnt;
	ok_cnt += pool->ok_cnt;
	err_cnt += pool->err_cnt;
	lat_sum += pool->lat_sum;
	psum += pool->actual_end - p->start_time;
	stdev += pool->lat_sq_sum;
    }
    // TBD actual times for each thread
    if (0 < err_cnt) {
	printf("%s encountered %ld errors.\n", p->addr, err_cnt);
    }
    if (ok_cnt + err_cnt < sent_cnt) {
	printf("%s did not respond to %ld requests.\n", p->addr, sent_cnt - ok_cnt - err_cnt);
    }
    if (0.0 < psum) {
	rate = (double)ok_cnt / (psum / p->tcnt);
    }
    if (0 < ok_cnt) {
	lat = lat_sum * 1000.0 / ok_cnt;
	stdev /= (double)ok_cnt;
	stdev = sqrt(stdev) * 1000.0;
    }
    printf("Benchmarks for:\n");
    printf("  URL:                %s/%s\n", p->addr, p->path);
    printf("  Threads:            %ld\n", p->tcnt);
    printf("  Connections/thread: %ld\n", p->ccnt);
    printf("  Duration:           %0.1f seconds\n", psum / p->tcnt);
    printf("  Keep-Alive:         %s\n", p->keep_alive ? "true" : "false");
    printf("Results:\n");
    if (0 < err_cnt) {
	printf("  Failures:           %ld\n", err_cnt);
    }
    printf("  Throughput:         %ld requests/second\n", (long)rate);
    printf("  Latency:            %0.3f +/-%0.3f msecs (and stdev)\n", lat, stdev);
    
    return 0;
}

static void
sig_handler(int sig) {
    if (perfer.inited) {
        perfer_stop(&perfer);
        perfer.inited = false;
    }
    exit(sig);
}

int
main(int argc, const char **argv) {
    int	err;
    
    if (0 != (err = perfer_init(&perfer, argc, argv))) {
	return err;
    }
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    if (0 != (err = perfer_start(&perfer))) {
	return err;
    }
    perfer_cleanup(&perfer);

    return 0;
}
