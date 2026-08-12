/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kernel_pc_demo -- shared context and internal API.
 *
 * Layout mirrors the assignment:
 *   main.c     -- module lifecycle, context/kfifo init
 *   params.c   -- module_param_cb interface
 * (run/result/stats/consumer_type/reset) producer.c -- hrtimer callback
 * (top-half) consumer.c -- tasklet and workqueue handlers (bottom-half)
 */
#ifndef PC_DEMO_H
#define PC_DEMO_H

#include <linux/atomic.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

/* Return codes (as recommended by the assignment). */
#define PC_OK 0                 /* operation succeeded */
#define PC_INVALID (-EINVAL)    /* bad parameter */
#define PC_NOMEM (-ENOMEM)      /* out of memory */
#define PC_BUSY (-EBUSY)        /* test already running */
#define PC_TIMEOUT (-ETIMEDOUT) /* test did not finish in time */

/* Parameter limits. */
#define PC_FIFO_SIZE_MIN 4u
#define PC_FIFO_SIZE_MAX 1024u
#define PC_NUM_EVENTS_MIN 1u
#define PC_NUM_EVENTS_MAX 50000u
#define PC_INTERVAL_US_MIN 100u
#define PC_INTERVAL_US_MAX 1000000u

/* consumer_type values. */
enum pc_consumer_type {
  PC_CONSUMER_TASKLET = 0,
  PC_CONSUMER_WORKQUEUE = 1,
};

#define PC_WQ_NAME "pc_demo_wq"

/*
 * Global module context. The first block is the parameter snapshot taken at
 * insmod time; everything below is runtime state.
 *
 * Fields marked (*) are not in the assignment's struct but are needed to
 * implement `run`: a wait queue to sleep on until the producer is done, and a
 * flag that makes run/reset mutually exclusive (-EBUSY).
 */
struct pc_ctx {
  unsigned int fifo_size;
  unsigned int num_events;
  unsigned int interval_us;
  unsigned int consumer_type;
  unsigned int active_type;

  /* Queue */
  DECLARE_KFIFO_PTR(fifo, unsigned int);

  /* Producer */
  struct hrtimer timer;
  atomic_t produced;        /* events pushed into the fifo */
  atomic_t dropped;         /* events dropped (fifo full) */
  atomic_t producer_active; /* producer active status */

  /* Consumer -- tasklet */
  struct tasklet_struct tasklet;

  /* Consumer -- workqueue */
  struct workqueue_struct *wq;
  struct work_struct work;

  /* Processing statistics */
  atomic_t consumed;       /* events processed */
  u64 sum;                 /* sum of processed values */
  unsigned int last_value; /* last processed value */
  struct mutex stats_lock; /* guards sum/last_value (WQ path only) */

  int last_run_result;

  wait_queue_head_t done; /* (*) woken when producer stops */
  atomic_t running;       /* (*) 1 while a test is active */
};

extern struct pc_ctx pc_ctx_global;

/* --- producer.c --- */
enum hrtimer_restart pc_producer_timer_fn(struct hrtimer *timer);
void pc_producer_start(struct pc_ctx *ctx);
void pc_producer_stop(struct pc_ctx *ctx);

/* --- consumer.c --- */
void pc_tasklet_consumer(unsigned long data);
void pc_work_consumer(struct work_struct *work);
int pc_consumer_setup(struct pc_ctx *ctx);
void pc_consumer_teardown(struct pc_ctx *ctx);
void pc_consumer_schedule(struct pc_ctx *ctx);

/* --- params.c --- */
int pc_params_validate(unsigned int fifo_size, unsigned int num_events,
                       unsigned int interval_us, unsigned int consumer_type);
void pc_stats_reset(struct pc_ctx *ctx);

static inline const char *pc_consumer_name(unsigned int type) {
  return type == PC_CONSUMER_WORKQUEUE ? "workqueue" : "tasklet";
}

/* Total events accounted for so far: produced + dropped. */
static inline unsigned int pc_total_events(const struct pc_ctx *ctx) {
  return atomic_read(&ctx->produced) + atomic_read(&ctx->dropped);
}

#endif /* PC_DEMO_H */
