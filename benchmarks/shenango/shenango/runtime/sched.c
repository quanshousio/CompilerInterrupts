/*
 * sched.c - a scheduler for user-level threads
 */

#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <base/stddef.h>
#include <base/lock.h>
#include <base/list.h>
#include <base/hash.h>
#include <base/limits.h>
#include <base/tcache.h>
#include <base/slab.h>
#include <base/log.h>
#include <runtime/sync.h>
#include <runtime/thread.h>

#include "defs.h"

/* the currently running thread, or NULL if in runtime code */
__thread thread_t *__self;
/* a pointer to the top of the per-kthread (TLS) runtime stack */
static __thread void *runtime_stack;
/* a pointer to the bottom of the per-kthread (TLS) runtime stack */
static __thread void *runtime_stack_base;

/* Flag to prevent watchdog from running */
bool disable_watchdog;

/* fast allocation of struct thread */
static struct slab thread_slab;
static struct tcache *thread_tcache;
static DEFINE_PERTHREAD(struct tcache_perthread, thread_pt);

/* used to track cycle usage in scheduler */
static __thread uint64_t last_tsc;
/* used to force timer and network processing after a timeout */
static __thread uint64_t last_watchdog_tsc;

/**
 * In inc/runtime/thread.h, this function is declared inline (rather than static
 * inline) so that it is accessible to the Rust bindings. As a result, it must
 * also appear in a source file to avoid linker errors.
 */
thread_t *thread_self(void);

#ifdef DBG
int sched_parked1 = 0;
int sched_preempt_needed = 0;
#endif

/**
 * jmp_thread - runs a thread, popping its trap frame
 * @th: the thread to run
 *
 * This function restores the state of the thread and switches from the runtime
 * stack to the thread's stack. Runtime state is not saved.
 */
static __noreturn void jmp_thread(thread_t *th)
{
	__self = th;
	assert(th->state == THREAD_STATE_RUNNABLE);
	th->state = THREAD_STATE_RUNNING;
	__jmp_thread(&th->tf);
}

/**
 * jmp_runtime - saves the current trap frame and jumps to a function in the
 *               runtime
 * @fn: the runtime function to call
 * @arg: an argument to pass to the runtime function
 *
 * WARNING: Only threads can call this function.
 *
 * This function saves state of the running thread and switches to the runtime
 * stack, making it safe to run the thread elsewhere.
 */
static void jmp_runtime(runtime_fn_t fn, unsigned long arg)
{
	preempt_disable();
	assert(thread_self() != NULL);
	__jmp_runtime(&thread_self()->tf, fn, runtime_stack, arg);
}

/**
 * jmp_runtime_nosave - jumps to a function in the runtime without saving the
 *			caller's state
 * @fn: the runtime function to call
 * @arg: an argument to pass to the runtime function
 */
static __noreturn void jmp_runtime_nosave(runtime_fn_t fn, unsigned long arg)
{
	preempt_disable();
	__jmp_runtime_nosave(fn, runtime_stack, arg);
}

static void drain_overflow(struct kthread *l)
{
	thread_t *th;

	assert_spin_lock_held(&l->lock);

	while (l->rq_head - l->rq_tail < RUNTIME_RQ_SIZE) {
		th = list_pop(&l->rq_overflow, thread_t, link);
		if (!th)
			break;
		l->rq[l->rq_head++ % RUNTIME_RQ_SIZE] = th;
		l->q_ptrs->rq_head++;
	}
}

static bool steal_work(struct kthread *l, struct kthread *r)
{
	thread_t *th;
	uint32_t i, avail, rq_tail;

	assert_spin_lock_held(&l->lock);
	assert(l->rq_head == 0 && l->rq_tail == 0);

	if (!spin_try_lock(&r->lock))
		return false;

	/* harmless race condition */
	if (unlikely(r->detached)) {
		spin_unlock(&r->lock);
		return false;
	}

	/* try to steal directly from the runqueue */
	avail = load_acquire(&r->rq_head) - r->rq_tail;
	if (avail) {
		/* steal half the tasks */
		avail = div_up(avail, 2);
		assert(avail <= div_up(RUNTIME_RQ_SIZE, 2));
		rq_tail = r->rq_tail;
		for (i = 0; i < avail; i++)
			l->rq[i] = r->rq[rq_tail++ % RUNTIME_RQ_SIZE];
		store_release(&r->rq_tail, rq_tail);
		r->q_ptrs->rq_tail += avail;
		spin_unlock(&r->lock);

		l->rq_head = avail;
		l->q_ptrs->rq_head += avail;
		STAT(THREADS_STOLEN) += avail;
		return true;
	}

	/* check for overflow tasks */
	th = list_pop(&r->rq_overflow, thread_t, link);
	if (th)
		goto done;

	/* check for softirqs */
	th = softirq_run_thread(r, RUNTIME_SOFTIRQ_BUDGET);
	if (th) {
		STAT(SOFTIRQS_STOLEN)++;
		goto done;
	}

done:
	/* either enqueue the stolen work or detach the kthread */
	if (th) {
		l->rq[l->rq_head++] = th;
		l->q_ptrs->rq_head++;
		STAT(THREADS_STOLEN)++;
	} else if (r->parked) {
		kthread_detach(r);

		/*
		 * handle the case where kthread_detach -> rcu_detach leads to a
		 * thread being added to the runqueue (but not returned above)
		 */
		if (l->rq_head != l->rq_tail)
			th = l->rq[l->rq_head];
	}

	spin_unlock(&r->lock);
	return th != NULL;
}

static __noinline struct thread *do_watchdog(struct kthread *l)
{
	thread_t *th;

	assert_spin_lock_held(&l->lock);

	/* then check the network queues */
	th = softirq_run_thread(l, RUNTIME_SOFTIRQ_BUDGET);
	if (th) {
		STAT(SOFTIRQS_LOCAL)++;
		return th;
	}

	return NULL;
}

/* the main scheduler routine, decides what to run next */
static __noreturn void schedule(void)
{
	struct kthread *r = NULL, *l = myk();
	uint64_t start_tsc, end_tsc;
	thread_t *th = NULL;
	unsigned int last_nrks;
	unsigned int iters = 0;
	int i, sibling;

	/* detect misuse of preempt disable */
	BUG_ON((preempt_cnt & ~PREEMPT_NOT_PENDING) != 1);

	/* update entry stat counters */
	STAT(RESCHEDULES)++;
	start_tsc = rdtsc();
	STAT(PROGRAM_CYCLES) += start_tsc - last_tsc;

	/* mark the end of the RCU quiescent period */
	rcu_recurrent();
	/* drain overflow packets */
	net_recurrent();

	__self = NULL;
	spin_lock(&l->lock);

	assert(l->parked == false);
	assert(l->detached == false);

	/* if it's been too long, run the softirq handler */
	if (unlikely(!disable_watchdog && start_tsc - last_watchdog_tsc >
	             cycles_per_us * RUNTIME_WATCHDOG_US)) {
		last_watchdog_tsc = start_tsc;
		th = do_watchdog(l);
		if (th)
			goto done;
	}

	/* move overflow tasks into the runqueue */
	if (unlikely(!list_empty(&l->rq_overflow)))
		drain_overflow(l);

again:
	/* first try the local runqueue */
	if (l->rq_head != l->rq_tail)
		goto done;

	/* reset the local runqueue since it's empty */
	l->rq_head = l->rq_tail = 0;

	/* then check for local softirqs */
	th = softirq_run_thread(l, RUNTIME_SOFTIRQ_BUDGET);
	if (th) {
		STAT(SOFTIRQS_LOCAL)++;
		goto done;
	}

	last_nrks = load_acquire(&nrks);

	/* then try to steal from a sibling kthread */
	sibling = cpu_map[l->curr_cpu].sibling_core;
	r = cpu_map[sibling].recent_kthread;
	if (r && r != l && steal_work(l, r))
		goto done;

	/* then try to steal from a random kthread */
	r = ks[rand_crc32c((uintptr_t)l) % last_nrks];
	if (r != l && steal_work(l, r))
		goto done;

	/* finally try to steal from every kthread */
	for (i = 0; i < last_nrks; i++)
		if (ks[i] != l && steal_work(l, ks[i]))
			goto done;

	/* check for RCU reclamation */
	if (unlikely(load_acquire(&rcu_gen) != l->rcu_gen)) {
		spin_unlock(&l->lock);
		__rcu_recurrent(l);
		spin_lock(&l->lock);
		goto again;
	}

	/* keep trying to find work until the polling timeout expires */
	if (!preempt_needed() &&
	    (++iters < RUNTIME_SCHED_POLL_ITERS ||
	     rdtsc() - start_tsc < cycles_per_us * RUNTIME_SCHED_MIN_POLL_US))
		goto again;

	/* did not find anything to run, park this kthread */
	STAT(SCHED_CYCLES) += rdtsc() - start_tsc;
	/* we may have got a preempt signal before voluntarily yielding */
#ifdef DBG
  if(preempt_needed())
    sched_preempt_needed++;
  sched_parked1++;
#endif
	kthread_park(!preempt_needed());
	start_tsc = rdtsc();

	goto again;

done:
	/* pop off a thread and run it */
	if (!th) {
		assert(l->rq_head != l->rq_tail);
		th = l->rq[l->rq_tail++ % RUNTIME_RQ_SIZE];
		l->q_ptrs->rq_tail++;
	}

	/* move overflow tasks into the runqueue */
	if (unlikely(!list_empty(&l->rq_overflow)))
		drain_overflow(l);

	spin_unlock(&l->lock);

	/* update exit stat counters */
	end_tsc = rdtsc();
	STAT(SCHED_CYCLES) += end_tsc - start_tsc;
	last_tsc = end_tsc;

	jmp_thread(th);
}

/**
 * join_kthread - detaches a kthread immediately (rather than through stealing)
 * @k: the kthread to detach
 *
 * Can and must be called from thread context.
 */
void join_kthread(struct kthread *k)
{
	thread_t *waketh;
	struct list_head tmp;

	//log_info_ratelimited("join_kthread() %p", k);

	list_head_init(&tmp);

	/* if the lock can't be acquired, the kthread is unparking */
	if (!spin_try_lock_np(&k->lock))
		return;

	/* harmless race conditions */
	if (k->detached || !k->parked || k == myk()) {
		spin_unlock_np(&k->lock);
		return;
	}

	/* drain the runqueue */
	for (; k->rq_tail < k->rq_head; k->rq_tail++) {
		list_add_tail(&tmp, &k->rq[k->rq_tail % RUNTIME_RQ_SIZE]->link);
		k->q_ptrs->rq_tail++;
	}
	k->rq_head = k->rq_tail = 0;

	/* drain the overflow runqueue */
	list_append_list(&tmp, &k->rq_overflow);

	/* detach the kthread */
	kthread_detach(k);
	spin_unlock_np(&k->lock);

	/* re-wake all the runnable threads belonging to the detached kthread */
	while (true) {
		waketh = list_pop(&tmp, thread_t, link);
		if (!waketh)
			break;
		waketh->state = THREAD_STATE_SLEEPING;
		thread_ready(waketh);
	}
}

/**
 * immediately park each kthread when it first starts up, only schedule it once
 * the iokernel has granted it a core
 */
static __noreturn void schedule_start(void)
{
	/* force kthread parking (iokernel assumes all kthreads are parked
	 * initially) */
	kthread_wait_to_attach();

	schedule();
}

static void thread_finish_park_and_unlock(unsigned long data)
{
	thread_t *myth = thread_self();
	spinlock_t *lock = (spinlock_t *)data;

	assert(myth->state == THREAD_STATE_RUNNING);
	myth->state = THREAD_STATE_SLEEPING;
	spin_unlock(lock);

	schedule();
}

static void thread_finish_park_and_unlock_np(unsigned long data)
{
	thread_t *myth = thread_self();
	spinlock_t *lock = (spinlock_t *)data;

	assert(myth->state == THREAD_STATE_RUNNING);
	myth->state = THREAD_STATE_SLEEPING;
	spin_unlock_np(lock);

	schedule();
}

/**
 * thread_park_and_unlock - puts a thread to sleep and unlocks when finished
 * @l: this lock will be released when the thread state is fully saved
 */
void thread_park_and_unlock(spinlock_t *l)
{
	/* this will switch from the thread stack to the runtime stack */
	jmp_runtime(thread_finish_park_and_unlock, (unsigned long)l);
}

/**
 * thread_park_and_unlock_np - puts a thread to sleep and unlocks when finished
 * and re-enables preemption
 * @l: this lock will be released when the thread state is fully saved
 */
void thread_park_and_unlock_np(spinlock_t *l)
{
	/* this will switch from the thread stack to the runtime stack */
	jmp_runtime(thread_finish_park_and_unlock_np, (unsigned long)l);
}


/**
 * thread_ready - marks a thread as a runnable
 * @th: the thread to mark runnable
 *
 * This function can only be called when @th is sleeping.
 */
void thread_ready(thread_t *th)
{
	struct kthread *k;
	uint32_t rq_tail;

	assert(th->state == THREAD_STATE_SLEEPING);
	th->state = THREAD_STATE_RUNNABLE;

	k = getk();
	rq_tail = load_acquire(&k->rq_tail);
	if (unlikely(k->rq_head - rq_tail >= RUNTIME_RQ_SIZE)) {
		assert(k->rq_head - rq_tail == RUNTIME_RQ_SIZE);
		spin_lock(&k->lock);
		list_add_tail(&k->rq_overflow, &th->link);
		spin_unlock(&k->lock);
		putk();
		return;
	}

	k->rq[k->rq_head % RUNTIME_RQ_SIZE] = th;
	store_release(&k->rq_head, k->rq_head + 1);
	k->q_ptrs->rq_head++;
	putk();
}

static void thread_finish_yield_kthread(unsigned long data)
{
	struct kthread *k = myk();
	thread_t *myth = thread_self();

	assert(myth->state == THREAD_STATE_RUNNING);
	myth->state = THREAD_STATE_SLEEPING;
	thread_ready(myth);

	STAT(PROGRAM_CYCLES) += rdtsc() - last_tsc;

	spin_lock(&k->lock);
	clear_preempt_needed();
	kthread_park(false);
	spin_unlock(&k->lock);

	last_tsc = rdtsc();

	schedule();
}

/**
 * thread_yield_kthread - yields the running thread and immediately parks
 */
void thread_yield_kthread(void)
{
	/* this will switch from the thread stack to the runtime stack */
	jmp_runtime(thread_finish_yield_kthread, 0);
}

static void thread_finish_yield(unsigned long data)
{
	thread_t *myth = thread_self();

	assert(myth->state == THREAD_STATE_RUNNING);
	myth->state = THREAD_STATE_SLEEPING;
	thread_ready(myth);

	schedule();
}

/**
 * thread_yield - yields the currently running thread
 *
 * Yielding will give other threads a chance to run.
 */
void thread_yield(void)
{
	/* check for softirqs */
	softirq_run(RUNTIME_SOFTIRQ_BUDGET);

	/* this will switch from the thread stack to the runtime stack */
	jmp_runtime(thread_finish_yield, 0);
}

static __always_inline thread_t *__thread_create(void)
{
	struct thread *th;
	struct stack *s;

	preempt_disable();
	th = tcache_alloc(&perthread_get(thread_pt));
	if (unlikely(!th)) {
		preempt_enable();
		return NULL;
	}

	s = stack_alloc();
	if (unlikely(!s)) {
		tcache_free(&perthread_get(thread_pt), th);
		preempt_enable();
		return NULL;
	}
	preempt_enable();

	th->stack = s;
	th->state = THREAD_STATE_SLEEPING;
	th->main_thread = false;

	return th;
}

/**
 * thread_create - creates a new thread
 * @fn: a function pointer to the starting method of the thread
 * @arg: an argument passed to @fn
 *
 * Returns 0 if successful, otherwise -ENOMEM if out of memory.
 */
thread_t *thread_create(thread_fn_t fn, void *arg)
{
	thread_t *th = __thread_create();
	if (unlikely(!th))
		return NULL;

	th->tf.rsp = stack_init_to_rsp(th->stack, thread_exit);
	th->tf.rdi = (uint64_t)arg;
	th->tf.rbp = (uint64_t)0; /* just in case base pointers are enabled */
	th->tf.rip = (uint64_t)fn;
	return th;
}

/**
 * thread_create_with_buf - creates a new thread with space for a buffer on the
 * stack
 * @fn: a function pointer to the starting method of the thread
 * @buf: a pointer to the stack allocated buffer (passed as arg too)
 * @buf_len: the size of the stack allocated buffer
 *
 * Returns 0 if successful, otherwise -ENOMEM if out of memory.
 */
thread_t *thread_create_with_buf(thread_fn_t fn, void **buf, size_t buf_len)
{
	void *ptr;
	thread_t *th = __thread_create();
	if (unlikely(!th))
		return NULL;

	th->tf.rsp = stack_init_to_rsp_with_buf(th->stack, &ptr,
						buf_len, thread_exit);
	th->tf.rdi = (uint64_t)ptr;
	th->tf.rbp = (uint64_t)0; /* just in case base pointers are enabled */
	th->tf.rip = (uint64_t)fn;
	*buf = ptr;

	return th;
}

/**
 * thread_spawn - creates and launches a new thread
 * @fn: a function pointer to the starting method of the thread
 * @arg: an argument passed to @fn
 *
 * Returns 0 if successful, otherwise -ENOMEM if out of memory.
 */
int thread_spawn(thread_fn_t fn, void *arg)
{
	thread_t *th = thread_create(fn, arg);
	if (unlikely(!th))
		return -ENOMEM;
	thread_ready(th);
	return 0;
}

/**
 * thread_spawn_main - creates and launches the main thread
 * @fn: a function pointer to the starting method of the thread
 * @arg: an argument passed to @fn
 *
 * WARNING: Only can be called once.
 *
 * Returns 0 if successful, otherwise -ENOMEM if out of memory.
 */
int thread_spawn_main(thread_fn_t fn, void *arg)
{
	static bool called = false;
	thread_t *th;

	BUG_ON(called);
	called = true;

	th = thread_create(fn, arg);
	if (!th)
		return -ENOMEM;
	th->main_thread = true;
	thread_ready(th);
	return 0;
}

static void thread_finish_exit(unsigned long data)
{
	struct thread *th = thread_self();

	/* if the main thread dies, kill the whole program */
	if (unlikely(th->main_thread))
		init_shutdown(EXIT_SUCCESS);
	stack_free(th->stack);
	tcache_free(&perthread_get(thread_pt), th);

	schedule();
}

/**
 * thread_exit - terminates a thread
 */
void thread_exit(void)
{
	/* can't free the stack we're currently using, so switch */
	jmp_runtime_nosave(thread_finish_exit, 0);
}

/**
 * sched_start - used only to enter the runtime the first time
 */
void sched_start(void)
{
	last_tsc = rdtsc();
	jmp_runtime_nosave((runtime_fn_t)schedule_start, 0);
}

static void runtime_top_of_stack(void)
{
	panic("a thread returned to the top of the stack");
}

/**
 * sched_init_thread - initializes per-thread state for the scheduler
 *
 * Returns 0 if successful, or -ENOMEM if out of memory.
 */
int sched_init_thread(void)
{
	struct stack *s;

	tcache_init_perthread(thread_tcache, &perthread_get(thread_pt));

	s = stack_alloc();
	if (!s)
		return -ENOMEM;

	runtime_stack_base = (void *)s;
	runtime_stack = (void *)stack_init_to_rsp(s, runtime_top_of_stack); 

	return 0;
}

/**
 * sched_init - initializes the scheduler subsystem
 *
 * Returns 0 if successful, or -ENOMEM if out of memory.
 */
int sched_init(void)
{
	int ret, i, j, siblings;

	/*
	 * set up allocation routines for threads
	 */
	ret = slab_create(&thread_slab, "runtime_threads",
			  sizeof(struct thread), 0);
	if (ret)
		return ret;

	thread_tcache = slab_create_tcache(&thread_slab,
					   TCACHE_DEFAULT_MAG_SIZE);
	if (!thread_tcache) {
		slab_destroy(&thread_slab);
		return -ENOMEM;
	}

	for (i = 0; i < cpu_count; i++) {
		siblings = 0;
		bitmap_for_each_set(cpu_info_tbl[i].thread_siblings_mask,
				    cpu_count, j) {
			if (i == j)
				continue;
			BUG_ON(siblings++);
			cpu_map[i].sibling_core = j;
		}
	}

	return 0;
}
