/*
 * ias.c - the Interference-Aware Scheduler (IAS) policy
 */

#include <stdlib.h>
#include <string.h>
#include <float.h>

#include <base/stddef.h>
#include <base/log.h>

#include "defs.h"
#include "sched.h"
#include "ksched.h"
#include "ias.h"

/* a list of all processes */
LIST_HEAD(all_procs);
/* a bitmap of all available cores that are currently idle */
static DEFINE_BITMAP(ias_idle_cores, NCPU);
/* a bitmap of all cores that tasks have reserved */
static DEFINE_BITMAP(ias_reserved_cores, NCPU);
/* used for calculating a unique index number */
static struct ias_data *ias_procs[IAS_NPROC];
/* used for calculating a unique index number */
static unsigned int ias_procs_nr;
/* the current process running on each core */
struct ias_data *cores[NCPU];
/* the generation number (to detect context switches on a core) */
uint64_t ias_gen[NCPU];
/* the current time in microseconds */
uint64_t now_us;

#ifdef IAS_DEBUG
static int owners[NCPU];
#endif

static void ias_cleanup_core(unsigned int core)
{
	struct ias_data *sd = cores[core];

	if (sd) {
		sd->loc_last_us[core] = now_us;
		sd->threads_active--;
	}
	cores[core] = NULL;
}

static int ias_attach(struct proc *p, struct sched_spec *sched_cfg)
{
	struct ias_data *sd;
	int i, core, sib;

	/* validate parameters */
	if (ias_procs_nr >= IAS_NPROC)
		return -ENOENT;
	if (!cfg.noht && sched_cfg->guaranteed_cores % 2 != 0)
		return -EINVAL;

	/* allocate and initialize process state */
	sd = malloc(sizeof(*sd));
	if (!sd)
		return -ENOMEM;
	memset(sd, 0, sizeof(*sd));
	sd->p = p;
	sd->threads_guaranteed = sched_cfg->guaranteed_cores;
	sd->threads_max = sched_cfg->max_cores;
	sd->threads_limit = sched_cfg->max_cores;
	sd->is_lc = sched_cfg->priority == SCHED_PRIO_LC;
	if (sd->is_lc)
		sd->ht_punish_us = sched_cfg->ht_punish_us;
	if (sd->ht_punish_us)
		sd->ht_punish_tsc_inv = 1.0 / (float)(sd->ht_punish_us * cycles_per_us);
	sd->qdelay_us = sched_cfg->qdelay_us;
	sd->threads_active = 0;
	p->policy_data = (unsigned long)sd;
	list_add(&all_procs, &sd->all_link);

	/* reserve priority cores */
	i = sd->threads_guaranteed;
	while (i > 0) {
		core = bitmap_find_next_cleared(ias_reserved_cores, NCPU, 0);
		if (core == NCPU)
			goto fail_reserve;

		bitmap_set(sd->reserved_cores, core);
		bitmap_set(ias_reserved_cores, core);
		i--;
#ifdef IAS_DEBUG
		owners[core] = p->pid;
#endif

		if (cfg.noht)
			continue;

		sib = sched_siblings[core];
		bitmap_set(sd->reserved_cores, sib);
		bitmap_set(ias_reserved_cores, sib);
		i--;
#ifdef IAS_DEBUG
		owners[sib] = p->pid;
#endif
	}

	/* reserve a unique index */
	for (i = 0; i < ias_procs_nr; i++) {
		if (ias_procs[i] == NULL) {
			sd->idx = i;
			ias_procs[i] = sd;
			return 0;
		}
	}
	sd->idx = ias_procs_nr;
	ias_procs[ias_procs_nr++] = sd;
	return 0;

fail_reserve:
	list_del_from(&all_procs, &sd->all_link);
	bitmap_xor(ias_reserved_cores, ias_reserved_cores,
		   sd->reserved_cores, NCPU);
	free(sd);
	return -ENOENT;
}

static void ias_detach(struct proc *p)
{
	struct ias_data *sd = (struct ias_data *)p->policy_data;
	int i;

	ias_procs[sd->idx] = NULL;
	if (sd->idx == ias_procs_nr)
		ias_procs_nr--;

	list_del_from(&all_procs, &sd->all_link);
	bitmap_xor(ias_reserved_cores, ias_reserved_cores,
		   sd->reserved_cores, NCPU);

	for (i = 0; i < NCPU; i++) {
		if (cores[i] == sd)
			cores[i] = NULL;
	}

	free(sd);
}

static int ias_run_kthread_on_core(struct ias_data *sd, unsigned int core)
{
	int ret;

	/*
	 * WARNING: A kthread could be stuck waiting to detach and thus
	 * temporarily unavailable even if it is no longer assigned to a core.
	 * We check with the scheduler layer here to catch such a race
	 * condition.  In this sense, applications can get new cores more
	 * quickly if they yield promptly when requested.
	 */
	if (sched_threads_avail(sd->p) == 0)
		return -EBUSY;

	ret = sched_run_on_core(sd->p, core);
	if (ret)
		return ret;

	ias_cleanup_core(core);
	cores[core] = sd;
	ias_gen[core]++;
	bitmap_clear(ias_idle_cores, core);
	sd->threads_active++;
	return 0;
}

/**
 * ias_idle_placeholder_on_core - idle the core but count it as active for @sd
 * @sd: the task to account the core to
 * @core: the core to make idle
 *
 * Returns 0 if successful.
 */
int ias_idle_placeholder_on_core(struct ias_data *sd, unsigned int core)
{
	int ret;

	if (!bitmap_test(ias_idle_cores, core)) {
		ret = sched_idle_on_core(0, core);
		if (ret)
			return -EBUSY;
	}

	ias_cleanup_core(core);
	cores[core] = sd;
	ias_gen[core]++;
	bitmap_set(ias_idle_cores, core);
	sd->threads_active++;
	return 0;
}

/**
 * ias_idle_on_core - evict the current process and idle the core
 * @core: the core to make idle
 *
 * Returns 0 if successful.
 */
int ias_idle_on_core(unsigned int core)
{
	int ret;

	if (!bitmap_test(ias_idle_cores, core)) {
		ret = sched_idle_on_core(0, core);
		if (ret)
			return -EBUSY;
	}

	ias_cleanup_core(core);
	cores[core] = NULL;
	ias_gen[core]++;
	bitmap_set(ias_idle_cores, core);
	return 0;
}

static bool ias_can_preempt_core(struct ias_data *sd, unsigned int core)
{
	struct ias_data *cur = cores[core];

	/* any task can take idle cores */
	if (cur == NULL)
		return true;

	/* can't preempt if the current task is the same task */
	if (sd == cur)
		return false;
	/* can't preempt if the current task reserved this core */
	if (bitmap_test(cur->reserved_cores, core))
		return false;
	/* BE tasks can't preempt LC tasks */
	if (cur->is_lc && !sd->is_lc)
		return false;
	/* can't preempt a task that will be using <= burst cores */
	if (cur->is_lc == sd->is_lc &&
	    (int)cur->threads_active - (int)cur->threads_guaranteed <=
	    (int)sd->threads_active - (int)sd->threads_guaranteed + 1) {
		return false;
	}

	/* all conditions passed, can preempt */
	return true;
}

/**
 * ias_core_score - estimates the amount of cache locality in the core's cache
 * @sd: the process to check
 * @core: the core to check
 * @now_us: the current time in microseconds
 *
 * Returns a locality score, higher is better.
 */
static float ias_locality_score(struct ias_data *sd, unsigned int core)
{
	uint64_t delta_us;
	unsigned int sib = sched_siblings[core];

	/* if the sibling is already running the process, locality is perfect */
	if (cores[sib] == sd)
		return 1.0f;

	delta_us = now_us - MAX(sd->loc_last_us[core],
				sd->loc_last_us[sib]);
	if (delta_us >= IAS_LOC_EVICTED_US)
		return 0.0f;
	return (float)(IAS_LOC_EVICTED_US - delta_us) / IAS_LOC_EVICTED_US;
}

static float ias_core_score(struct ias_data *sd, unsigned int core)
{
	float score;
	struct ias_data *sib_task = cores[sched_siblings[core]];

	score = ias_locality_score(sd, core);

	if (bitmap_test(sd->reserved_cores, core))
		score += 7.0f;

	if (!cfg.ias_prefer_selfpair && sib_task != sd) {
		score += 3.0f;
		if (sib_task == NULL && cores[core] == NULL)
			score += 2.0f;
	}

	/* bias in favor of cores whose siblings are most under HT budget */
	score += 1.0 - MIN(1.0, ias_ht_budget_used(sched_siblings[core]));

	return score;
}

static unsigned int ias_choose_core(struct ias_data *sd)
{
	unsigned int core, best_core = NCPU, tmp;
	float score, best_score = -1.0f;

	sched_for_each_allowed_core(core, tmp) {
		/* check if this process can preempt this core */
		if (!ias_can_preempt_core(sd, core))
			continue;

		/* try to estimate how good this core is for the process */
		score = ias_core_score(sd, core);
		if (score > best_score) {
			best_score = score;
			best_core = core;
		}
	}

	return best_core;
}

/**
 * ias_can_add_kthread - checks if a core can be added to a process
 * @sd: the process to check
 * @ignore_ht_punish_cores: exclude cores that may be pending HT punishment
 * *
 * Returns true if a core can be added.
 */
bool ias_can_add_kthread(struct ias_data *sd, bool ignore_ht_punish_cores)
{
	unsigned int core, tmp;

	sched_for_each_allowed_core(core, tmp) {
		if (!ias_can_preempt_core(sd, core))
			continue;
		if (ignore_ht_punish_cores &&
		    ias_ht_budget_used(sched_siblings[core]) >= 1.0f)
			continue;
		return true;
	}

	return false;
}

int ias_add_kthread(struct ias_data *sd)
{
	unsigned int core;

	/* check if we're constrained by the thread limit */
	if (sd->threads_active >= sd->threads_limit) {
		core = ias_ht_relinquish_core(sd);
		if (core != NCPU)
			goto done;
		return -ENOENT;
	}

	/* choose the best core to run the process on */
	core = ias_choose_core(sd);
	if (core == NCPU) {
		core = ias_ht_relinquish_core(sd);
		if (core == NCPU)
			return -ENOENT;
	}

done:
	/* finally, wake up the thread on the chosen core */
	return ias_run_kthread_on_core(sd, core);
}

static int ias_notify_core_needed(struct proc *p)
{
	struct ias_data *sd = (struct ias_data *)p->policy_data;
	return ias_add_kthread(sd);
}

static void ias_notify_congested(struct proc *p, bool busy, uint64_t delay)
{
	struct ias_data *sd = (struct ias_data *)p->policy_data;
	int ret;
	bool congested;

	/* detect congestion */
	congested = sd->qdelay_us == 0 ? busy : delay >= sd->qdelay_us;

	/* stop if there is no congestion */
	if (!congested) {
		sd->is_congested = false;
		return;
	}

	/* try to add an additional core right away */
	ret = ias_add_kthread(sd);
	if (!ret)
		return;

	/* otherwise mark the process as congested, cores can be added later */
	sd->is_congested = true;
}

static int ias_kthread_score(struct ias_data *sd, int core)
{
	bool has_resv = bitmap_test(sd->reserved_cores, core);
	bool is_lc = sd->is_lc;
	int burst_score;

	burst_score = (int)sd->threads_active - (int)sd->threads_guaranteed;
	burst_score = MAX(burst_score, 0);
	burst_score = NCPU - burst_score;

	return has_resv * NCPU * 4 + is_lc * NCPU * 2 + burst_score;
}

static struct ias_data *ias_choose_kthread(unsigned int core)
{
	struct ias_data *sd, *best_sd = NULL;
	int score, best_score = -1;

	ias_for_each_proc(sd) {
		/* only congested processes need more cores */
		if (!sd->is_congested)
			continue;
		/* check if we're constrained by the thread limit */
		if (sd->threads_active >= sd->threads_limit)
			continue;

		/* try to estimate how good this core is for the process */
		score = ias_kthread_score(sd, core);
		if (score > best_score) {
			best_score = score;
			best_sd = sd;
		}
	}

	return best_sd;
}

/**
 * ias_add_kthread_on_core - pick a process and wake it on a core
 * @core: the core to schedule on
 *
 * Returns 0 if successful.
 */
int ias_add_kthread_on_core(unsigned int core)
{
	struct ias_data *sd;
	int ret;

	sd = ias_choose_kthread(core);
	if (unlikely(!sd))
		return -ENOENT;

	ret = ias_run_kthread_on_core(sd, core);
	if (unlikely(ret))
		return ret;

	return 0;
}

#ifdef IAS_DEBUG
static void ias_print_debug_info(void)
{
	bool printed[NCPU];
	struct ias_data *sd;
	int core, sib;
	uint64_t now = rdtsc();

	ias_for_each_proc(sd) {
		log_info("PID %d: %s%s ACTIVE %d, LIMIT %d, MAX %d",
			 sd->p->pid,
			 sd->is_congested ? "C" : "_",
			 sd->is_bwlimited ? "B" : "_",
			 sd->threads_active, sd->threads_limit,
			 sd->threads_max);
	}

	log_info("tsc %lu bw_cur %f bw_punish %ld bw_relax %ld bw_sample_failures %ld "
		 "bw_sample_aborts %ld",
		 now, ias_bw_estimate * ias_bw_estimate_multiplier, ias_bw_punish_count,
		 ias_bw_relax_count, ias_bw_sample_failures, ias_bw_sample_aborts);
	log_info("tsc %lu ht_punish %ld ht_relax %ld", now, ias_ht_punish_count,
		 ias_ht_relax_count);

	memset(printed, 0, sizeof(printed));
	bitmap_for_each_set(sched_allowed_cores, NCPU, core) {
		if (printed[core]) {
			continue;
		}
		sib = sched_siblings[core];
		printed[core] = printed[sib] = true;
		int pid0 = cores[core] ? cores[core]->p->pid : -1;
		int pid1 = cores[sib] ? cores[sib]->p->pid : -1;
		log_info("core %d, %d (owner: %d): pid %d, %d", core, sib,
			 owners[core], pid0, pid1);
	}
}
#endif

static void ias_sched_poll(uint64_t now, int idle_cnt, bitmap_ptr_t idle)
{
	static uint64_t last_bw_us, last_ht_us;
#ifdef IAS_DEBUG
	static uint64_t debug_ts = 0;
#endif
	unsigned int core;

	now_us = now;

	/* mark cores idle */
	if (idle_cnt != 0)
		bitmap_or(ias_idle_cores, ias_idle_cores, idle, NCPU);

	/* try to allocate any idle cores */
	bitmap_for_each_set(ias_idle_cores, NCPU, core) {
		if (bitmap_test(ias_ht_punished_cores, core))
			continue;
		if (cores[core] != NULL)
			cores[core]->is_congested = false;
		ias_cleanup_core(core);
		ias_add_kthread_on_core(core);
	}

	/* try to run the bandwidth controller */
	if (!cfg.nobw && now - last_bw_us >= IAS_BW_INTERVAL_US) {
		last_bw_us = now;
		ias_bw_poll();
	}

	/* try to run the hyperthread controller */
	if (!cfg.noht && now - last_ht_us >= IAS_HT_INTERVAL_US) {
		last_ht_us = now;
		ias_ht_poll();
	}

#ifdef IAS_DEBUG
	if (now - debug_ts >= IAS_DEBUG_PRINT_US) {
		debug_ts = now;
		ias_print_debug_info();
	}
#endif
}

struct sched_ops ias_ops = {
	.proc_attach		= ias_attach,
	.proc_detach		= ias_detach,
	.notify_congested	= ias_notify_congested,
	.notify_core_needed	= ias_notify_core_needed,
	.sched_poll		= ias_sched_poll,
};

/**
 * ias_init - initializes the ias scheduler policy
 *
 * Returns 0 (always successful).
 */
int ias_init(void)
{
	bitmap_init(ias_reserved_cores, true, NCPU);
	bitmap_xor(ias_reserved_cores, ias_reserved_cores, sched_allowed_cores,
		   NCPU);

	return ias_bw_init();
}
