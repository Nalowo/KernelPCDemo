// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/string.h>

#include "pc_demo.h"

/* --- helpers shared with main.c --- */

int pc_params_validate(unsigned int fifo_size, unsigned int num_events,
                       unsigned int interval_us, unsigned int consumer_type) {
  if (fifo_size < PC_FIFO_SIZE_MIN || fifo_size > PC_FIFO_SIZE_MAX ||
      (fifo_size & (fifo_size - 1)) != 0) {
    pr_err("fifo_size=%u: must be a power of two in [%u..%u]\n", fifo_size,
           PC_FIFO_SIZE_MIN, PC_FIFO_SIZE_MAX);
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
void pc_stats_reset(struct pc_ctx *ctx) {
  atomic_set(&ctx->produced, 0);
  atomic_set(&ctx->dropped, 0);
  atomic_set(&ctx->consumed, 0);
  ctx->sum = 0;
  ctx->last_value = 0;
}

/* Parse a "1"-style trigger written into a write-only parameter. */
static int pc_parse_trigger(const char *val) {
  unsigned int v;

  if (!val)
    return PC_INVALID;
  if (kstrtouint(strim((char *)val), 0, &v) || v != 1)
    return PC_INVALID;
  return PC_OK;
}

/* --- run --- */
static int pc_param_run_set(const char *val, const struct kernel_param *kp) {
  struct pc_ctx *ctx = &pc_ctx_global;
  int ret;

  ret = pc_parse_trigger(val);
  if (ret)
    return ret;

  if (atomic_cmpxchg(&ctx->running, 0, 1) != 0)
    return PC_BUSY;

  kfifo_reset(&ctx->fifo);
  pc_stats_reset(ctx);

  ret = pc_consumer_setup(ctx);
  if (ret != PC_OK)
    goto out_unlock;

  pc_producer_start(ctx);

  u64 timeout_ms;
  unsigned long timeout;
  long wait_ret;

  timeout_ms = div_u64((u64)ctx->num_events * ctx->interval_us, USEC_PER_MSEC);
  timeout = msecs_to_jiffies(timeout_ms + timeout_ms / 2 + 100);

  wait_ret = wait_event_interruptible_timeout(
      ctx->done, pc_total_events(ctx) >= ctx->num_events, timeout);

  if (wait_ret < 0)
    ret = wait_ret;
  else if (wait_ret == 0)
    ret = PC_TIMEOUT;
  else
    ret = PC_OK;

  pc_producer_stop(ctx);
  pc_consumer_teardown(ctx);

out_unlock:
  ctx->last_run_result = ret;
  atomic_set(&ctx->running, 0);
  return ret;
}

static const struct kernel_param_ops pc_run_ops = {
    .set = pc_param_run_set,
};
module_param_cb(run, &pc_run_ops, NULL, 0200);
MODULE_PARM_DESC(run, "write 1 to run one producer/consumer test (blocking)");

/* --- result --- */
static int pc_param_result_get(char *buffer, const struct kernel_param *kp) {
  struct pc_ctx *ctx = &pc_ctx_global;

  const int produced = atomic_read(&ctx->produced);
  const int consumed = atomic_read(&ctx->consumed);
  int len = scnprintf(buffer, PAGE_SIZE,
                      "produced=%d consumed=%d dropped=%d consumer=%s",
                      produced, consumed, atomic_read(&ctx->dropped),
                      pc_consumer_name(ctx->active_type));

  if (ctx->last_run_result == PC_OK)
    len += scnprintf(buffer + len, PAGE_SIZE - len, " ok");
  else
    len += scnprintf(buffer + len, PAGE_SIZE - len, " error=%d",
                     ctx->last_run_result);

  if (consumed < produced)
    len += scnprintf(buffer + len, PAGE_SIZE - len, " warn: lost=%d",
                     produced - consumed);

  return len + scnprintf(buffer + len, PAGE_SIZE - len, "\n");
}

static const struct kernel_param_ops pc_result_ops = {
    .get = pc_param_result_get,
};
module_param_cb(result, &pc_result_ops, NULL, 0444);
MODULE_PARM_DESC(result, "result of the last run");

/* --- stats --- */
static int pc_param_stats_get(char *buffer, const struct kernel_param *kp) {
  struct pc_ctx *ctx = &pc_ctx_global;

  mutex_lock(&ctx->stats_lock);
  const u64 sum = ctx->sum;
  const unsigned int last = ctx->last_value;
  mutex_unlock(&ctx->stats_lock);

  const int consumed = atomic_read(&ctx->consumed);
  return scnprintf(
      buffer, PAGE_SIZE,
      "produced=%d consumed=%d dropped=%d sum=%llu last=%u avg=%llu\n",
      atomic_read(&ctx->produced), consumed, atomic_read(&ctx->dropped), sum,
      last, consumed ? div_u64(sum, consumed) : 0);
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
                                      const struct kernel_param *kp) {
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
                                      const struct kernel_param *kp) {
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
static int pc_param_reset_set(const char *val, const struct kernel_param *kp) {
  struct pc_ctx *ctx = &pc_ctx_global;
  int ret;

  ret = pc_parse_trigger(val);
  if (ret)
    return ret;

  if (atomic_cmpxchg(&ctx->running, 0, 1) != 0)
    return PC_BUSY;

  pc_producer_stop(ctx);
  pc_consumer_teardown(ctx);
  kfifo_reset(&ctx->fifo);
  pc_stats_reset(ctx);
  atomic_set(&ctx->running, 0);
  return PC_OK;
}

static const struct kernel_param_ops pc_reset_ops = {
    .set = pc_param_reset_set,
};
module_param_cb(reset, &pc_reset_ops, NULL, 0200);
MODULE_PARM_DESC(reset, "write 1 to clear the queue and all counters");
