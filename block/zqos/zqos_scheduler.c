/* zQoS Scheduler Implementation for Linux Kernel 4.12 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/blkdev.h>
#include <linux/random.h>
#include <linux/timekeeping.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/zqos_scheduler.h>

struct zqos_arbiter *global_arbiter;
struct workqueue_struct *zqos_wq;

EXPORT_SYMBOL(global_arbiter);
EXPORT_SYMBOL(zqos_wq);

/**
 * zqos_normalize_iops_to_viops - Normalize IOPS to VIOPS
 * @model: device model
 * @iops: input IOPS
 * @read_ratio: read ratio percentage
 * @io_size_dist: I/O size distribution array
 * @usage: device usage percentage
 *
 * Converts actual IOPS to virtual IOPS considering I/O characteristics.
 * Accounts for read/write ratio, I/O size distribution, and device usage.
 *
 * Return: normalized VIOPS value
 */
u64 zqos_normalize_iops_to_viops(struct zqos_device_model *model,
                                 u32 iops, u32 read_ratio,
                                 u32 *io_size_dist, u32 usage)
{
    u64 viops;
    u32 size_factor = 0;
    u32 write_weight;
    int i;
    
    /* Calculate size factor */
    for (i = 0; i < 6; i++) {
        size_factor += model->size_weight[i] * io_size_dist[i] / 100;
    }
    
    /* Get write weight based on usage */
    write_weight = model->write_weight[usage / 10 - 1];
    
    /* Calculate VIOPS */
    viops = iops * size_factor;
    viops = viops * (read_ratio + (100 - read_ratio) * write_weight) / 100;
    
    return viops;
}

/**
 * zqos_adjust_device_viops - Adjust device VIOPS capacity
 * @enforcer: zQoS enforcer
 *
 * Adjusts device VIOPS capacity based on current latency metrics
 * and device model curves. Uses linear interpolation for optimization.
 */
void zqos_adjust_device_viops(struct zqos_enforcer *enforcer)
{
    struct viops_tlat_point *curve;
    u64 new_viops = enforcer->dev_viops;
    int usage_idx = (int)enforcer->current_usage / 10 - 1;
    if (usage_idx < 0)
        usage_idx = 0;
    if (usage_idx > 9)
        usage_idx = 9;
    int i;
    
    /* Check if adjustment needed */
    if (enforcer->tlat_metric <= enforcer->model->viops_tlat_curves[usage_idx][0].tail_latency) {
        /* Latency below SLO, try to increase VIOPS */
        if (enforcer->viops_metric < enforcer->dev_viops * 90 / 100) {
            return; /* Insufficient usage, no adjustment */
        }
        
        /* Check historical latency */
        for (i = 0; i < ZQOS_INTERVAL_NUM; i++) {
            if (enforcer->history_tlat[i] >= 
                enforcer->model->viops_tlat_curves[usage_idx][0].tail_latency * 80 / 100) {
                return; /* Historical latency too high, no adjustment */
            }
        }
        
        /* Increase VIOPS based on model */
        curve = enforcer->model->viops_tlat_curves[usage_idx];
        for (i = 0; curve[i].viops != 0; i++) {
            if (curve[i].viops > enforcer->dev_viops) {
                if (curve[i].tail_latency > enforcer->model->viops_tlat_curves[usage_idx][0].tail_latency) {
                    /* Linear interpolation for new VIOPS */
                    u64 delta_viops = curve[i].viops - enforcer->viops_metric;
                    u32 delta_tlat = curve[i].tail_latency - enforcer->tlat_metric;
                    u64 r = delta_viops / delta_tlat;
                    new_viops = enforcer->viops_metric + 
                               r * (enforcer->model->viops_tlat_curves[usage_idx][0].tail_latency - 
                                   enforcer->tlat_metric);
                } else {
                    new_viops = curve[i].viops;
                }
                break;
            }
        }
    } else {
        /* Latency exceeds SLO, reset to model value */
        new_viops = enforcer->model->viops_tlat_curves[usage_idx][0].viops;
    }
    
    enforcer->dev_viops = new_viops;
}

/**
 * zqos_allocate_viops_to_tenants - Allocate VIOPS to tenants
 * @enforcer: zQoS enforcer
 *
 * Allocates device VIOPS capacity to tenants.
 * LC tenants get priority allocation, remaining goes to BE tenants.
 */
void zqos_allocate_viops_to_tenants(struct zqos_enforcer *enforcer)
{
    struct zqos_tenant *tenant;
    u64 remaining_viops = enforcer->dev_viops;
    u32 total_be_weight = 0;
    u32 be_count = 0;
    
    spin_lock(&enforcer->tenants_lock);
    
    /* First allocate to LC tenants and count BE tenants */
    list_for_each_entry(tenant, &enforcer->tenants, list) {
        if (tenant->type == TENANT_TYPE_LC) {
            /* Adjust based on load distribution */
            tenant->viops = min(tenant->viops_metric, tenant->viops_slo);
            tenant->preemptive = (tenant->viops_metric < tenant->viops_slo);
            remaining_viops -= tenant->viops;
        } else {
            total_be_weight += 1; /* Simplified: all BE tenants have equal weight */
            be_count++;
        }
    }
    
    /* Allocate remaining VIOPS to BE tenants */
    if (total_be_weight > 0) {
        u64 viops_per_be = remaining_viops / total_be_weight;
        list_for_each_entry(tenant, &enforcer->tenants, list) {
            if (tenant->type == TENANT_TYPE_BE) {
                tenant->viops = viops_per_be;
            }
        }
    }
    
    /* Assign backup tokens source for each LC tenant: randomly select one BE */
    list_for_each_entry(tenant, &enforcer->tenants, list) {
        if (tenant->type != TENANT_TYPE_LC)
            continue;
        tenant->backup_from = NULL;
        tenant->backup_tokens = 0;
        if (be_count == 0)
            continue;
        /* choose a BE index */
        u32 pick = prandom_u32() % be_count;
        u32 idx = 0;
        struct zqos_tenant *be;
        list_for_each_entry(be, &enforcer->tenants, list) {
            if (be->type != TENANT_TYPE_BE)
                continue;
            if (idx == pick) {
                tenant->backup_from = be;
                /* Snapshot at most half of BE tokens as backup potential */
                tenant->backup_tokens = min((u32)(ZQOS_TOKEN_BUCKET_SIZE / 2), be->tokens / 2);
                break;
            }
            idx++;
        }
    }
    
    spin_unlock(&enforcer->tenants_lock);
}

/**
 * zqos_schedule_requests - Schedule requests using token bucket
 * @enforcer: zQoS enforcer
 * @delta_ns: time elapsed since last slice (nanoseconds)
 * @dispatch_budget: maximum number of requests to dispatch in this slice
 *
 * Schedules requests based on token bucket algorithm.
 * LC tenants get priority and can preempt BE tokens.
 */
static void zqos_schedule_requests(struct zqos_enforcer *enforcer,
                                   u64 delta_ns,
                                   u32 dispatch_budget)
{
    struct zqos_tenant *tenant;
    struct request *req;
    u32 tokens_needed;

    spin_lock(&enforcer->tenants_lock);

    if (delta_ns) {
        if (delta_ns > ZQOS_MAX_TOKEN_TIMESPAN_NS)
            delta_ns = ZQOS_MAX_TOKEN_TIMESPAN_NS;

        list_for_each_entry(tenant, &enforcer->tenants, list) {
            u64 generated = tenant->viops * delta_ns + tenant->token_residual_ns;
            u32 new_tokens = div64_u64(generated, NSEC_PER_SEC);

            tenant->token_residual_ns = generated - (u64)new_tokens * NSEC_PER_SEC;
            if (new_tokens) {
                u32 updated = tenant->tokens + new_tokens;
                if (updated > ZQOS_TOKEN_BUCKET_SIZE)
                    updated = ZQOS_TOKEN_BUCKET_SIZE;
                tenant->tokens = updated;
            }
        }
    }

    list_for_each_entry(tenant, &enforcer->tenants, list) {
        if (!dispatch_budget)
            break;
        if (tenant->type != TENANT_TYPE_LC)
            continue;

        spin_lock(&tenant->queue_lock);
        while (dispatch_budget && !list_empty(&tenant->request_queue)) {
            req = list_first_entry(&tenant->request_queue, struct request, queuelist);
            tokens_needed = blk_rq_bytes(req) / 4096; /* Simplified: 4KB units */

            if (uid_valid(req->rq_uid)) {
                uid_t req_uid = from_kuid_munged(&init_user_ns, req->rq_uid);
                if (req_uid != tenant->user_id) {
                    printk(KERN_WARNING "ZQoS: Request UID mismatch in LC queue - req_uid=%u tenant_uid=%u\n",
                           req_uid, tenant->user_id);
                    list_del_init(&req->queuelist);
                    continue; /* Skip this mismatched request */
                }
            }

            if (tenant->tokens >= tokens_needed) {
                tenant->tokens -= tokens_needed;
                list_del_init(&req->queuelist);
                blk_execute_rq_nowait(req->q, NULL, req, 1, NULL);
                dispatch_budget--;
                continue;
            }

            if (tenant->preemptive &&
                (tenant->tokens + tenant->backup_tokens) >= tokens_needed) {
                u32 tokens_from_backup = tokens_needed - tenant->tokens;
                tenant->backup_tokens -= tokens_from_backup;
                tenant->tokens = 0;
                if (tenant->backup_from) {
                    u32 take = tokens_from_backup;
                    if (tenant->backup_from->tokens < take)
                        take = tenant->backup_from->tokens;
                    tenant->backup_from->tokens -= take;
                }
                list_del_init(&req->queuelist);
                blk_execute_rq_nowait(req->q, NULL, req, 1, NULL);
                dispatch_budget--;
                continue;
            }

            break; /* Insufficient tokens */
        }
        spin_unlock(&tenant->queue_lock);
        if (!dispatch_budget)
            goto out_unlock;
    }

    list_for_each_entry(tenant, &enforcer->tenants, list) {
        if (!dispatch_budget)
            break;
        if (tenant->type != TENANT_TYPE_BE)
            continue;

        spin_lock(&tenant->queue_lock);
        while (dispatch_budget && !list_empty(&tenant->request_queue) &&
               enforcer->concurrent_writes < enforcer->model->optimal_concurrent_writes[0]) {
            req = list_first_entry(&tenant->request_queue, struct request, queuelist);
            tokens_needed = blk_rq_bytes(req) / 4096;

            if (uid_valid(req->rq_uid)) {
                uid_t req_uid = from_kuid_munged(&init_user_ns, req->rq_uid);
                if (req_uid != tenant->user_id) {
                    printk(KERN_WARNING "ZQoS: Request UID mismatch in BE queue - req_uid=%u tenant_uid=%u\n",
                           req_uid, tenant->user_id);
                    list_del_init(&req->queuelist);
                    continue; /* Skip this mismatched request */
                }
            }

            if (tenant->tokens < tokens_needed)
                break;

            tenant->tokens -= tokens_needed;
            list_del_init(&req->queuelist);

            if (req_op(req) == REQ_OP_WRITE)
                enforcer->concurrent_writes++;

            blk_execute_rq_nowait(req->q, NULL, req, 1, NULL);
            dispatch_budget--;
        }
        spin_unlock(&tenant->queue_lock);
    }

out_unlock:
    spin_unlock(&enforcer->tenants_lock);
}


static unsigned long zqos_sched_delay_jiffies(void)
{
    unsigned long delay = usecs_to_jiffies(ZQOS_SCHED_SLICE_US);

    return delay ? delay : 1;
}

static void zqos_sched_slice_work(struct work_struct *work)
{
    struct zqos_enforcer *enforcer =
        container_of(work, struct zqos_enforcer, sched_work.work);
    ktime_t now;
    u64 delta_ns;

    if (!enforcer->sched_active)
        return;

    now = ktime_get();
    if (enforcer->last_sched_time)
        delta_ns = ktime_to_ns(ktime_sub(now, enforcer->last_sched_time));
    else
        delta_ns = (u64)ZQOS_SCHED_SLICE_US * NSEC_PER_USEC;

    enforcer->last_sched_time = now;

    zqos_schedule_requests(enforcer, delta_ns, ZQOS_MAX_DISPATCH_PER_SLICE);

    if (enforcer->sched_active)
        queue_delayed_work(zqos_wq, &enforcer->sched_work,
                           zqos_sched_delay_jiffies());
}

void zqos_init_enforcer_runtime(struct zqos_enforcer *enforcer)
{
    if (!enforcer || enforcer->sched_active)
        return;

    INIT_DELAYED_WORK(&enforcer->sched_work, zqos_sched_slice_work);
    enforcer->last_sched_time = ktime_get();
    enforcer->sched_active = true;

    queue_delayed_work(zqos_wq, &enforcer->sched_work, 0);
}

void zqos_stop_enforcer_runtime(struct zqos_enforcer *enforcer)
{
    if (!enforcer || !enforcer->sched_active)
        return;

    enforcer->sched_active = false;
    cancel_delayed_work_sync(&enforcer->sched_work);
    enforcer->last_sched_time = 0;
}

/**
 * zqos_adjustment_work_fn - Adjustment work function
 * @work: work struct
 *
 * Periodic adjustment work function that performs device VIOPS adjustment,
 * VIOPS allocation, and request scheduling.
 */
static void zqos_adjustment_work_fn(struct work_struct *work)
{
    struct zqos_enforcer *enforcer = 
        container_of(work, struct zqos_enforcer, adjustment_work);
    
    /* Adjust device VIOPS */
    zqos_adjust_device_viops(enforcer);
    
    /* Reallocate VIOPS */
    zqos_allocate_viops_to_tenants(enforcer);
    
    /* Schedule requests */
    zqos_schedule_requests(enforcer, 0, ZQOS_MAX_DISPATCH_PER_SLICE);
    
    /* Update history */
    memmove(&enforcer->history_tlat[1], &enforcer->history_tlat[0],
            (ZQOS_INTERVAL_NUM - 1) * sizeof(u32));
    enforcer->history_tlat[0] = enforcer->tlat_metric;
}

/**
 * zqos_adjustment_timer_fn - Timer callback function
 * @data: timer data (unused)
 *
 * Timer callback that triggers adjustment work for all enforcers.
 */
static void zqos_adjustment_timer_fn(unsigned long data)
{
    struct zqos_enforcer *enforcer;
    
    read_lock(&global_arbiter->enforcers_lock);
    list_for_each_entry(enforcer, &global_arbiter->enforcers, list) {
        queue_work(zqos_wq, &enforcer->adjustment_work);
    }
    read_unlock(&global_arbiter->enforcers_lock);
    
    /* Reset timer */
    mod_timer(&global_arbiter->adjustment_timer,
              jiffies + msecs_to_jiffies(ZQOS_ADJUSTMENT_INTERVAL_MS));
}

/**
 * zqos_init - Initialize zQoS scheduler
 *
 * Initializes global arbiter, work queue, and adjustment timer.
 *
 * Return: 0 on success, negative error code on failure
 */
int zqos_init(void)
{
    global_arbiter = kzalloc(sizeof(*global_arbiter), GFP_KERNEL);
    if (!global_arbiter)
        return -ENOMEM;
    
    INIT_LIST_HEAD(&global_arbiter->enforcers);
    rwlock_init(&global_arbiter->enforcers_lock);
    
    /* Create work queue */
    zqos_wq = create_workqueue("zqos_wq");
    if (!zqos_wq) {
        kfree(global_arbiter);
        return -ENOMEM;
    }
    
    /* Initialize timer */
    setup_timer(&global_arbiter->adjustment_timer, 
                zqos_adjustment_timer_fn, 0);
    mod_timer(&global_arbiter->adjustment_timer,
              jiffies + msecs_to_jiffies(ZQOS_ADJUSTMENT_INTERVAL_MS));
    
    printk(KERN_INFO "zQoS scheduler initialized\n");
    return 0;
}

/**
 * zqos_exit - Clean up zQoS scheduler
 *
 * Cleans up timer, work queue, and global arbiter.
 */
void zqos_exit(void)
{
    del_timer_sync(&global_arbiter->adjustment_timer);
    destroy_workqueue(zqos_wq);
    kfree(global_arbiter);
    printk(KERN_INFO "zQoS scheduler exited\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ZQos");
MODULE_DESCRIPTION("zQoS Scheduler for NVMe SSDs");
