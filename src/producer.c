// SPDX-License-Identifier: GPL-2.0
/*
 * Producer -- hrtimer callback, runs in atomic (soft-irq) context.
 *
 * Hard requirement: it must never sleep and never wait. If the kfifo is full
 * the event is dropped immediately and accounted in ctx->dropped.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/random.h>

#include "pc_demo.h"

/*
 * Timer callback (top-half).
 *
 * TODO:
 *   1. if pc_total_events(ctx) >= ctx->num_events:
 *        wake_up(&ctx->done) and return HRTIMER_NORESTART;
 *   2. val = get_random_u32() % 1000;
 *   3. if (!kfifo_put(&ctx->fifo, val)) atomic_inc(&ctx->dropped);
 *      else                             atomic_inc(&ctx->produced);
 *   4. pc_consumer_schedule(ctx);          -- tasklet_schedule / queue_work
 *   5. hrtimer_forward_now(timer, us_to_ktime(ctx->interval_us));
 *      return HRTIMER_RESTART;
 */
enum hrtimer_restart pc_producer_timer_fn(struct hrtimer *timer)
{
	struct pc_ctx *ctx = container_of(timer, struct pc_ctx, timer);

	(void)ctx;
	return HRTIMER_NORESTART;
}

/*
 * Arm the timer for the first shot.
 *
 * TODO: hrtimer_start(&ctx->timer, us_to_ktime(ctx->interval_us),
 *                     HRTIMER_MODE_REL);
 */
void pc_producer_start(struct pc_ctx *ctx)
{
	(void)ctx;
}

/*
 * Stop the producer. Safe to call when the timer was never started.
 *
 * TODO: hrtimer_cancel(&ctx->timer); -- must not be called from the callback
 *       itself (it waits for the handler to finish).
 */
void pc_producer_stop(struct pc_ctx *ctx)
{
	(void)ctx;
}
