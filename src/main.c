// SPDX-License-Identifier: GPL-2.0
/*
 * kernel_pc_demo -- producer/consumer: tasklet vs workqueue.
 *
 * The producer is an hrtimer callback (top-half) that pushes random values
 * into a kfifo without ever sleeping; a full fifo means the event is dropped.
 * The consumer drains the fifo either from a tasklet (atomic context, cannot
 * sleep) or from a workqueue (process context, may sleep) -- selectable at
 * runtime via /sys/module/kernel_pc_demo/parameters/consumer_type.
 *
 * This file owns the module lifecycle: parameter validation, the global
 * context and the kfifo backing buffer.
 *
 *     insmod kernel_pc_demo.ko fifo_size=64 num_events=500 interval_us=500
 *     echo 1 > /sys/module/kernel_pc_demo/parameters/run
 *     cat   /sys/module/kernel_pc_demo/parameters/result
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include "pc_demo.h"

struct pc_ctx pc_ctx_global;

/*
 * Load-time parameters. They are read-only after insmod (0444) and are copied
 * into the context by pc_init(). consumer_type is *not* here: it needs a
 * setter (it is writable at runtime), so it lives in params.c.
 */
static unsigned int fifo_size = 64;
module_param(fifo_size, uint, 0444);
MODULE_PARM_DESC(fifo_size,
                 "kfifo size in slots, power of two, 4..1024 (default 64)");

static unsigned int num_events = 200;
module_param(num_events, uint, 0444);
MODULE_PARM_DESC(
    num_events,
    "number of events the producer generates, 1..50000 (default 200)");

static unsigned int interval_us = 1000;
module_param(interval_us, uint, 0444);
MODULE_PARM_DESC(
    interval_us,
    "producer interval in microseconds, 100..1000000 (default 1000)");

static int __init pc_init(void) {
  struct pc_ctx *ctx = &pc_ctx_global;
  int ret;
  /*
   * consumer_type has already been set by params.c (module parameters are
   * parsed before init), so it is validated but not overwritten here.
   */
  ret = pc_params_validate(fifo_size, num_events, interval_us,
                           ctx->consumer_type);
  if (ret)
    return ret;

  ctx->fifo_size = fifo_size;
  ctx->num_events = num_events;
  ctx->interval_us = interval_us;

  mutex_init(&ctx->stats_lock);
  init_waitqueue_head(&ctx->done);
  atomic_set(&ctx->running, 0);
  ctx->last_run_result = PC_OK;
  pc_stats_reset(ctx);

  /* Dynamic kfifo buffer: fifo_size elements of unsigned int. */
  ret = kfifo_alloc(&ctx->fifo, ctx->fifo_size, GFP_KERNEL);
  if (ret) {
    pr_err("kfifo_alloc(%u) failed: %d\n", ctx->fifo_size, ret);
    return PC_NOMEM;
  }

  hrtimer_setup(&ctx->timer, pc_producer_timer_fn, CLOCK_MONOTONIC,
                HRTIMER_MODE_REL);

  pr_info("loaded: fifo_size=%u num_events=%u interval_us=%u consumer=%s\n",
          ctx->fifo_size, ctx->num_events, ctx->interval_us,
          pc_consumer_name(ctx->consumer_type));
  return PC_OK;
}

static void __exit pc_exit(void) {
  struct pc_ctx *ctx = &pc_ctx_global;

  /* Order matters: stop the producer first, then drain/kill the consumer. */
  pc_producer_stop(ctx);
  pc_consumer_teardown(ctx);
  kfifo_free(&ctx->fifo);
  mutex_destroy(&ctx->stats_lock);

  pr_info("unloaded\n");
}

module_init(pc_init);
module_exit(pc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nalowo");
MODULE_DESCRIPTION(
    "Producer/consumer demo: hrtimer + kfifo, tasklet vs workqueue");
MODULE_VERSION("0.1");
