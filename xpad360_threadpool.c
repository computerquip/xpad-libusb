#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct worker_data {
	unsigned char data[32];
	void *ctx;
	pthread_t thread;
	pthread_cond_t condition;
	pthread_mutex_t mutex;
	int ready;
};

static void(*process)(unsigned char*, void*);
static struct worker_data *workers;
static int num_workers;

static int parse_cpucount_sysfs()
{
	const char online_filepath[] = "/sys/devices/system/cpu/online";
	int prev = -1; /* Not 0 as 0 is a valid value. */
	int num_cores = 0;
 
	/* file_num_online contains the number of cores online (not all possible) across various CPUs. */
	FILE* file = fopen(online_filepath, "r");
 
	if (!file) {
		printf("You got problems son. Most likely, you're missing an important scheduling module.\n");
		return 0;
	}
 
	/* The following is similar to what's found in glibc for fetching number of online processors.  */
 
	for (;;) {
		char sep;
		int cpu;
		int n = fscanf(file, "%u%c", &cpu, &sep);
 
		if (n <= 0)
			break;
		
		if (n == 1) /* There was no seperator... meaning eol.*/
			sep = '\n';
 
		if (prev >= 0) { /* This is the second number in a range, for sure higher than the last. */
			for (int i = prev; i <= cpu; ++i) {
				++num_cores;
			}
 
			prev = -1;
		} else if (sep == '-') {
			prev = cpu;
		} else {
			++num_cores;
		}
 
		if (sep == '\n') break;
	}
	
	fclose(file);
	return num_cores;
}

static void *worker_func(void *_data) 
{
	struct worker_data *data = _data;
	pthread_mutex_lock(&data->mutex);
	
	for (;;) {
		while (!data->ready) {
			pthread_cond_wait(&data->condition, &data->mutex);
		}
		
		process(data->data, data->ctx);
		data->ready = 0;
	}
	
	return 0;
}

int pool_init(void(*proc)(unsigned char*, void*)) 
{
	num_workers = parse_cpucount_sysfs();
	
	if (!num_workers) return -1;
	workers = calloc(num_workers, sizeof(struct worker_data));
	
	if (!workers) return -2;
	
	process = proc;
	
	for (int i = 0; i < num_workers; ++i) {
		pthread_cond_init(&workers[i].condition, NULL);
		pthread_mutex_init(&workers[i].mutex, NULL);
		
		pthread_create(
			&workers[i].thread, NULL, 
			worker_func, &workers[i]);
	}
		
	return 0;
}

void pool_queue_work(unsigned char *data, void *ctx)
{
	static int worker;
	
	++worker;
	worker = worker % num_workers;
	memcpy(workers[worker].data, data, 32);
	workers[worker].ctx = ctx;
	workers[worker].ready = 1;
	pthread_cond_signal(&workers[worker].condition);
}

void pool_destroy()
{
	free(workers);
}



