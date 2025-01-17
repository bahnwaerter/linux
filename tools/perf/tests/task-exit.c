// SPDX-License-Identifier: GPL-2.0
#include "evlist.h"
#include "evsel.h"
#include "thread_map.h"
#include "cpumap.h"
#include "tests.h"

#include <errno.h>
#include <signal.h>
#include <perf/evlist.h>

static int exited;
static int nr_exit;

static void sig_handler(int sig __maybe_unused)
{
	exited = 1;
}

/*
 * perf_evlist__prepare_workload will send a SIGUSR1 if the fork fails, since
 * we asked by setting its exec_error to this handler.
 */
static void workload_exec_failed_signal(int signo __maybe_unused,
					siginfo_t *info __maybe_unused,
					void *ucontext __maybe_unused)
{
	exited	= 1;
	nr_exit = -1;
}

/*
 * This test will start a workload that does nothing then it checks
 * if the number of exit event reported by the kernel is 1 or not
 * in order to check the kernel returns correct number of event.
 */
int test__task_exit(struct test *test __maybe_unused, int subtest __maybe_unused)
{
	int err = -1;
	union perf_event *event;
	struct evsel *evsel;
	struct evlist *evlist;
	struct target target = {
		.uid		= UINT_MAX,
		.uses_mmap	= true,
	};
	const char *argv[] = { "true", NULL };
	char sbuf[STRERR_BUFSIZE];
	struct perf_cpu_map *cpus;
	struct perf_thread_map *threads;
	struct perf_mmap *md;

	signal(SIGCHLD, sig_handler);

	evlist = perf_evlist__new_default();
	if (evlist == NULL) {
		pr_debug("perf_evlist__new_default\n");
		return -1;
	}

	/*
	 * Create maps of threads and cpus to monitor. In this case
	 * we start with all threads and cpus (-1, -1) but then in
	 * perf_evlist__prepare_workload we'll fill in the only thread
	 * we're monitoring, the one forked there.
	 */
	cpus = perf_cpu_map__dummy_new();
	threads = thread_map__new_by_tid(-1);
	if (!cpus || !threads) {
		err = -ENOMEM;
		pr_debug("Not enough memory to create thread/cpu maps\n");
		goto out_free_maps;
	}

	perf_evlist__set_maps(&evlist->core, cpus, threads);

	cpus	= NULL;
	threads = NULL;

	err = perf_evlist__prepare_workload(evlist, &target, argv, false,
					    workload_exec_failed_signal);
	if (err < 0) {
		pr_debug("Couldn't run the workload!\n");
		goto out_delete_evlist;
	}

	evsel = perf_evlist__first(evlist);
	evsel->core.attr.task = 1;
#ifdef __s390x__
	evsel->core.attr.sample_freq = 1000000;
#else
	evsel->core.attr.sample_freq = 1;
#endif
	evsel->core.attr.inherit = 0;
	evsel->core.attr.watermark = 0;
	evsel->core.attr.wakeup_events = 1;
	evsel->core.attr.exclude_kernel = 1;

	err = evlist__open(evlist);
	if (err < 0) {
		pr_debug("Couldn't open the evlist: %s\n",
			 str_error_r(-err, sbuf, sizeof(sbuf)));
		goto out_delete_evlist;
	}

	if (perf_evlist__mmap(evlist, 128) < 0) {
		pr_debug("failed to mmap events: %d (%s)\n", errno,
			 str_error_r(errno, sbuf, sizeof(sbuf)));
		goto out_delete_evlist;
	}

	perf_evlist__start_workload(evlist);

retry:
	md = &evlist->mmap[0];
	if (perf_mmap__read_init(md) < 0)
		goto out_init;

	while ((event = perf_mmap__read_event(md)) != NULL) {
		if (event->header.type == PERF_RECORD_EXIT)
			nr_exit++;

		perf_mmap__consume(md);
	}
	perf_mmap__read_done(md);

out_init:
	if (!exited || !nr_exit) {
		perf_evlist__poll(evlist, -1);
		goto retry;
	}

	if (nr_exit != 1) {
		pr_debug("received %d EXIT records\n", nr_exit);
		err = -1;
	}

out_free_maps:
	perf_cpu_map__put(cpus);
	perf_thread_map__put(threads);
out_delete_evlist:
	evlist__delete(evlist);
	return err;
}
