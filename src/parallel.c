// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <time.h>
#include <stdatomic.h>

#include "os_graph.h"
#include "os_threadpool.h"
#include "log/log.h"
#include "utils.h"

#define NUM_THREADS		4

static int sum;
static os_graph_t *graph;
static os_threadpool_t *tp;

// mutex for synchronizing the graph access
pthread_mutex_t graph_mutex;
// atomic variable to count the number of processable nodes remaining
atomic_int node_cnt;

// graph task argument containing in ->node_idx the idx of the node
// that has to be processed
typedef struct graph_arg {
	unsigned int node_idx;
} graph_arg;

static void process_node(unsigned int idx);

// function executed by a thread at any given time
void execute_task(void *arg)
{
	// cast arg to graph_arg * in order to get the information
	graph_arg *func_arg = (graph_arg *)arg;
	unsigned int idx = func_arg->node_idx;

	// lock the task execution mutex
	// update the sum and set the node state to DONE
	pthread_mutex_lock(&tp->mutex_exec_task);
	sum += graph->nodes[idx]->info;
	graph->visited[idx] = DONE;
	node_cnt--;
	// unlock the task execution mutex
	pthread_mutex_unlock(&tp->mutex_exec_task);

	// iterate through the nodes' neighbours and process them
	unsigned int n_cnt = graph->nodes[idx]->num_neighbours;

	for (unsigned int i = 0; i < n_cnt; i++)
		if (graph->visited[graph->nodes[idx]->neighbours[i]] == NOT_VISITED)
			process_node(graph->nodes[idx]->neighbours[i]);
}

static void process_node(unsigned int idx)
{
	// lock the mutex and set the current to node to status PROCESSING
	// if the current node wasn't yet visited
	pthread_mutex_lock(&graph_mutex);
	if (graph->visited[idx] != NOT_VISITED) {
		pthread_mutex_unlock(&graph_mutex);
		return;
	}
	graph->visited[idx] = PROCESSING;
	pthread_mutex_unlock(&graph_mutex);

	// define a graph task argument and create a new task
	graph_arg *arg = (graph_arg *)malloc(sizeof(graph_arg));

	DIE(arg == NULL, "malloc");

	// set the information
	arg->node_idx = idx;
	os_task_t *new_task = create_task(execute_task, (void *)arg, free);

	// enqueue the task
	enqueue_task(tp, new_task);
}

int main(int argc, char *argv[])
{
	FILE *input_file;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s input_file\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	input_file = fopen(argv[1], "r");
	DIE(input_file == NULL, "fopen");

	graph = create_graph_from_file(input_file);

	// initialize graph mutex
	pthread_mutex_init(&graph_mutex, NULL);
	// initialize the number of processable nodes
	node_cnt = graph->num_nodes;

	// remove the isolated nodes, except for node 0 from the counter
	for (unsigned int i = 1; i < graph->num_nodes; i++)
		if (graph->nodes[i]->num_neighbours == 0)
			node_cnt--;

	tp = create_threadpool(NUM_THREADS);
	process_node(0);
	wait_for_completion(tp);
	destroy_threadpool(tp);
	pthread_mutex_destroy(&graph_mutex);
	printf("%d", sum);

	return 0;
}
