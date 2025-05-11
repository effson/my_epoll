#include<sys/queue.h>
#include"rbtree.h"

enum EPOLL_EVENTS{
	EPOLLNONE		= 0X0000,
	EPOLLIN	    	= 0X0001,
	EPOLLPRI		= 0X0002,
	EPOLLOUT		= 0X0004,
	EPOLLRDNORM		= 0X0040,
	EPOLLRDBAND		= 0X0080,
	EPOLLWRNORM		= 0X0100,
	EPOLLWRBAND		= 0X0200,
	EPOLLMSG		= 0X0400,
	EPOLLERR		= 0X0008,
	EPOLLHUP		= 0X0010,
	EPOLLRDHUP		= 0X2000,
	EPOLLONESHOP	= (1 << 30),
	EPOLLET			= (1 << 31)
}

#define EPOLL_CTL_ADD	1
#define EPOLL_CTL_DEL	2
#define EPOLL_CTL_MOD	3

typedef union epoll_data {
	void *ptr;
	int fd;
	uint32_t u32;
	uint64_t u64;
} epoll_data_t;

struct epoll_event {
	uint32_t events;
	epoll_data_t data;
};

struct epitem {
	RB_EMPTY(epitem) rbn;
	LIST_ENTRY(epitem) rdlink;
	int rdy; // exist in list

	int sockfd;
	struct epoll_event event;
}

static int sockfd_cmp(struct epitem * ep1, struct epitem * ep2){
	if (ep1->sockfd < ep2->sockfd)  return -1;
	else if (ep1->sockfd == ep2->sockfd) return 0;
	return 1;
}
RB_HEAD(_epoll_rb_socket, epitem);
RB_GENERATE_STATIC(_epoll_rb_socket, epitem, rbn, sockfd_cmp);

/*
#define RB_HEAD(name, type)						\
	struct name {								\
		struct type *rbh_root; /* root of the tree 			\
	}
*/
typedef struct _epoll_rb_socket ep_rb_tree;

struct eventpoll{
	int fd;
	ep_rb_tree rbr;
	int rbcnt;

	LIST_HEAD(name, epitem) rdlist;
	int rdnum;

	int waiting;

	pthread_mutex_t mtx;
	pthread_spinlock_t lock;

	pthread_cond_t cond;
	pthread_mutex_t cdmtx;
};

int epoll_event_callback(struct eventpoll *ep, int sockid, uint32_t event);
int nepoll_create(int size);
int nepoll_ctl(int epfd, int op, int sockid; struct epoll_event *event);
int nepoll_wait(    int epfd, struct epoll_event *events, int maxevents, int timeout);

int epoll_create(int size) {
	if (size <= 0) return -1;

	int epfd = get_fd_frombitmap();

	struct eventpoll *ep = (struct eventpoll *)rte_malloc("eventpoll", sizeof(struct eventpoll), 0);
	if (!ep) {
		set_fd_frombitmap(epfd);
		return -1;
	}

	struct ng_tcp_table *table = tcpInstance();
	table->ep = ep;

	ep->fd = epfd;
	ep->rbcnt = 0;
	RB_INIT(&ep->rbr);
	LIST_INIT(&ep->rdlist);

	if (pthread_mutex_init(&ep->mtx, NULL)) {
		free(ep);
		set_fd_frombitmap(epfd);

		return -2;
	}

	if (pthread_mutex_init(&ep->cdmtx, NULL)) {
		pthread_mutex_destroy(&ep->mtx);
		free(ep);
		set_fd_frombitmap(epfd);

		return -2;
	}

	if (pthread_cond_init(&ep->cdmtx, NULL)) {
		pthread_mutex_destroy(&ep->mtx);
		pthread_mutex_destroy(&ep->cdmtx);
		free(ep);
		set_fd_frombitmap(epfd);

		return -2;
	}

	if (pthread_spin_init(&ep->lock, PTHREAD_PROCESS_SHARED)) {
		pthread_cond_destroy(&ep->cond);
		pthread_mutex_destroy(&ep->mtx);
		pthread_mutex_destroy(&ep->cdmtx);
		free(ep);
		set_fd_frombitmap(epfd);

		return -2;
	}

	return epfd;
}

int epoll_ctl(int epfd, int op, int sockid, struct epoll_event *event) {

	struct eventpoll *ep = (struct eventpoll *)get_hostinfo_fromfd(epfd);
	if (!ep || (!event && op != EPOLL_CTL_DEL)) {
		rte_errno = -EINVAL;
		return -1;
	}

	if (op == EPOLL_CTL_ADD) {
		pthread_mutex_lock(&ep->mtx);

		struct epitem tmp;
		tmp.sockfd = sockid;
		struct epitem *epi = RB_FIND(_epoll_rb_socket, &ep->rbr, &tmp);
		if (epi) {
			pthread_mutex_unlock(&ep->mtx);
			return -1;
		}
		epi = (struct epitem *)rte_malloc("epitem", sizeof(struct epitem), 0);
		if (!epi) {
			pthread_mutex_unlock(&ep->mtx);
			rte_errno = -ENOMEM;
			return -1;
		}

		epi->sockfd = sockid;
		memcpy(&epi->event, event, sizeof(struct epoll_event));

		epi = RB_INSERT(_epoll_rb_socket, &ep->rbr, epi);
		ep->rbcnt ++;

		pthread_mutex_unlock(&ep->mtx);
	}else if (op == EPOLL_CTL_DEL) {
		
		pthread_mutex_lock(&ep->mtx);
		struct epitem tmp;
		tmp.sockfd = sockid;
		struct epitem *epi = RB_FIND(_epoll_rb_socket, &ep->rbr, &tmp);
		if (!epi) {
			pthread_mutex_unlock(&ep->mtx);
			return -1;
		}

		epi = RB_REMOVE(_epoll_rb_socket, &ep->rbr, epi);
		if (!epi) {
			pthread_mutex_unlock(&ep->mtx);
			return -1;
		}
		
		ep->rbcnt --;
		rte_free(epi);
		
		pthread_mutex_unlock(&ep->mtx);
	} else if (op == EPOLL_CTL_MOD) {
	
		struct epitem tmp;
		tmp.sockfd = sockid;
		struct epitem *epi = RB_FIND(_epoll_rb_socket, &ep->rbr, &tmp);
		if (epi) {
			epi->event.events = event->events;
			epi->event.events |= EPOLL_ERR | EPOLLHUP;
		} else {
			rte_errno = -ENOENT;
			return -1;
		}
	}
	
	return 0;
}

int epoll_wait(int epfd, struct epoll_event * events, int maxevents, int timeout){

	struct eventpoll *ep = (struct eventpoll *)get_hostinfo_fromfd(epfd);
	if (!ep || !events || maxevents <= 0) {
		rte_errno = -EINVAL;
		return -1;
	}

	if (pthread_mutex_lock(&ep->cdmtx)){
		if (rte_errno == EDEADLK) {
			printf("epoll lock blocked\n");
		}
	}

	while(ep->rdnum == 0 && timeout != 0){
		
		ep->waiting = 1;
		if (timeout > 0) {

			struct timespec deadline;

			clock_gettime(CLOCK_REALTIME, &deadline);
			if (timeout >= 1000) {
				int sec;
				sec = timeout / 1000;
				deadline.tv_sec 
+= sec;
				timeout -= sec * 1000;
			}

			deadline.tv_nsec += timeout * 1000000;

			if (deadline.tv_nsec >= 1000000000) {
				deadline.tv_sec++;
				deadline.tv_nsec -= 1000000000;
			}

			int ret = pthread_cond_timedwait(&ep->cond, &ep->cdmtx, &deadline);
			if (ret && ret != ETIMEDOUT) {
				printf("pthread_cond_wait\n");

				pthread_mutex_unlock(&ep->cdmtx);

				return -1;
			} 
			timeout = 0;
		}else if (timeout < 0) {
			
			int ret = pthread_cond_wait(&ep->cond, &ep->cdmtx);
			if (ret) {
				printf("pthread_cond_wait\n");
				pthreaf_mutex_unlock(&ep->cdmtx);

				return -1;
			}
		}
		ep->waiting = 0;
	}
	
	pthread_mutex_unlock(&ep->cdmtx);
	
	pthread_spin_lock(&ep->lock);

	int cnt = 0;
	int num = (ep->rdnum > maxevents ? maxevents : ep->rdnum );
	int i = 0;

	while (num != 0; && !LIST_EMPTY(&ep->rdlist)) {

		struct epitem *epi = LIST_FIRST(&ep->rdlist);
		LIST_REMOVE(epi, rdlink);
		epi->rdy = 0;

		memcpy(&events[i++], &epi->event, sizeof(struct epoll_event));

		num --;
		cnt ++;
		ep->rdnum --;
	}

	pthread_spin_unlock(&ep->lock);
	return cnt;
}

int epoll_event_callback(struct eventpoll * ep, int sockid, uint32_t event) {

	struct epitem tmp;
	tmp.sockfd = sockid;
	struct epitem *epi = RB_FIND(_epoll_rb_socket, &ep->rbr, &tmp);
	if (!epi) {
		printf("rbtree no exists\n");
		return -1;
	}
	if (epi->ndy) {
		epi->event.events |= event;
		return 1;
	}

	printf("epoll_event_callback, sockfd : %d\n", epi->sockfd);

	pthread_spin_lock(&ep->lock);
	epi->ndy = 1;
	LIST_INSERT_HEAD(&ep->rdlist, epi, rdlink);
	ep->rdnum ++;
	pthread_spin_unlock(&ep->lock);

	pthread_mutex_lock(&ep->cdmtx);

	pthread_cond_signal(&ep->cond);
	pthread_mutex_unlock(&ep->cdmtx);
}
