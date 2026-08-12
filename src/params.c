// SPDX-License-Identifier: GPL-2.0
/*
 * module_param_cb interface exported under
 * /sys/module/kernel_pc_demo/parameters/:
 *
 *   run            (w)  echo 1 > run            -- run one test, blocks until done
 *   result         (r)  cat result              -- "produced=.. consumed=.. dropped=.. consumer=.. ok"
 *   stats          (r)  cat stats               -- "... sum=.. last=.. avg=.."
 *   consumer_type  (rw) echo 1 > consumer_type  -- 0 tasklet, 1 workqueue
 *   reset          (w)  echo 1 > reset          -- drop queue and counters
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/string.h>

#include "pc_demo.h"

/* --- helpers shared with main.c --- */

int pc_params_validate(unsigned int fifo_size, unsigned int num_events,
		       unsigned int interval_us, unsigned int consumer_type)
{
	if (fifo_size < PC_FIFO_SIZE_MIN || fifo_size > PC_FIFO_SIZE_MAX ||
	    (fifo_size & (fifo_size - 1)) != 0) {
		pr_err("fifo_size=%u: must be a power of two in [%u..%u]\n",
		       fifo_size, PC_FIFO_SIZE_MIN, PC_FIFO_SIZE_MAX);
		return PC_INVALID;
	}
	if (num_events < PC_NUM_EVENTS_MIN || num_events > PC_NUM_EVENTS_MAX) {
		pr_err("num_events=%u: must be in [%u..%u]\n", num_events,
		       PC_NUM_EVENTS_MIN, PC_NUM_EVENTS_MAX);
		return PC_INVALID;
	}
	if (interval_us < PC_INTERVAL_US_MIN || interval_us > PC_INTERVAL_US_MAX) {
		pr_err("interval_us=%u: must be in [%u..%u]\n", interval_us,
		       PC_INTERVAL_US_MIN, PC_INTERVAL_US_MAX);
		return PC_INVALID;
	}
	if (consumer_type != PC_CONSUMER_TASKLET &&
	    consumer_type != PC_CONSUMER_WORKQUEUE) {
		pr_err("consumer_type=%u: must be 0 (tasklet) or 1 (workqueue)\n",
		       consumer_type);
		return PC_INVALID;
	}
	return PC_OK;
}

/* Zero every counter. Callers must make sure no bottom-half is running. */
void pc_stats_reset(struct pc_ctx *ctx)
{
	atomic_set(&ctx->produced, 0);
	atomic_set(&ctx->dropped, 0);
	atomic_set(&ctx->consumed, 0);
	ctx->sum = 0;
	ctx->last_value = 0;
}

/* Parse a "1"-style trigger written into a write-only parameter. */
static int pc_parse_trigger(const char *val)
{
	unsigned int v;

	if (!val)
		return PC_INVALID;
	if (kstrtouint(strim((char *)val), 0, &v) || v != 1)
		return PC_INVALID;
	return PC_OK;
}

/* --- run --- */

/*
 * TODO:
 *   1. atomic_cmpxchg(&ctx->running, 0, 1) != 0  -> return PC_BUSY;
 *   2. kfifo_reset(&ctx->fifo); pc_stats_reset(ctx);
 *   3. pc_consumer_setup(ctx)   -- tasklet_init / create_singlethread_workqueue
 *   4. pc_producer_start(ctx)
 *   5. wait_event_timeout(ctx->done,
 *                         pc_total_events(ctx) >= ctx->num_events,
 *                         timeout)  -- timeout from num_events * interval_us
 *                                      plus slack; 0 -> PC_TIMEOUT
 *   6. pc_producer_stop(ctx); pc_consumer_teardown(ctx) (drains the last items)
 *   7. ctx->last_run_result = ...; atomic_set(&ctx->running, 0);
 */
static int pc_param_run_set(const char *val, const struct kernel_param *kp)
{
	struct pc_ctx *ctx = &pc_ctx_global;
	int ret;

	ret = pc_parse_trigger(val);
	if (ret)
		return ret;

	(void)ctx;
	return PC_OK;
}

static const struct kernel_param_ops pc_run_ops = {
	.set = pc_param_run_set,
};
module_param_cb(run, &pc_run_ops, NULL, 0200);
MODULE_PARM_DESC(run, "write 1 to run one producer/consumer test (blocking)");

/* --- result --- */

/*
 * TODO: "produced=%d consumed=%d dropped=%d consumer=%s ok"
 *       when consumed < produced append " warn: lost=%d";
 *       report ctx->last_run_result when the last run failed.
 */
static int pc_param_result_get(char *buffer, const struct kernel_param *kp)
{
	struct pc_ctx *ctx = &pc_ctx_global;

	return scnprintf(buffer, PAGE_SIZE, "produced=%d consumed=%d dropped=%d consumer=%s ok\n",
			 atomic_read(&ctx->produced), atomic_read(&ctx->consumed),
			 atomic_read(&ctx->dropped),
			 pc_consumer_name(ctx->consumer_type));
}

static const struct kernel_param_ops pc_result_ops = {
	.get = pc_param_result_get,
};
module_param_cb(result, &pc_result_ops, NULL, 0444);
MODULE_PARM_DESC(result, "result of the last run");

/* --- stats --- */

/*
 * TODO: "produced=%d consumed=%d dropped=%d sum=%llu last=%u avg=%llu",
 *       avg = consumed ? div_u64(sum, consumed) : 0;
 *       read sum/last_value under ctx->stats_lock.
 */
static int pc_param_stats_get(char *buffer, const struct kernel_param *kp)
{
	struct pc_ctx *ctx = &pc_ctx_global;

	return scnprintf(buffer, PAGE_SIZE,
			 "produced=%d consumed=%d dropped=%d sum=%llu last=%u avg=%u\n",
			 atomic_read(&ctx->produced), atomic_read(&ctx->consumed),
			 atomic_read(&ctx->dropped), ctx->sum, ctx->last_value, 0u);
}

static const struct kernel_param_ops pc_stats_ops = {
	.get = pc_param_stats_get,
};
module_param_cb(stats, &pc_stats_ops, NULL, 0444);
MODULE_PARM_DESC(stats, "produced/consumed/dropped/sum/last/avg");

/* --- consumer_type --- */

/*
 * Also the load-time handler for consumer_type=N: module parameters are parsed
 * before pc_init(), so this runs with an all-zero context, which is fine --
 * it only touches ctx->consumer_type and ctx->running.
 */
static int pc_param_consumer_type_set(const char *val,
				      const struct kernel_param *kp)
{
	struct pc_ctx *ctx = &pc_ctx_global;
	unsigned int type;

	if (!val || kstrtouint(strim((char *)val), 0, &type))
		return PC_INVALID;
	if (type != PC_CONSUMER_TASKLET && type != PC_CONSUMER_WORKQUEUE)
		return PC_INVALID;
	if (atomic_read(&ctx->running))
		return PC_BUSY;

	ctx->consumer_type = type;
	return PC_OK;
}

static int pc_param_consumer_type_get(char *buffer,
				      const struct kernel_param *kp)
{
	struct pc_ctx *ctx = &pc_ctx_global;

	return scnprintf(buffer, PAGE_SIZE, "%u (%s)\n", ctx->consumer_type,
			 pc_consumer_name(ctx->consumer_type));
}

static const struct kernel_param_ops pc_consumer_type_ops = {
	.set = pc_param_consumer_type_set,
	.get = pc_param_consumer_type_get,
};
module_param_cb(consumer_type, &pc_consumer_type_ops, NULL, 0644);
MODULE_PARM_DESC(consumer_type, "consumer: 0 = tasklet, 1 = workqueue");

/* --- reset --- */

/*
 * TODO:
 *   1. atomic_cmpxchg(&ctx->running, 0, 1) != 0 -> PC_BUSY;
 *   2. pc_producer_stop(ctx)      -- hrtimer_cancel
 *   3. pc_consumer_teardown(ctx)  -- tasklet_kill / flush + destroy_workqueue
 *   4. kfifo_reset(&ctx->fifo); pc_stats_reset(ctx);
 *   5. atomic_set(&ctx->running, 0);
 */
static int pc_param_reset_set(const char *val, const struct kernel_param *kp)
{
	struct pc_ctx *ctx = &pc_ctx_global;
	int ret;

	ret = pc_parse_trigger(val);
	if (ret)
		return ret;

	(void)ctx;
	return PC_OK;
}

static const struct kernel_param_ops pc_reset_ops = {
	.set = pc_param_reset_set,
};
module_param_cb(reset, &pc_reset_ops, NULL, 0200);
MODULE_PARM_DESC(reset, "write 1 to clear the queue and all counters");
