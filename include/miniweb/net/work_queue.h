#ifndef MINIWEB_NET_WORK_QUEUE_H
#define MINIWEB_NET_WORK_QUEUE_H

#include <signal.h>
#include <pthread.h>

#define MINIWEB_QUEUE_CAPACITY 4096

typedef struct {
	void *items[MINIWEB_QUEUE_CAPACITY];
	int head;
	int tail;
	int count;
	pthread_mutex_t lock;
	pthread_cond_t not_empty;
} miniweb_work_queue_t;

void miniweb_work_queue_init(miniweb_work_queue_t *q);
int miniweb_work_queue_push(miniweb_work_queue_t *q, void *item);
void *miniweb_work_queue_pop(miniweb_work_queue_t *q,
    volatile sig_atomic_t *running);
void miniweb_work_queue_broadcast_shutdown(miniweb_work_queue_t *q);

#endif
