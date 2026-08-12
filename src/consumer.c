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

/*
 * TODO:
 *   while (kfifo_get(&ctx->fifo, &val)) {
 *           ctx->sum += val;
 *           ctx->last_value = val;
 *           atomic_inc(&ctx->consumed);
 *   }
 */
void pc_tasklet_consumer(unsigned long data)
{
	struct pc_ctx *ctx = (struct pc_ctx *)data;

	(void)ctx;
}

/*
 * TODO:
 *   loop:
 *     if (kfifo_get(&ctx->fifo, &val)) {
 *             mutex_lock(&ctx->stats_lock);
 *             ctx->sum += val;
 *             ctx->last_value = val;
 *             mutex_unlock(&ctx->stats_lock);
 *             atomic_inc(&ctx->consumed);
 *     } else if (pc_total_events(ctx) < ctx->num_events) {
 *             msleep(1);      -- producer still running, wait for more
 *     } else {
 *             break;
 *     }
 */
void pc_work_consumer(struct work_struct *work)
{
	struct pc_ctx *ctx = container_of(work, struct pc_ctx, work);

	(void)ctx;
}

/*
 * Prepare the consumer selected by ctx->consumer_type. Called from `run`
 * before the producer is armed.
 *
 * TODO:
 *   tasklet:   tasklet_init(&ctx->tasklet, pc_tasklet_consumer,
 *                           (unsigned long)ctx);
 *   workqueue: INIT_WORK(&ctx->work, pc_work_consumer);
 *              ctx->wq = create_singlethread_workqueue(PC_WQ_NAME);
 *              return PC_NOMEM if it is NULL.
 */
int pc_consumer_setup(struct pc_ctx *ctx)
{
	(void)ctx;
	return PC_OK;
}

/*
 * Tear the consumer down: no bottom-half may be pending or running after this
 * returns. Idempotent -- called from `reset`, from the end of `run` and from
 * module exit.
 *
 * TODO:
 *   tasklet:   tasklet_kill(&ctx->tasklet);
 *   workqueue: if (ctx->wq) { flush_workqueue(ctx->wq);
 *                             destroy_workqueue(ctx->wq); ctx->wq = NULL; }
 */
void pc_consumer_teardown(struct pc_ctx *ctx)
{
	(void)ctx;
}

/*
 * Kick the bottom-half from the producer's timer callback (atomic context).
 *
 * TODO:
 *   tasklet:   tasklet_schedule(&ctx->tasklet);
 *   workqueue: queue_work(ctx->wq, &ctx->work);
 */
void pc_consumer_schedule(struct pc_ctx *ctx)
{
	(void)ctx;
}
