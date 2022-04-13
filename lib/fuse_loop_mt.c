/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  Implementation of the multi-threaded FUSE session loop.

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB.
*/

#define _GNU_SOURCE

#include "config.h"
#include "fuse_lowlevel.h"
#include "fuse_misc.h"
#include "fuse_kernel.h"
#include "fuse_i.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <semaphore.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <limits.h>
#include <sched.h>
#include <sys/syscall.h>
#include <stdatomic.h>

/* Environment var controlling the thread stack size */
#define ENVNAME_THREAD_STACK "FUSE_THREAD_STACK"

#define FUSE_LOOP_MT_V2_IDENTIFIER	 INT_MAX - 2
#define FUSE_LOOP_MT_DEF_CLONE_FD	 0
#define FUSE_LOOP_MT_DEF_MAX_THREADS 10
#define FUSE_LOOP_MT_DEF_IDLE_THREADS -1 /* thread destruction is disabled
                                          * by default */
#define FUSE_LOOP_MT_DEF_INIT_STARTS_THREADS 0

struct fuse_worker {
	struct fuse_worker *prev;
	struct fuse_worker *next;
	pthread_t thread_id;

	// We need to include fuse_buf so that we can properly free
	// it when a thread is terminated by pthread_cancel().
	struct fuse_buf fbuf;
	struct fuse_chan *ch;
	struct fuse_mt *mt;
};

struct fuse_mt {
	pthread_mutex_t lock;
	int numworker;
	int numavail;
	struct fuse_session *se;
	struct fuse_worker main;
	sem_t finish;
	int exit;
	int error;
	int clone_fd;
	int max_idle;
	int max_threads;
	int threads_created_at_startup;
	int sched_policy;
	int sched_prio;
};

static struct fuse_chan *fuse_chan_new(int fd)
{
	struct fuse_chan *ch = (struct fuse_chan *) malloc(sizeof(*ch));
	if (ch == NULL) {
		fuse_log(FUSE_LOG_ERR, "fuse: failed to allocate channel\n");
		return NULL;
	}

	memset(ch, 0, sizeof(*ch));
	ch->fd = fd;
	ch->ctr = 1;
	pthread_mutex_init(&ch->lock, NULL);

	return ch;
}

struct fuse_chan *fuse_chan_get(struct fuse_chan *ch)
{
	assert(ch->ctr > 0);
	pthread_mutex_lock(&ch->lock);
	ch->ctr++;
	pthread_mutex_unlock(&ch->lock);

	return ch;
}

void fuse_chan_put(struct fuse_chan *ch)
{
	if (ch == NULL)
		return;
	pthread_mutex_lock(&ch->lock);
	ch->ctr--;
	if (!ch->ctr) {
		pthread_mutex_unlock(&ch->lock);
		close(ch->fd);
		pthread_mutex_destroy(&ch->lock);
		free(ch);
	} else
		pthread_mutex_unlock(&ch->lock);
}

static void list_add_worker(struct fuse_worker *w, struct fuse_worker *next)
{
	struct fuse_worker *prev = next->prev;
	w->next = next;
	w->prev = prev;
	prev->next = w;
	next->prev = w;
}

static void list_del_worker(struct fuse_worker *w)
{
	struct fuse_worker *prev = w->prev;
	struct fuse_worker *next = w->next;
	prev->next = next;
	next->prev = prev;
}

static int fuse_loop_start_thread(struct fuse_mt *mt);

/**
 * Avoid reporting the same error for all threads - only report scheduler
 * getting/setting errors once
 */
static void fuse_sched_log_err(const char *str, int saved_errno)
{
	static _Atomic int err_reported = 0;
	int old = 0;

	if (atomic_compare_exchange_strong(&err_reported, &old ,1))
		fuse_log(FUSE_LOG_ERR, "%s : %s", str, strerror(saved_errno));
}

#if __linux__ && defined SCHED_DEADLINE
static void fuse_set_sched_deadline(int prio)
{
	int res;

	/* defined with #include <linux/sched/types.h>, but that redefines
	 * sched_param and include fails therefore
	 */
	struct sched_attr {
		__u32 size;

		__u32 sched_policy;
		__u64 sched_flags;

		/* SCHED_NORMAL, SCHED_BATCH */
		__s32 sched_nice;

		/* SCHED_FIFO, SCHED_RR */
		__u32 sched_priority;

		/* SCHED_DEADLINE */
		__u64 sched_runtime;
		__u64 sched_deadline;
		__u64 sched_period;

		/* Utilization hints */
		__u32 sched_util_min;
		__u32 sched_util_max;

	};

	struct sched_attr attr = {
		.size = sizeof(attr),
		.sched_policy  = SCHED_DEADLINE,
		.sched_flags = 0,
		.sched_nice = 0, /* make configurable ? */
		.sched_priority = prio,
		/* deadline values - needs to become confiurable */
		.sched_runtime = 10 * 1000 * 1000,
		.sched_deadline = 30 * 1000 * 1000,
		.sched_period = 30 * 1000 * 1000,
	};

	res = syscall(__NR_sched_setattr, 0 /*self */, attr, 0);

	if (res != 0)
		fuse_sched_log_err("Setting deadline syscall failed", errno);
}
#endif

static void set_thread_scheduling(struct fuse_mt *mt)
{
	int res;
	struct sched_param param;
	int policy = mt->sched_policy;
	int prio = mt->sched_prio;

	if (mt->sched_policy == INT_MAX && mt->sched_prio == INT_MAX)
		return; /* no scheduler override */

	/* get the current values */
	res = pthread_getschedparam(pthread_self(), &policy, &param);
	if (res != 0) {
		fuse_sched_log_err("pthread_getschedparam() failed", errno);
		return;
	}
	prio = param.sched_priority;

	if (mt->sched_policy != INT_MAX)
		policy = mt->sched_policy;

	if (mt->sched_prio != INT_MAX)
		prio = mt->sched_prio;

#if __linux__ && defined SCHED_DEADLINE
	if (mt->sched_policy == SCHED_DEADLINE) {
		fuse_set_sched_deadline(prio);
		return;
	}
#endif

	param.sched_priority = mt->sched_prio;

	res = pthread_setschedparam(pthread_self(), mt->sched_policy, &param);
	if (res != 0)
		fuse_sched_log_err("sched_setattr() failed", errno);
}

static void *fuse_do_work(void *data)
{
	struct fuse_worker *w = (struct fuse_worker *) data;
	struct fuse_mt *mt = w->mt;

	set_thread_scheduling(mt);

	while (!fuse_session_exited(mt->se)) {
		int isforget = 0;
		int res;

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		res = fuse_session_receive_buf_int(mt->se, &w->fbuf, w->ch);
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		if (res == -EINTR)
			continue;
		if (res <= 0) {
			if (res < 0) {
				fuse_session_exit(mt->se);
				mt->error = res;
			}
			break;
		}

		pthread_mutex_lock(&mt->lock);
		if (mt->exit) {
			pthread_mutex_unlock(&mt->lock);
			return NULL;
		}

		/*
		 * This disgusting hack is needed so that zillions of threads
		 * are not created on a burst of FORGET messages
		 */
		if (!(w->fbuf.flags & FUSE_BUF_IS_FD)) {
			struct fuse_in_header *in = w->fbuf.mem;

			if (in->opcode == FUSE_FORGET ||
			    in->opcode == FUSE_BATCH_FORGET)
				isforget = 1;
		}

		if (!isforget)
			mt->numavail--;
		if (mt->numavail == 0 && mt->numworker < mt->max_threads)
			fuse_loop_start_thread(mt);
		pthread_mutex_unlock(&mt->lock);

		fuse_session_process_buf_int(mt->se, &w->fbuf, w->ch);

		pthread_mutex_lock(&mt->lock);
		if (!isforget)
			mt->numavail++;

		/* creating and destroying threads is rather expensive - and there is
		 * not much gain from destroying existing threads. It is therefore
		 * discouraged to set max_idle to anything else than -1. If there
		 * is indeed a good reason to destruct threads it should be done
		 * delayed, a moving average might be useful for that.
		 */
		if (mt->max_idle != -1 && mt->numavail > mt->max_idle) {
			if (mt->exit) {
				pthread_mutex_unlock(&mt->lock);
				return NULL;
			}
			list_del_worker(w);
			mt->numavail--;
			mt->numworker--;
			pthread_mutex_unlock(&mt->lock);

			pthread_detach(w->thread_id);
			free(w->fbuf.mem);
			fuse_chan_put(w->ch);
			free(w);
			return NULL;
		}
		pthread_mutex_unlock(&mt->lock);
	}

	sem_post(&mt->finish);

	return NULL;
}

int fuse_start_thread(pthread_t *thread_id, void *(*func)(void *), void *arg)
{
	sigset_t oldset;
	sigset_t newset;
	int res;
	pthread_attr_t attr;
	char *stack_size;

	/* Override default stack size
	 * XXX: This should ideally be a parameter option. It is rather
	 *      well hidden here.
	 */
	pthread_attr_init(&attr);
	stack_size = getenv(ENVNAME_THREAD_STACK);
	if (stack_size && pthread_attr_setstacksize(&attr, atoi(stack_size)))
		fuse_log(FUSE_LOG_ERR, "fuse: invalid stack size: %s\n", stack_size);

	/* Disallow signal reception in worker threads */
	sigemptyset(&newset);
	sigaddset(&newset, SIGTERM);
	sigaddset(&newset, SIGINT);
	sigaddset(&newset, SIGHUP);
	sigaddset(&newset, SIGQUIT);
	pthread_sigmask(SIG_BLOCK, &newset, &oldset);
	res = pthread_create(thread_id, &attr, func, arg);
	pthread_sigmask(SIG_SETMASK, &oldset, NULL);
	pthread_attr_destroy(&attr);
	if (res != 0) {
		fuse_log(FUSE_LOG_ERR, "fuse: error creating thread: %s\n",
			strerror(res));
		return -1;
	}

	return 0;
}

static struct fuse_chan *fuse_clone_chan(struct fuse_mt *mt)
{
	int res;
	int clonefd;
	uint32_t masterfd;
	struct fuse_chan *newch;
	const char *devname = "/dev/fuse";

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
	clonefd = open(devname, O_RDWR | O_CLOEXEC);
	if (clonefd == -1) {
		fuse_log(FUSE_LOG_ERR, "fuse: failed to open %s: %s\n", devname,
			strerror(errno));
		return NULL;
	}
	fcntl(clonefd, F_SETFD, FD_CLOEXEC);

	masterfd = mt->se->fd;
	res = ioctl(clonefd, FUSE_DEV_IOC_CLONE, &masterfd);
	if (res == -1) {
		fuse_log(FUSE_LOG_ERR, "fuse: failed to clone device fd: %s\n",
			strerror(errno));
		close(clonefd);
		return NULL;
	}
	newch = fuse_chan_new(clonefd);
	if (newch == NULL)
		close(clonefd);

	return newch;
}

static int fuse_loop_start_thread(struct fuse_mt *mt)
{
	int res;

	struct fuse_worker *w = malloc(sizeof(struct fuse_worker));
	if (!w) {
		fuse_log(FUSE_LOG_ERR, "fuse: failed to allocate worker structure\n");
		return -1;
	}
	memset(w, 0, sizeof(struct fuse_worker));
	w->fbuf.mem = NULL;
	w->mt = mt;

	w->ch = NULL;
	if (mt->clone_fd) {
		w->ch = fuse_clone_chan(mt);
		if(!w->ch) {
			/* Don't attempt this again */
			fuse_log(FUSE_LOG_ERR, "fuse: trying to continue "
				"without -o clone_fd.\n");
			mt->clone_fd = 0;
		}
	}

	res = fuse_start_thread(&w->thread_id, fuse_do_work, w);
	if (res == -1) {
		fuse_chan_put(w->ch);
		free(w);
		return -1;
	}
	list_add_worker(w, &mt->main);
	mt->numavail ++;
	mt->numworker ++;

	return 0;
}

static void fuse_join_worker(struct fuse_mt *mt, struct fuse_worker *w)
{
	pthread_join(w->thread_id, NULL);
	pthread_mutex_lock(&mt->lock);
	list_del_worker(w);
	pthread_mutex_unlock(&mt->lock);
	free(w->fbuf.mem);
	fuse_chan_put(w->ch);
	free(w);
}

int fuse_session_loop_mt_311(struct fuse_session *se, struct fuse_loop_config *config);
FUSE_SYMVER("fuse_session_loop_mt_311", "fuse_session_loop_mt@@FUSE_3.11")
int fuse_session_loop_mt_311(struct fuse_session *se, struct fuse_loop_config *config)
{
int err;
	struct fuse_mt mt;
	struct fuse_worker *w;
	int created_config = 0;
	int n_start_threads = 1;

	if (config) {
		err = fuse_loop_cfg_verify(config);
		if (err)
			return err;
	} else {
		/* The caller does not care about parameters - use the default */
		config = fuse_loop_cfg_create();
		created_config = 1;
	}

	if (config->threads_created_at_startup)
		n_start_threads = config->max_threads;


	se->loop_config = config;

	memset(&mt, 0, sizeof(struct fuse_mt));
	mt.se = se;
	mt.clone_fd = config->clone_fd;
	mt.error = 0;
	mt.numworker = 0;
	mt.numavail = 0;
	mt.max_idle = config->max_idle_threads;
	mt.max_threads = config->max_threads;
	mt.threads_created_at_startup = config->threads_created_at_startup;
	mt.main.thread_id = pthread_self();
	mt.main.prev = mt.main.next = &mt.main;
	mt.sched_policy = config->thread_policy;
	mt.sched_prio   = config->thread_prio;
	sem_init(&mt.finish, 0, 0);
	pthread_mutex_init(&mt.lock, NULL);

	pthread_mutex_lock(&mt.lock);
	for (int i = 0; i < n_start_threads; i++) {
		err = fuse_loop_start_thread(&mt);
		if (err)
			break;
	}
	pthread_mutex_unlock(&mt.lock);
	if (!err) {
		/* sem_wait() is interruptible */
		while (!fuse_session_exited(se))
			sem_wait(&mt.finish);

		pthread_mutex_lock(&mt.lock);
		for (w = mt.main.next; w != &mt.main; w = w->next)
			pthread_cancel(w->thread_id);
		mt.exit = 1;
		pthread_mutex_unlock(&mt.lock);

		while (mt.main.next != &mt.main)
			fuse_join_worker(&mt, mt.main.next);

		err = mt.error;
	}

	pthread_mutex_destroy(&mt.lock);
	sem_destroy(&mt.finish);
	if(se->error != 0)
		err = se->error;
	fuse_session_reset(se);

	if (created_config) {
		fuse_loop_cfg_destroy(config);
		config = NULL;
	}

	return err;
}

int fuse_session_loop_mt_32(struct fuse_session *se, struct fuse_loop_config_v1 *config_v1);
FUSE_SYMVER("fuse_session_loop_mt_32", "fuse_session_loop_mt@FUSE_3.2")
int fuse_session_loop_mt_32(struct fuse_session *se, struct fuse_loop_config_v1 *config_v1)
{
	int err;
	struct fuse_loop_config *config = NULL;

	if (config_v1 != NULL) {
		/* convert the given v1 config */
		config = fuse_loop_cfg_create();
		if (config == NULL)
			return ENOMEM;

		fuse_loop_cfg_convert(config, config_v1);
	}

	err = fuse_session_loop_mt_311(se, config);

	fuse_loop_cfg_destroy(config);

	return err;
}


int fuse_session_loop_mt_31(struct fuse_session *se, int clone_fd);
FUSE_SYMVER("fuse_session_loop_mt_31", "fuse_session_loop_mt@FUSE_3.0")
int fuse_session_loop_mt_31(struct fuse_session *se, int clone_fd)
{
	struct fuse_loop_config *config = fuse_loop_cfg_create();
	if (clone_fd > 0)
		 fuse_loop_cfg_set_clone_fd(config, clone_fd);
	return fuse_session_loop_mt_311(se, config);
}

struct fuse_loop_config *fuse_loop_cfg_create(void)
{
	struct fuse_loop_config *config = calloc(1, sizeof(*config));
	if (config == NULL)
		return NULL;

	config->version_id       = FUSE_LOOP_MT_V2_IDENTIFIER;
	config->max_idle_threads = FUSE_LOOP_MT_DEF_IDLE_THREADS;
	config->max_threads      = FUSE_LOOP_MT_DEF_MAX_THREADS;
	config->clone_fd         = FUSE_LOOP_MT_DEF_CLONE_FD;
	config->threads_created_at_startup = FUSE_LOOP_MT_DEF_INIT_STARTS_THREADS;

	return config;
}

void fuse_loop_cfg_destroy(struct fuse_loop_config *config)
{
	free(config);
}

int fuse_loop_cfg_verify(struct fuse_loop_config *config)
{
	if (config->version_id != FUSE_LOOP_MT_V2_IDENTIFIER)
		return -EINVAL;

	if (config->threads_created_at_startup && config->max_idle_threads != -1) {
		fuse_log(FUSE_LOG_ERR,
				"Invalid configuration, setting max_idle_threads and "
				"threads_created_at_startup is contracticting.");
		return -EINVAL;
	}

	return 0;
}

void fuse_loop_cfg_convert(struct fuse_loop_config *config,
			   struct fuse_loop_config_v1 *v1_conf)
{
	fuse_loop_cfg_set_idle_threads(config, v1_conf->max_idle_threads);

	fuse_loop_cfg_set_clone_fd(config, v1_conf->clone_fd);
}

void fuse_loop_cfg_set_idle_threads(struct fuse_loop_config *config,
				    unsigned int value)
{
	config->max_idle_threads = value;

	/* No point to pre-create all threads if there is max-idle is configured,
	 * thread creation is then done dynamically.
	 * When threads are created at startup time this also might be set into
	 * the kernel to let it know how many threads there are and to let
	 * it manage these threads - dynamic thread creation with max-idle is
	 * conflicting for that.
	 */
	config->threads_created_at_startup = 0;

}

void fuse_loop_cfg_set_max_threads(struct fuse_loop_config *config,
				   unsigned int value)
{
	config->max_threads = value;
}

void fuse_loop_cfg_set_clone_fd(struct fuse_loop_config *config,
				unsigned int value)
{
	config->clone_fd = value;
}

void fuse_loop_cfg_set_dynamic_thread_startup(struct fuse_loop_config *config,
					      unsigned int value)
{
	/* dynamic and create-at-at-start are the opposite of each other */
	config->threads_created_at_startup = value > 0 ? 0 : 1;
}

static int fuse_loop_cfg_sched_policy_cmp(const char * const sched,
					  const char * const option)
{
	return strncmp(sched, option, strlen(sched));
}

static int fuse_loop_cfg_sched_policy_str2val(const char * const policy)
{
	if (fuse_loop_cfg_sched_policy_cmp("SCHED_FIFO", policy))
		return SCHED_FIFO;

	if (fuse_loop_cfg_sched_policy_cmp("SCHED_RR", policy))
		return SCHED_RR;

	if (fuse_loop_cfg_sched_policy_cmp("SCHED_OTHER", policy))
		return SCHED_OTHER;

	/* GNU values below */

#ifdef SCHED_BATCH
	if (fuse_loop_cfg_sched_policy_cmp("SCHED_BATCH", policy))
		return SCHED_BATCH;
#endif

#ifdef SCHED_ISO
	if (fuse_loop_cfg_sched_policy_cmp("SCHED_ISO", policy))
		return SCHED_ISO;
#endif

#ifdef SCHED_IDLE
	if (fuse_loop_cfg_sched_policy_cmp("SCHED_IDLE", policy))
		return SCHED_IDLE;
#endif

#ifdef SCHED_IDLE
	if (fuse_loop_cfg_sched_policy_cmp("SCHED_DEADLINE", policy))
		return SCHED_DEADLINE;
#endif

	return -EINVAL;
}

/* TODO: Add a function to get the policy as numeric value? */
int fuse_loop_cfg_set_sched_policy(struct fuse_loop_config *config,
				   const char * const policy)
{
	int value = fuse_loop_cfg_sched_policy_str2val(policy);
	if (value < 0)
		return value;

	config->thread_policy = value;

	const size_t len = sizeof(config->thread_policy_str);
	memset(config->thread_policy_str, 0, len); /* ensure terminated string */
	strncpy(config->thread_policy_str, policy, len - 1);

	return 0;
}

void fuse_loop_cfg_set_sched_prio(struct fuse_loop_config *config, int prio)
{
	config->thread_prio = prio;
}
