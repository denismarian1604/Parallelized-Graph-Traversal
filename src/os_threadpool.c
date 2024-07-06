// SPDX-License-Identifier: BSD-3-Clause

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <stdatomic.h>

#include "os_threadpool.h"
#include "log/log.h"
#include "utils.h"

extern atomic_int node_cnt;

/* Create a task that would be executed by a thread. */
os_task_t *create_task(void (*action)(void *), void *arg, void (*destroy_arg)(void *))
{
	os_task_t *t;

	t = malloc(sizeof(*t));
	DIE(t == NULL, "malloc");

	t->action = action;		// the function
	t->argument = arg;		// arguments for the function
	t->destroy_arg = destroy_arg;	// destroy argument function

	return t;
}

/* Destroy task. */
void destroy_task(os_task_t *t)
{
	if (t->destroy_arg != NULL)
		t->destroy_arg(t->argument);
	free(t);
}

/* Put a new task to threadpool task queue. */
void enqueue_task(os_threadpool_t *tp, os_task_t *t)
{
	assert(tp != NULL);
	assert(t != NULL);

	// lock the threadpool mutex
	pthread_mutex_lock(&tp->mutex_q);
	// add the task to the queue
	list_add_tail(&tp->head, &t->list);
	// unlock the threadpool mutex
	pthread_mutex_unlock(&tp->mutex_q);
	// signal a sleeping thread to pick up the newly enqueued task
	pthread_cond_signal(&tp->cond);
}

/*
 * Check if queue is empty.
 * This function should be called in a synchronized manner.
 */
static int queue_is_empty(os_threadpool_t *tp)
{
	return list_empty(&tp->head);
}

/*
 * Get a task from threadpool task queue.
 * Block if no task is available.
 * Return NULL if work is complete, i.e. no task will become available,
 * i.e. all threads are going to block.
 */

os_task_t *dequeue_task(os_threadpool_t *tp)
{
	os_task_t *t;

	// lock the threadpool mutex
	pthread_mutex_lock(&tp->mutex_q);

	// the queue is empty and the work is not done
	while (queue_is_empty(tp) && !tp->terminate) {
		// if there are no more nodes remaining to be processed
		// set the terminate variable to 1
		// unlock the threadpool mutex
		// broadcast the condition variable
		if (node_cnt == 0) {
			tp->terminate = 1;
			pthread_mutex_unlock(&tp->mutex_q);
			pthread_cond_broadcast(&tp->cond);
			return NULL;
		}

		// or else, wait for a task to be enqueued
		pthread_cond_wait(&tp->cond, &tp->mutex_q);
	}

	// if the work is done, unlock the threadpool mutex and return NULL
	if (tp->terminate) {
		pthread_mutex_unlock(&tp->mutex_q);
		return NULL;
	}

	// get the first task from the queue
	t = list_entry(tp->head.next, os_task_t, list);
	// remove it from the list
	list_del(tp->head.next);

	// unlockt the threadpool mutex
	pthread_mutex_unlock(&tp->mutex_q);

	// return the task
	return t;
}

/* Loop function for threads */
static void *thread_loop_function(void *arg)
{
	os_threadpool_t *tp = (os_threadpool_t *) arg;

	while (1) {
		os_task_t *t;

		t = dequeue_task(tp);
		if (t == NULL)
			break;
		t->action(t->argument);
		destroy_task(t);
	}

	return NULL;
}

/* Wait completion of all threads. This is to be called by the main thread. */
void wait_for_completion(os_threadpool_t *tp)
{
	// lock the threadpool mutex
	pthread_mutex_lock(&tp->mutex_q);

	// while the task queue is not empty, wait
	while (!queue_is_empty(tp))
		pthread_cond_wait(&tp->cond, &tp->mutex_q);

	// unlock the threadpool mutex
	pthread_mutex_unlock(&tp->mutex_q);

	// signal all threads to wake up
	pthread_cond_broadcast(&tp->cond);
	/* Join all worker threads. */
	for (unsigned int i = 0; i < tp->num_threads; i++)
		pthread_join(tp->threads[i], NULL);
}

/* Create a new threadpool. */
os_threadpool_t *create_threadpool(unsigned int num_threads)
{
	os_threadpool_t *tp = NULL;
	int rc;

	tp = malloc(sizeof(*tp));
	DIE(tp == NULL, "malloc");

	list_init(&tp->head);

	// initialize mutex_q threadpool mutex
	pthread_mutex_init(&tp->mutex_q, NULL);
	// initialize mutex_exec_task threadpool mutex
	pthread_mutex_init(&tp->mutex_exec_task, NULL);
	// initialize the threadpool condition variable
	pthread_cond_init(&tp->cond, NULL);
	// set the terminate variable to false
	tp->terminate = 0;

	tp->num_threads = num_threads;
	tp->threads = malloc(num_threads * sizeof(*tp->threads));
	DIE(tp->threads == NULL, "malloc");
	for (unsigned int i = 0; i < num_threads; ++i) {
		rc = pthread_create(&tp->threads[i], NULL, &thread_loop_function, (void *) tp);
		DIE(rc < 0, "pthread_create");
	}

	return tp;
}

/* Destroy a threadpool. Assume all threads have been joined. */
void destroy_threadpool(os_threadpool_t *tp)
{
	os_list_node_t *n, *p;

	pthread_mutex_destroy(&tp->mutex_q);
	pthread_mutex_destroy(&tp->mutex_exec_task);
	pthread_cond_destroy(&tp->cond);

	list_for_each_safe(n, p, &tp->head) {
		list_del(n);
		destroy_task(list_entry(n, os_task_t, list));
	}

	free(tp->threads);
	free(tp);
}
