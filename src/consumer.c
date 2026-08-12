// SPDX-License-Identifier: GPL-2.0
/*
 * Consumers (bottom-half), two flavours:
 *
 *   tasklet   -- atomic context. Must not sleep: no msleep/mutex_lock/
 *                kmalloc(GFP_KERNEL). Drains the fifo and returns; an empty
 *                fifo means "return now", never wait. sum/last_value need no
 *                lock: a tasklet never runs concurrently with itself.
 *
 *   workqueue -- process context. May sleep, so on an empty fifo it can
 *                msleep(1) while the producer is still running instead of
 *                losing events. queue_work() may re-queue the item, so
 *                sum/last_value are protected by ctx->stats_lock.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "pc_demo.h"

void pc_tasklet_consumer(unsigned long data) {
  struct pc_ctx *ctx = (struct pc_ctx *)data;

  u32 val = 0;
  while (kfifo_get(&ctx->fifo, &val)) {
    ctx->sum += val;
    ctx->last_value = val;
    atomic_inc(&ctx->consumed);
  }
}

void pc_work_consumer(struct work_struct *work) {
  struct pc_ctx *ctx = container_of(work, struct pc_ctx, work);

  u32 val = 0;
  while (true) {
    if (kfifo_get(&ctx->fifo, &val)) {
      mutex_lock(&ctx->stats_lock);
      ctx->sum += val;
      ctx->last_value = val;
      mutex_unlock(&ctx->stats_lock);
      atomic_inc(&ctx->consumed);
    } else if (atomic_read(&ctx->producer_active) &&
               pc_total_events(ctx) < ctx->num_events) {
      msleep(1);
    } else {
      break;
    }
  }
}

/*
 * Prepare the consumer selected by ctx->consumer_type. Called from `run`
 * before the producer is armed.
 */
int pc_consumer_setup(struct pc_ctx *ctx) {
  ctx->active_type = ctx->consumer_type;
  switch (ctx->active_type) {
  case PC_CONSUMER_WORKQUEUE:
    INIT_WORK(&ctx->work, pc_work_consumer);
    ctx->wq = create_singlethread_workqueue(PC_WQ_NAME);
    if (ctx->wq == NULL)
      return PC_NOMEM;
    break;
  case PC_CONSUMER_TASKLET:
    tasklet_init(&ctx->tasklet, pc_tasklet_consumer, (unsigned long)ctx);
    break;
  default:
    return PC_INVALID;
  }
  return PC_OK;
}

/*
 * Tear the consumer down: no bottom-half may be pending or running after this
 * returns. Idempotent -- called from `reset`, from the end of `run` and from
 * module exit.
 */
void pc_consumer_teardown(struct pc_ctx *ctx) {
  switch (ctx->active_type) {
  case PC_CONSUMER_WORKQUEUE:
    if (ctx->wq) {
      flush_workqueue(ctx->wq);
      destroy_workqueue(ctx->wq);
      ctx->wq = NULL;
    }
    break;
  case PC_CONSUMER_TASKLET:
    tasklet_kill(&ctx->tasklet);
    break;
  default:
    break;
  }
}

/*
 * Kick the bottom-half from the producer's timer callback (atomic context).
 */
void pc_consumer_schedule(struct pc_ctx *ctx) {
  switch (ctx->active_type) {
  case PC_CONSUMER_WORKQUEUE:
    if (ctx->wq)
      queue_work(ctx->wq, &ctx->work);
    break;
  case PC_CONSUMER_TASKLET:
    tasklet_schedule(&ctx->tasklet);
    break;
  default:
    break;
  }
}
