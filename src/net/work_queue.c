
#include <miniweb/net/work_queue.h>

#include <string.h>

/** Initialize a thread-safe FIFO work queue. */
void
miniweb_work_queue_init(miniweb_work_queue_t *q)
{
	memset(q, 0, sizeof(*q));
	pthread_mutex_init(&q->lock, NULL);
	pthread_cond_init(&q->not_empty, NULL);
}

/** Push one item to the queue; returns -1 when full. */
int
miniweb_work_queue_push(miniweb_work_queue_t *q, void *item)
{
	pthread_mutex_lock(&q->lock);
	if (q->count >= MINIWEB_QUEUE_CAPACITY) {
		pthread_mutex_unlock(&q->lock);
		return -1;
	}
	q->items[q->tail] = item;
	q->tail = (q->tail + 1) % MINIWEB_QUEUE_CAPACITY;
	q->count++;
	pthread_cond_signal(&q->not_empty);
	pthread_mutex_unlock(&q->lock);
	return 0;
}

/** Pop one item, blocking until available or shutdown is requested. */
void *
miniweb_work_queue_pop(miniweb_work_queue_t *q, volatile sig_atomic_t *running)
{
	void *item;

	pthread_mutex_lock(&q->lock);
	while (q->count == 0 && *running)
		pthread_cond_wait(&q->not_empty, &q->lock);
	if (q->count == 0) {
		pthread_mutex_unlock(&q->lock);
		return NULL;
	}
	item = q->items[q->head];
	q->head = (q->head + 1) % MINIWEB_QUEUE_CAPACITY;
	q->count--;
	pthread_mutex_unlock(&q->lock);
	return item;
}

/** Wake all waiting workers so they can observe shutdown state. */
void
miniweb_work_queue_broadcast_shutdown(miniweb_work_queue_t *q)
{
	pthread_mutex_lock(&q->lock);
	pthread_cond_broadcast(&q->not_empty);
	pthread_mutex_unlock(&q->lock);
}
