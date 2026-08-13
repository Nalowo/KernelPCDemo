// SPDX-License-Identifier: GPL-2.0
/*
 * Consumers (bottom-half), two flavours:
 *
 *   tasklet   -- atomic context. Must not sleep: no msleep/mutex_lock/
 *                kmalloc(GFP_KERNEL). Drains the fifo and returns; an empty
 *                fifo means "return now", never wait. sum/last_value are
 *                written without a lock -- a tasklet never runs concurrently
 *                with itself, and a mutex is not an option here anyway. A
 *                concurrent `cat stats` is therefore unsynchronised: that is
 *                deliberate, and part of what this demo is meant to show.
 *
 *   workqueue -- process context. May sleep, so on an empty fifo it waits for
 *                the producer instead of returning. queue_work() may re-queue
 *                the item while it runs, so sum/last_value are protected by
 *                ctx->stats_lock.
 *
 * Being able to wait does *not* mean fewer lost events. msleep(1) sleeps for
 * at least one tick (1 ms at CONFIG_HZ=1000), and queue_work() from the
 * producer's hard-irq context costs noticeably more than tasklet_schedule().
 * Measured with fifo_size=4 num_events=5000 interval_us=100:
 *
 *   tasklet     produced=4979  dropped=21
 *   workqueue   produced=1193  dropped=3807
 *
 * The tasklet drains the fifo right after the timer interrupt, while the
 * worker oversleeps its 100 us window tenfold and the producer overruns the
 * 4-slot fifo meanwhile. The workqueue pays off only when the consumer really
 * must block (I/O, mutexes, GFP_KERNEL); on latency it loses.
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
    } else if (atomic_read(
                   &ctx->producer_active) && // чтобы избежать случая вечной
                                             // блокировки потребителя при
                                             // выключеном продюсере
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
