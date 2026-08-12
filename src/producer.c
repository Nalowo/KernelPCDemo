// SPDX-License-Identifier: GPL-2.0
/*
 * Producer -- hrtimer callback, runs in hard-irq context.
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
 */
enum hrtimer_restart pc_producer_timer_fn(struct hrtimer *timer) {
  struct pc_ctx *ctx = container_of(timer, struct pc_ctx, timer);
  u32 val = get_random_u32_below(1000);

  if (!kfifo_put(&ctx->fifo, val))
    atomic_inc(&ctx->dropped);
  else
    atomic_inc(&ctx->produced);

  pc_consumer_schedule(ctx);

  if (pc_total_events(ctx) >= ctx->num_events) {
    wake_up(&ctx->done);
    return HRTIMER_NORESTART;
  }

  hrtimer_forward_now(timer, us_to_ktime(ctx->interval_us));
  return HRTIMER_RESTART;
}

/*
 * Arm the timer for the first shot.
 */
void pc_producer_start(struct pc_ctx *ctx) {
  atomic_set(&ctx->producer_active, 1);
  hrtimer_start(&ctx->timer, us_to_ktime(ctx->interval_us), HRTIMER_MODE_REL);
}

/*
 * Stop the producer. Safe to call when the timer was never started.
 */
void pc_producer_stop(struct pc_ctx *ctx) {
  hrtimer_cancel(&ctx->timer);
  atomic_set(&ctx->producer_active, 0);
}
