/*
 * urcu.c
 *
 * Userspace RCU library
 *
 * Copyright (c) 2009 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright (c) 2009 Paul E. McKenney, IBM Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * IBM's contributions to this file may be relicensed under LGPLv2 or later.
 */

#define _BSD_SOURCE
#define _GNU_SOURCE
#define _LGPL_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <poll.h>

#include "urcu/wfcqueue.h"
#include "urcu/map/urcu.h"
#include "urcu/static/urcu.h"
#include "urcu-pointer.h"
#include "urcu/tls-compat.h"

#include "urcu-die.h"
#include "urcu-wait.h"

/* Do not #define _LGPL_SOURCE to ensure we can emit the wrapper symbols */
#undef _LGPL_SOURCE
#include "urcu.h"
#define _LGPL_SOURCE

/*
 * If a reader is really non-cooperative and refuses to commit its
 * rcu_active_readers count to memory (there is no barrier in the reader
 * per-se), kick it after a few loops waiting for it.
 */
#define KICK_READER_LOOPS 10000

/*
 * Active attempts to check for reader Q.S. before calling futex().
 */
#define RCU_QS_ACTIVE_ATTEMPTS 100

/*
 * RCU_MEMBARRIER is only possibly available on Linux.
 */
#if defined(RCU_MEMBARRIER) && defined(__linux__)
#include <syscall.h>
#endif

/* If the headers do not support SYS_membarrier, fall back on RCU_MB */
#ifdef SYS_membarrier
# define membarrier(...)		syscall(SYS_membarrier, __VA_ARGS__)
#else
# define membarrier(...)		-ENOSYS
#endif

#define MEMBARRIER_EXPEDITED		(1 << 0)
#define MEMBARRIER_DELAYED		(1 << 1)
#define MEMBARRIER_QUERY		(1 << 16)

#ifdef RCU_MEMBARRIER
static int init_done;
int rcu_has_sys_membarrier;

void __attribute__((constructor)) rcu_init(void);
#endif

#ifdef RCU_MB
void rcu_init(void)
{
}
#endif

#ifdef RCU_SIGNAL
static int init_done;

void __attribute__((constructor)) rcu_init(void);
void __attribute__((destructor)) rcu_exit(void);
#endif

static pthread_mutex_t rcu_gp_lock = PTHREAD_MUTEX_INITIALIZER;
struct rcu_gp rcu_gp = { .ctr = RCU_GP_COUNT };

/*
 * Written to only by each individual reader. Read by both the reader and the
 * writers.
 */
DEFINE_URCU_TLS(struct rcu_reader, rcu_reader);

#ifdef DEBUG_YIELD
unsigned int rcu_yield_active;
DEFINE_URCU_TLS(unsigned int, rcu_rand_yield);
#endif

static CDS_LIST_HEAD(registry);

/*
 * Queue keeping threads awaiting to wait for a grace period. Contains
 * struct gp_waiters_thread objects.
 */
static DEFINE_URCU_WAIT_QUEUE(gp_waiters);

static void mutex_lock(pthread_mutex_t *mutex)
{
	int ret;

#ifndef DISTRUST_SIGNALS_EXTREME
	ret = pthread_mutex_lock(mutex);
	if (ret)
		urcu_die(ret);
#else /* #ifndef DISTRUST_SIGNALS_EXTREME */
	while ((ret = pthread_mutex_trylock(mutex)) != 0) {
		if (ret != EBUSY && ret != EINTR)
			urcu_die(ret);
		if (CMM_LOAD_SHARED(URCU_TLS(rcu_reader).need_mb)) {
			cmm_smp_mb();
			_CMM_STORE_SHARED(URCU_TLS(rcu_reader).need_mb, 0);
			cmm_smp_mb();
		}
		poll(NULL,0,10);
	}
#endif /* #else #ifndef DISTRUST_SIGNALS_EXTREME */
}

static void mutex_unlock(pthread_mutex_t *mutex)
{
	int ret;

	ret = pthread_mutex_unlock(mutex);
	if (ret)
		urcu_die(ret);
}

#ifdef RCU_MEMBARRIER
static void smp_mb_master(int group)
{
	if (caa_likely(rcu_has_sys_membarrier))
		(void) membarrier(MEMBARRIER_EXPEDITED);
	else
		cmm_smp_mb();
}
#endif

#ifdef RCU_MB
static void smp_mb_master(int group)
{
	cmm_smp_mb();
}
#endif

#ifdef RCU_SIGNAL
static void force_mb_all_readers(void)
{
	struct rcu_reader *index;

	/*
	 * Ask for each threads to execute a cmm_smp_mb() so we can consider the
	 * compiler barriers around rcu read lock as real memory barriers.
	 */
	if (cds_list_empty(&registry))
		return;
	/*
	 * pthread_kill has a cmm_smp_mb(). But beware, we assume it performs
	 * a cache flush on architectures with non-coherent cache. Let's play
	 * safe and don't assume anything : we use cmm_smp_mc() to make sure the
	 * cache flush is enforced.
	 */
	cds_list_for_each_entry(index, &registry, node) {
		CMM_STORE_SHARED(index->need_mb, 1);
		pthread_kill(index->tid, SIGRCU);
	}
	/*
	 * Wait for sighandler (and thus mb()) to execute on every thread.
	 *
	 * Note that the pthread_kill() will never be executed on systems
	 * that correctly deliver signals in a timely manner.  However, it
	 * is not uncommon for kernels to have bugs that can result in
	 * lost or unduly delayed signals.
	 *
	 * If you are seeing the below pthread_kill() executing much at
	 * all, we suggest testing the underlying kernel and filing the
	 * relevant bug report.  For Linux kernels, we recommend getting
	 * the Linux Test Project (LTP).
	 */
	cds_list_for_each_entry(index, &registry, node) {
		while (CMM_LOAD_SHARED(index->need_mb)) {
			pthread_kill(index->tid, SIGRCU);
			poll(NULL, 0, 1);
		}
	}
	cmm_smp_mb();	/* read ->need_mb before ending the barrier */
}

static void smp_mb_master(int group)
{
	force_mb_all_readers();
}
#endif /* #ifdef RCU_SIGNAL */

/*
 * synchronize_rcu() waiting. Single thread.
 */
static void wait_gp(void)
{
	/* Read reader_gp before read futex */
	smp_mb_master(RCU_MB_GROUP);
	if (uatomic_read(&rcu_gp.futex) == -1)
		futex_async(&rcu_gp.futex, FUTEX_WAIT, -1,
		      NULL, NULL, 0);
}

static void wait_for_readers(struct cds_list_head *input_readers,
			struct cds_list_head *cur_snap_readers,
			struct cds_list_head *qsreaders)
{
	int wait_loops = 0;
	struct rcu_reader *index, *tmp;

	/*
	 * Wait for each thread URCU_TLS(rcu_reader).ctr to either
	 * indicate quiescence (not nested), or observe the current
	 * rcu_gp.ctr value.
	 */
	for (;;) {
		wait_loops++;
		if (wait_loops == RCU_QS_ACTIVE_ATTEMPTS) {
			uatomic_dec(&rcu_gp.futex);
			/* Write futex before read reader_gp */
			smp_mb_master(RCU_MB_GROUP);
		}

		cds_list_for_each_entry_safe(index, tmp, input_readers, node) {
			switch (rcu_reader_state(&index->ctr)) {
			case RCU_READER_ACTIVE_CURRENT:
				if (cur_snap_readers) {
					cds_list_move(&index->node,
						cur_snap_readers);
					break;
				}
				/* Fall-through */
			case RCU_READER_INACTIVE:
				cds_list_move(&index->node, qsreaders);
				break;
			case RCU_READER_ACTIVE_OLD:
				/*
				 * Old snapshot. Leaving node in
				 * input_readers will make us busy-loop
				 * until the snapshot becomes current or
				 * the reader becomes inactive.
				 */
				break;
			}
		}

#ifndef HAS_INCOHERENT_CACHES
		if (cds_list_empty(input_readers)) {
			if (wait_loops == RCU_QS_ACTIVE_ATTEMPTS) {
				/* Read reader_gp before write futex */
				smp_mb_master(RCU_MB_GROUP);
				uatomic_set(&rcu_gp.futex, 0);
			}
			break;
		} else {
			if (wait_loops == RCU_QS_ACTIVE_ATTEMPTS)
				wait_gp();
			else
				caa_cpu_relax();
		}
#else /* #ifndef HAS_INCOHERENT_CACHES */
		/*
		 * BUSY-LOOP. Force the reader thread to commit its
		 * URCU_TLS(rcu_reader).ctr update to memory if we wait
		 * for too long.
		 */
		if (cds_list_empty(input_readers)) {
			if (wait_loops == RCU_QS_ACTIVE_ATTEMPTS) {
				/* Read reader_gp before write futex */
				smp_mb_master(RCU_MB_GROUP);
				uatomic_set(&rcu_gp.futex, 0);
			}
			break;
		} else {
			switch (wait_loops) {
			case RCU_QS_ACTIVE_ATTEMPTS:
				wait_gp();
				break; /* only escape switch */
			case KICK_READER_LOOPS:
				smp_mb_master(RCU_MB_GROUP);
				wait_loops = 0;
				break; /* only escape switch */
			default:
				caa_cpu_relax();
			}
		}
#endif /* #else #ifndef HAS_INCOHERENT_CACHES */
	}
}

void synchronize_rcu(void)
{
	CDS_LIST_HEAD(cur_snap_readers);
	CDS_LIST_HEAD(qsreaders);
	DEFINE_URCU_WAIT_NODE(wait, URCU_WAIT_WAITING);
	struct urcu_waiters waiters;

	/*
	 * Add ourself to gp_waiters queue of threads awaiting to wait
	 * for a grace period. Proceed to perform the grace period only
	 * if we are the first thread added into the queue.
	 * The implicit memory barrier before urcu_wait_add()
	 * orders prior memory accesses of threads put into the wait
	 * queue before their insertion into the wait queue.
	 */
	if (urcu_wait_add(&gp_waiters, &wait) != 0) {
		/* Not first in queue: will be awakened by another thread. */
		urcu_adaptative_busy_wait(&wait);
		/* Order following memory accesses after grace period. */
		cmm_smp_mb();
		return;
	}
	/* We won't need to wake ourself up */
	urcu_wait_set_state(&wait, URCU_WAIT_RUNNING);

	mutex_lock(&rcu_gp_lock);

	/*
	 * Move all waiters into our local queue.
	 */
	urcu_move_waiters(&waiters, &gp_waiters);

	if (cds_list_empty(&registry))
		goto out;

	/* All threads should read qparity before accessing data structure
	 * where new ptr points to. Must be done within rcu_gp_lock because it
	 * iterates on reader threads.*/
	/* Write new ptr before changing the qparity */
	smp_mb_master(RCU_MB_GROUP);

	/*
	 * Wait for readers to observe original parity or be quiescent.
	 */
	wait_for_readers(&registry, &cur_snap_readers, &qsreaders);

	/*
	 * Must finish waiting for quiescent state for original parity before
	 * committing next rcu_gp.ctr update to memory. Failure to do so could
	 * result in the writer waiting forever while new readers are always
	 * accessing data (no progress).  Enforce compiler-order of load
	 * URCU_TLS(rcu_reader).ctr before store to rcu_gp.ctr.
	 */
	cmm_barrier();

	/*
	 * Adding a cmm_smp_mb() which is _not_ formally required, but makes the
	 * model easier to understand. It does not have a big performance impact
	 * anyway, given this is the write-side.
	 */
	cmm_smp_mb();

	/* Switch parity: 0 -> 1, 1 -> 0 */
	CMM_STORE_SHARED(rcu_gp.ctr, rcu_gp.ctr ^ RCU_GP_CTR_PHASE);

	/*
	 * Must commit rcu_gp.ctr update to memory before waiting for quiescent
	 * state. Failure to do so could result in the writer waiting forever
	 * while new readers are always accessing data (no progress). Enforce
	 * compiler-order of store to rcu_gp.ctr before load rcu_reader ctr.
	 */
	cmm_barrier();

	/*
	 *
	 * Adding a cmm_smp_mb() which is _not_ formally required, but makes the
	 * model easier to understand. It does not have a big performance impact
	 * anyway, given this is the write-side.
	 */
	cmm_smp_mb();

	/*
	 * Wait for readers to observe new parity or be quiescent.
	 */
	wait_for_readers(&cur_snap_readers, NULL, &qsreaders);

	/*
	 * Put quiescent reader list back into registry.
	 */
	cds_list_splice(&qsreaders, &registry);

	/* Finish waiting for reader threads before letting the old ptr being
	 * freed. Must be done within rcu_gp_lock because it iterates on reader
	 * threads. */
	smp_mb_master(RCU_MB_GROUP);
out:
	mutex_unlock(&rcu_gp_lock);

	/*
	 * Wakeup waiters only after we have completed the grace period
	 * and have ensured the memory barriers at the end of the grace
	 * period have been issued.
	 */
	urcu_wake_all_waiters(&waiters);
}

/*
 * library wrappers to be used by non-LGPL compatible source code.
 */

void rcu_read_lock(void)
{
	_rcu_read_lock();
}

void rcu_read_unlock(void)
{
	_rcu_read_unlock();
}

int rcu_read_ongoing(void)
{
	return _rcu_read_ongoing();
}

void rcu_register_thread(void)
{
	URCU_TLS(rcu_reader).tid = pthread_self();
	assert(URCU_TLS(rcu_reader).need_mb == 0);
	assert(!(URCU_TLS(rcu_reader).ctr & RCU_GP_CTR_NEST_MASK));

	mutex_lock(&rcu_gp_lock);
	rcu_init();	/* In case gcc does not support constructor attribute */
	cds_list_add(&URCU_TLS(rcu_reader).node, &registry);
	mutex_unlock(&rcu_gp_lock);
}

void rcu_unregister_thread(void)
{
	mutex_lock(&rcu_gp_lock);
	cds_list_del(&URCU_TLS(rcu_reader).node);
	mutex_unlock(&rcu_gp_lock);
}

#ifdef RCU_MEMBARRIER
void rcu_init(void)
{
	if (init_done)
		return;
	init_done = 1;
	if (!membarrier(MEMBARRIER_EXPEDITED | MEMBARRIER_QUERY))
		rcu_has_sys_membarrier = 1;
}
#endif

#ifdef RCU_SIGNAL
static void sigrcu_handler(int signo, siginfo_t *siginfo, void *context)
{
	/*
	 * Executing this cmm_smp_mb() is the only purpose of this signal handler.
	 * It punctually promotes cmm_barrier() into cmm_smp_mb() on every thread it is
	 * executed on.
	 */
	cmm_smp_mb();
	_CMM_STORE_SHARED(URCU_TLS(rcu_reader).need_mb, 0);
	cmm_smp_mb();
}

/*
 * rcu_init constructor. Called when the library is linked, but also when
 * reader threads are calling rcu_register_thread().
 * Should only be called by a single thread at a given time. This is ensured by
 * holing the rcu_gp_lock from rcu_register_thread() or by running at library
 * load time, which should not be executed by multiple threads nor concurrently
 * with rcu_register_thread() anyway.
 */
void rcu_init(void)
{
	struct sigaction act;
	int ret;

	if (init_done)
		return;
	init_done = 1;

	act.sa_sigaction = sigrcu_handler;
	act.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&act.sa_mask);
	ret = sigaction(SIGRCU, &act, NULL);
	if (ret)
		urcu_die(errno);
}

void rcu_exit(void)
{
	struct sigaction act;
	int ret;

	ret = sigaction(SIGRCU, NULL, &act);
	if (ret)
		urcu_die(errno);
	assert(act.sa_sigaction == sigrcu_handler);
	assert(cds_list_empty(&registry));
}

#endif /* #ifdef RCU_SIGNAL */

DEFINE_RCU_FLAVOR(rcu_flavor);

#include "urcu-call-rcu-impl.h"
#include "urcu-defer-impl.h"
