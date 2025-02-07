#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>

#include "http_parsing.h"
#include "netlib.h"
#include "debug.h"

#define MAX_FLOW_NUM  (10000)

#define RCVBUF_SIZE (2*1024)
#define SNDBUF_SIZE (8*1024)

#define MAX_EVENTS (MAX_FLOW_NUM * 3)

#define HTTP_HEADER_LEN 1024
#define URL_LEN 128

#define MAX_FILES 30

#define NAME_LIMIT 256
#define FULLNAME_LIMIT 512

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef ERROR
#define ERROR (-1)
#endif

#define HT_SUPPORT FALSE

#ifndef MAX_CPUS
#define MAX_CPUS		16
#endif

/*#define TRACE_APP(f, m...) { \
 printf(f, ##m); \
  }*/

#ifdef MOD
#define CACHE_LINE_SZ 128
#define ONE_MILLION 1000000
typedef struct cache_aware_var {
  uint64_t var;
  char dummy[CACHE_LINE_SZ-8];
} cache_aware_var;
static volatile cache_aware_var g_var[MAX_CPUS];
#endif


/*----------------------------------------------------------------------------*/
struct file_cache
{
	char name[NAME_LIMIT];
	char fullname[FULLNAME_LIMIT];
	uint64_t size;
	char *file;
};
/*----------------------------------------------------------------------------*/
struct server_vars
{
	char request[HTTP_HEADER_LEN];
	int recv_len;
	int request_len;
	long int total_read, total_sent;
	uint8_t done;
	uint8_t rspheader_sent;
	uint8_t keep_alive;

	int fidx;						// file cache index
	char fname[NAME_LIMIT];				// file name
	long int fsize;					// file size
};
/*----------------------------------------------------------------------------*/
struct thread_context
{
	int ep;
	struct server_vars *svars;
};
/*----------------------------------------------------------------------------*/
static int num_cores;
static int core_limit;
static pthread_t app_thread[MAX_CPUS];
static int done[MAX_CPUS];
static char *conf_file = NULL;
static int backlog = -1;
/*----------------------------------------------------------------------------*/
const char *www_main;
static struct file_cache fcache[MAX_FILES];
static int nfiles;
/*----------------------------------------------------------------------------*/
static int finished;
/*----------------------------------------------------------------------------*/
static char *
StatusCodeToString(int scode)
{
	switch (scode) {
		case 200:
			return "OK";
			break;

		case 404:
			return "Not Found";
			break;
	}

	return NULL;
}
/*----------------------------------------------------------------------------*/
void
CleanServerVariable(struct server_vars *sv)
{
	sv->recv_len = 0;
	sv->request_len = 0;
	sv->total_read = 0;
	sv->total_sent = 0;
	sv->done = 0;
	sv->rspheader_sent = 0;
	sv->keep_alive = 0;
}
/*----------------------------------------------------------------------------*/
void 
CloseConnection(struct thread_context *ctx, int sockid, struct server_vars *sv)
{
	epoll_ctl(ctx->ep, EPOLL_CTL_DEL, sockid, NULL);
	close(sockid);
}
/*----------------------------------------------------------------------------*/
static int 
SendUntilAvailable(struct thread_context *ctx, int sockid, struct server_vars *sv)
{
	int ret;
	int sent;
	int len;

	if (sv->done || !sv->rspheader_sent) {
		return 0;
	}

	sent = 0;
	ret = 1;
	while (ret > 0) {
		len = MIN(SNDBUF_SIZE, sv->fsize - sv->total_sent);
		if (len <= 0) {
			break;
		}
		ret = write(sockid,  
				fcache[sv->fidx].file + sv->total_sent, len);
		if (ret < 0) {
			TRACE_APP("Connection closed with client.\n");
			break;
		}
		TRACE_APP("Socket %d: write try: %d, ret: %d\n", sockid, len, ret);
		sent += ret;
		sv->total_sent += ret;
	}

	if (sv->total_sent >= fcache[sv->fidx].size) {
		struct epoll_event ev;
		sv->done = TRUE;
		finished++;

		if (sv->keep_alive) {
			/* if keep-alive connection, wait for the incoming request */
			ev.events = EPOLLIN;
			ev.data.fd = sockid;
			epoll_ctl(ctx->ep, EPOLL_CTL_MOD, sockid, &ev);

			CleanServerVariable(sv);
		} else {
			/* else, close connection */
			CloseConnection(ctx, sockid, sv);
		}
	}

	return sent;
}
/*----------------------------------------------------------------------------*/
static int 
HandleReadEvent(struct thread_context *ctx, int sockid, struct server_vars *sv, int cpu)
{
	struct epoll_event ev;
	char buf[HTTP_HEADER_LEN];
	char url[URL_LEN];
	char response[HTTP_HEADER_LEN];
	int scode;						// status code
	time_t t_now;
	char t_str[128];
	char keepalive_str[128];
	int rd;
	int i;
	int len;
	int sent;

	/* HTTP request handling */
	rd = read(sockid, buf, HTTP_HEADER_LEN);
	if (rd <= 0) {
		return rd;
	}
	memcpy(sv->request + sv->recv_len, 
			(char *)buf, MIN(rd, HTTP_HEADER_LEN - sv->recv_len));
	sv->recv_len += rd;
	//sv->request[rd] = '\0';
	//fprintf(stderr, "HTTP Request: \n%s", request);
	sv->request_len = find_http_header(sv->request, sv->recv_len);
	if (sv->request_len <= 0) {
		TRACE_ERROR("Socket %d: Failed to parse HTTP request header.\n"
				"read bytes: %d, recv_len: %d, "
				"request_len: %d, strlen: %ld, request: \n%s\n", 
				sockid, rd, sv->recv_len, 
				sv->request_len, strlen(sv->request), sv->request);
		return rd;
	}

#ifdef MOD
    int iter;
    for (iter=0;iter<ONE_MILLION;iter++)
      g_var[cpu].var++;
#endif

	http_get_url(sv->request, sv->request_len, url, URL_LEN);
	TRACE_APP("Socket %d URL: %s\n", sockid, url);
	sprintf(sv->fname, "%s%s", www_main, url);
	TRACE_APP("Socket %d File name: %s\n", sockid, sv->fname);

	sv->keep_alive = FALSE;
	if (http_header_str_val(sv->request, "Connection: ", 
				strlen("Connection: "), keepalive_str, 128)) {	
		if (strstr(keepalive_str, "Keep-Alive")) {
			sv->keep_alive = TRUE;
		} else if (strstr(keepalive_str, "Close")) {
			sv->keep_alive = FALSE;
		}
	}

	/* Find file in cache */
	scode = 404;
	for (i = 0; i < nfiles; i++) {
		if (strcmp(sv->fname, fcache[i].fullname) == 0) {
			sv->fsize = fcache[i].size;
			sv->fidx = i;
			scode = 200;
			break;
		}
	}
	TRACE_APP("Socket %d File size: %ld (%ldMB)\n", 
			sockid, sv->fsize, sv->fsize / 1024 / 1024);

	/* Response header handling */
	time(&t_now);
	strftime(t_str, 128, "%a, %d %b %Y %X GMT", gmtime(&t_now));
	if (sv->keep_alive)
		sprintf(keepalive_str, "Keep-Alive");
	else
		sprintf(keepalive_str, "Close");

	sprintf(response, "HTTP/1.1 %d %s\r\n"
			"Date: %s\r\n"
			"Server: Webserver on Middlebox TCP (Ubuntu)\r\n"
			"Content-Length: %ld\r\n"
			"Connection: %s\r\n\r\n", 
			scode, StatusCodeToString(scode), t_str, sv->fsize, keepalive_str);
	len = strlen(response);
	TRACE_APP("Socket %d HTTP Response: \n%s", sockid, response);
	sent = write(sockid, response, len);
	TRACE_APP("Socket %d Sent response header: try: %d, sent: %d\n", 
			sockid, len, sent);
	assert(sent == len);
	sv->rspheader_sent = TRUE;

	ev.events = EPOLLIN | EPOLLOUT;
	ev.data.fd = sockid;
	epoll_ctl(ctx->ep, EPOLL_CTL_MOD, sockid, &ev);

	SendUntilAvailable(ctx, sockid, sv);

	return rd;
}
/*----------------------------------------------------------------------------*/
int 
AcceptConnection(struct thread_context *ctx, int listener)
{
	struct server_vars *sv;
	struct epoll_event ev;
	int c;

	c = accept(listener, NULL, NULL);

	if (c >= 0) {
		if (c >= MAX_FLOW_NUM) {
			TRACE_ERROR("Invalid socket id %d.\n", c);
			return -1;
		}

		sv = &ctx->svars[c];
		CleanServerVariable(sv);
		TRACE_APP("New connection %d accepted.\n", c);
		ev.events = EPOLLIN;
		ev.data.fd = c;

		int flags = fcntl(c,F_GETFL,0);
		fcntl(c, F_SETFL, flags | O_NONBLOCK);
		//		setsock_nonblock(c);
		epoll_ctl(ctx->ep, EPOLL_CTL_ADD, c, &ev);
		TRACE_APP("Socket %d registered.\n", c);

	} else {
		if (errno != EAGAIN) {
			TRACE_ERROR("accept() error %s\n", 
					strerror(errno));
		}
	}

	return c;
}
/*----------------------------------------------------------------------------*/
struct thread_context *
InitializeServerThread(int core)
{
	struct thread_context *ctx;

	printf("Commented out core_affinitize - was that good?\n");
	/* affinitize application thread to a CPU core */
	/*#if HT_SUPPORT
	core_affinitize(core + (num_cores / 2));
#else
	core_affinitize(core);
	#endif *//* HT_SUPPORT */

	ctx = (struct thread_context *)calloc(1, sizeof(struct thread_context));
	if (!ctx) {
		TRACE_ERROR("Failed to create thread context!\n");
		return NULL;
	}

	/* create epoll descriptor */
	ctx->ep = epoll_create(MAX_EVENTS);
	if (ctx->ep < 0) {
		free(ctx);
		TRACE_ERROR("Failed to create epoll descriptor!\n");
		return NULL;
	}

	/* allocate memory for server variables */
	ctx->svars = (struct server_vars *)
			calloc(MAX_FLOW_NUM, sizeof(struct server_vars));
	if (!ctx->svars) {
		close(ctx->ep);
		free(ctx);
		TRACE_ERROR("Failed to create server_vars struct!\n");
		return NULL;
	}

	return ctx;
}

int listener=0;
/*----------------------------------------------------------------------------*/
int 
CreateListeningSocket()
{
	struct sockaddr_in saddr;
	int ret;

	if(listener) {
	  printf("Only call CreateListeningSocket once!\n");
	  assert(0);
	}
	/* create socket and set it as nonblocking */
	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0) {
		TRACE_ERROR("Failed to create listening socket!\n");
		return -1;
	}

	int one = 1;
	if(setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one)) < 0) {
	  perror("problem with reuseaddr");
	  return -1;
	}
	int flags = fcntl(listener,F_GETFL,0);
	fcntl(listener, F_SETFL, flags | O_NONBLOCK);
	//	ret = setsock_nonblock(listener);
	//	if (ret < 0) {
	//		TRACE_ERROR("Failed to set socket in nonblocking mode.\n");
	//	return -1;
	//	}

	printf("creating a listening socket");
	/* bind to port 80 */
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = INADDR_ANY;
	saddr.sin_port = htons(8080);
	ret = bind(listener, 
			(struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
	if (ret < 0) {
	  perror("couldn't bind");
		TRACE_ERROR("Failed to bind to the listening socket!\n");
		return -1;
	}

	/* listen (backlog: can be configured) */
	ret = listen(listener, backlog);
	if (ret < 0) {
		TRACE_ERROR("listen() failed!\n");
		return -1;
	}

	return listener;
}
/*----------------------------------------------------------------------------*/
void *
RunServerThread(void *arg)
{
	int core = *(int *)arg;
	struct thread_context *ctx;
	int ep;
	struct epoll_event *events;
	struct epoll_event ev;
	int nevents;
	int i, ret;
	int do_accept;
	
	/* initialization */
  //TRACE_APP("Spawned thread %d\n", core);
	ctx = InitializeServerThread(core);
	if (!ctx) {
		TRACE_ERROR("Failed to initialize server thread.\n");
		return NULL;
	}
	ep = ctx->ep;

	/* wait for incoming accept events */
	ev.events = EPOLLIN;
	ev.data.fd = listener;
	epoll_ctl(ctx->ep, EPOLL_CTL_ADD, listener, &ev);

	
	events = (struct epoll_event *)
			calloc(MAX_EVENTS, sizeof(struct epoll_event));
	if (!events) {
		TRACE_ERROR("Failed to create event struct!\n");
		exit(-1);
	}

	while (!done[core]) {
		nevents = epoll_wait(ep, events, MAX_EVENTS, -1);
		if (nevents < 0) {
			if (errno != EINTR)
				perror("epoll_wait");
			break;
		}

		do_accept = FALSE;
		for (i = 0; i < nevents; i++) {

			if (events[i].data.fd == listener) {
				/* if the event is for the listener, accept connection */
				do_accept = TRUE;

			} else if (events[i].events & EPOLLERR) {
				int err;
				socklen_t len = sizeof(err);

				/* error on the connection */
				TRACE_APP("[CPU %d] Error on socket %d\n", 
						core, events[i].data.fd);
				if (getsockopt(events[i].data.fd, 
						SOL_SOCKET, SO_ERROR, (void *)&err, &len) == 0) {
					if (err != ETIMEDOUT) {
						fprintf(stderr, "Error on socket %d: %s\n", 
								events[i].data.fd, strerror(err));
					}
				} else {
					perror("getsockopt");
				}
				CloseConnection(ctx, events[i].data.fd, 
						&ctx->svars[events[i].data.fd]);

			} else if (events[i].events & EPOLLIN) {
				ret = HandleReadEvent(ctx, events[i].data.fd, 
						&ctx->svars[events[i].data.fd], core);

				if (ret == 0) {
					/* connection closed by remote host */
					CloseConnection(ctx, events[i].data.fd, 
							&ctx->svars[events[i].data.fd]);
				} else if (ret < 0) {
					/* if not EAGAIN, it's an error */
					if (errno != EAGAIN) {
						CloseConnection(ctx, events[i].data.fd, 
								&ctx->svars[events[i].data.fd]);
					}
				}

			} else if (events[i].events & EPOLLOUT) {
				struct server_vars *sv = &ctx->svars[events[i].data.fd];
				if (sv->rspheader_sent) {
					SendUntilAvailable(ctx, events[i].data.fd, sv);
				} else {
					TRACE_APP("Socket %d: Response header not sent yet.\n", 
							events[i].data.fd);
				}

			} else {
				assert(0);
			}
		}

		/* if do_accept flag is set, accept connections */
		if (do_accept) {
			while (1) {
				ret = AcceptConnection(ctx, listener);
				if (ret < 0)
					break;
			}
		}

	}

	/* destroy mtcp context: this will kill the mtcp thread */
	pthread_exit(NULL);

	return NULL;
}
/*----------------------------------------------------------------------------*/
void
SignalHandler(int signum)
{
	int i;

	for (i = 0; i < core_limit; i++) {
		if (app_thread[i] == pthread_self()) {
			//TRACE_INFO("Server thread %d got SIGINT\n", i);
			done[i] = TRUE;
		} else {
			if (!done[i]) {
				pthread_kill(app_thread[i], signum);
			}
		}
	}
}
/*----------------------------------------------------------------------------*/
static void
printHelp(const char *prog_name)
{
	TRACE_CONFIG("%s -p <path_to_www/> -f <conf_file> "
		     "[-N num_cores] [-c <per-process core_id>] [-h]\n",
		     prog_name);
	exit(EXIT_SUCCESS);
}
/*----------------------------------------------------------------------------*/
int 
main(int argc, char **argv)
{
	DIR *dir;
	struct dirent *ent;
	int fd;
	int ret;
	uint64_t total_read;
	//	struct conf mcfg;
	int cores[MAX_CPUS];
	int process_cpu;
	int i, o;

#ifdef MOD
  printf("Running modified version\n");
#else
  printf("Running unmodified version\n");
#endif
	printf("replaced GetNumCPUs with '64'. was that good?\n");
	num_cores = 16;//GetNumCPUs();
	core_limit = num_cores;
	process_cpu = -1;
	dir = NULL;

	if (argc < 2) {
		TRACE_CONFIG("$%s directory_to_service\n", argv[0]);
		return FALSE;
	}

	while (-1 != (o = getopt(argc, argv, "N:f:p:c:b:h"))) {
		switch (o) {
		case 'p':
			/* open the directory to serve */
			www_main = optarg;
			dir = opendir(www_main);
			if (!dir) {
				TRACE_CONFIG("Failed to open %s.\n", www_main);
				perror("opendir");
				return FALSE;
			}
			break;
		case 'N':
			core_limit = mystrtol(optarg, 10);
			if (core_limit > num_cores) {
				TRACE_CONFIG("CPU limit should be smaller than the "
					     "number of CPUs: %d\n", num_cores);
				return FALSE;
			}
			/** 
			 * it is important that core limit is set 
			 * before init() is called. You can
			 * not set core_limit after init()
			 */
			printf("The option N doesn't work after I commented something out.\n");
			//			getconf(&mcfg);
			//			mcfg.num_cores = core_limit;
			//			setconf(&mcfg);
			break;
		case 'f':
			conf_file = optarg;
			break;
		case 'c':
			process_cpu = mystrtol(optarg, 10);
			if (process_cpu > core_limit) {
				TRACE_CONFIG("Starting CPU is way off limits!\n");
				return FALSE;
			}
			break;
		case 'b':
			backlog = mystrtol(optarg, 10);
			break;
		case 'h':
			printHelp(argv[0]);
			break;
		}
	}
	
	if (dir == NULL) {
		TRACE_CONFIG("You did not pass a valid www_path!\n");
		exit(EXIT_FAILURE);
	}

	nfiles = 0;
	while ((ent = readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0)
			continue;
		else if (strcmp(ent->d_name, "..") == 0)
			continue;

		snprintf(fcache[nfiles].name, NAME_LIMIT, "%s", ent->d_name);
		snprintf(fcache[nfiles].fullname, FULLNAME_LIMIT, "%s/%s",
			 www_main, ent->d_name);
		fd = open(fcache[nfiles].fullname, O_RDONLY);
		if (fd < 0) {
			perror("open");
			continue;
		} else {
			fcache[nfiles].size = lseek64(fd, 0, SEEK_END);
			lseek64(fd, 0, SEEK_SET);
		}

		fcache[nfiles].file = (char *)malloc(fcache[nfiles].size);
		if (!fcache[nfiles].file) {
			TRACE_CONFIG("Failed to allocate memory for file %s\n", 
				     fcache[nfiles].name);
			perror("malloc");
			continue;
		}

		TRACE_INFO("Reading %s (%lu bytes)\n", 
				fcache[nfiles].name, fcache[nfiles].size);
		total_read = 0;
		while (1) {
			ret = read(fd, fcache[nfiles].file + total_read, 
					fcache[nfiles].size - total_read);
			if (ret < 0) {
				break;
			} else if (ret == 0) {
				break;
			}
			total_read += ret;
		}
		if (total_read < fcache[nfiles].size) {
			free(fcache[nfiles].file);
			continue;
		}
		close(fd);
		nfiles++;

		if (nfiles >= MAX_FILES)
			break;
	}

	finished = 0;

	/* initialize mtcp */
	/*	if (conf_file == NULL) {
		TRACE_CONFIG("You forgot to pass the mTCP startup config file!\n");
		exit(EXIT_FAILURE);
	}

	ret = init(conf_file);
	if (ret) {
		TRACE_CONFIG("Failed to initialize mtcp\n");
		exit(EXIT_FAILURE);
	}

	getconf(&mcfg);
	if (backlog > mcfg.max_concurrency) {
		TRACE_CONFIG("backlog can not be set larger than CONFIG.max_concurrency\n");
		return FALSE;
	}
	*/
	/* if backlog is not specified, set it to 4K */
	if (backlog == -1) {
		backlog = 4096;
	}
	
	/* register signal handler to mtcp */
	signal(SIGINT, SignalHandler);

	TRACE_INFO("Application initialization finished.\n");

	listener = CreateListeningSocket();
	if (listener < 0) {
		TRACE_ERROR("Failed to create listening socket.\n");
		exit(-1);
	}


	for (i = ((process_cpu == -1) ? 0 : process_cpu); i < core_limit; i++) {
		cores[i] = i;
		done[i] = FALSE;
		
		if (pthread_create(&app_thread[i], 
				   NULL, RunServerThread, (void *)&cores[i])) {
			perror("pthread_create");
			TRACE_CONFIG("Failed to create server thread.\n");
				exit(EXIT_FAILURE);
		}
		if (process_cpu != -1)
			break;
	}
	
	for (i = ((process_cpu == -1) ? 0 : process_cpu); i < core_limit; i++) {
		pthread_join(app_thread[i], NULL);

		if (process_cpu != -1)
			break;
	}
	
	//	destroy();
	closedir(dir);
	return 0;
}
