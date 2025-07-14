/* zQoS Scheduler Implementation for Linux Kernel 4.12 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/blkdev.h>
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
    u32 usage_idx = enforcer->current_usage / 10 - 1;
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
    
    spin_lock(&enforcer->tenants_lock);
    
    /* First allocate to LC tenants */
    list_for_each_entry(tenant, &enforcer->tenants, list) {
        if (tenant->type == TENANT_TYPE_LC) {
            /* Adjust based on load distribution */
            tenant->viops = min(tenant->viops_metric, tenant->viops_slo);
            tenant->preemptive = (tenant->viops_metric < tenant->viops_slo);
            remaining_viops -= tenant->viops;
        } else {
            total_be_weight += 1; /* Simplified: all BE tenants have equal weight */
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
    
    spin_unlock(&enforcer->tenants_lock);
}

/**
 * zqos_schedule_requests - Schedule requests using token bucket
 * @enforcer: zQoS enforcer
 *
 * Schedules requests based on token bucket algorithm.
 * LC tenants get priority and can preempt BE tokens.
 */
static void zqos_schedule_requests(struct zqos_enforcer *enforcer)
{
    struct zqos_tenant *tenant;
    struct request *req;
    u32 tokens_needed;
    
    spin_lock(&enforcer->tenants_lock);
    
    /* Generate tokens */
    list_for_each_entry(tenant, &enforcer->tenants, list) {
        u32 new_tokens = tenant->viops * ZQOS_ADJUSTMENT_INTERVAL_MS / 1000;
        tenant->tokens = min((u32)(tenant->tokens + new_tokens), (u32)ZQOS_TOKEN_BUCKET_SIZE);
    }
    
    /* Schedule LC tenant requests first */
    list_for_each_entry(tenant, &enforcer->tenants, list) {
        if (tenant->type != TENANT_TYPE_LC)
            continue;
            
        spin_lock(&tenant->queue_lock);
        while (!list_empty(&tenant->request_queue)) {
            req = list_first_entry(&tenant->request_queue, struct request, queuelist);
            tokens_needed = blk_rq_bytes(req) / 4096; /* Simplified: 4KB units */
            
            /* Verify request belongs to correct tenant */
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
                /* Add scheduling trace */
                printk(KERN_DEBUG "ZQoS: LC_DISPATCH tenant_id=%d tokens_used=%u remaining=%u\n",
                       tenant->tenant_id, tokens_needed, tenant->tokens);
                /* Submit request to actual device */
                blk_execute_rq_nowait(req->q, NULL, req, 1, NULL);
            } else if (tenant->preemptive && 
                      (tenant->tokens + tenant->backup_tokens) >= tokens_needed) {
                /* Use backup tokens */
                u32 tokens_from_backup = tokens_needed - tenant->tokens;
                tenant->backup_tokens -= tokens_from_backup;
                tenant->tokens = 0;
                list_del_init(&req->queuelist);
                printk(KERN_DEBUG "ZQoS: LC_PREEMPT tenant_id=%d backup_used=%u\n",
                       tenant->tenant_id, tokens_from_backup);
                blk_execute_rq_nowait(req->q, NULL, req, 1, NULL);
            } else {
                break; /* Insufficient tokens */
            }
        }
        spin_unlock(&tenant->queue_lock);
    }
    
    /* Schedule BE tenant requests */
    list_for_each_entry(tenant, &enforcer->tenants, list) {
        if (tenant->type != TENANT_TYPE_BE)
            continue;
            
        spin_lock(&tenant->queue_lock);
        while (!list_empty(&tenant->request_queue) &&
               enforcer->concurrent_writes < enforcer->model->optimal_concurrent_writes[0]) {
            req = list_first_entry(&tenant->request_queue, struct request, queuelist);
            tokens_needed = blk_rq_bytes(req) / 4096;
            
            /* Verify request belongs to correct tenant */
            if (uid_valid(req->rq_uid)) {
                uid_t req_uid = from_kuid_munged(&init_user_ns, req->rq_uid);
                if (req_uid != tenant->user_id) {
                    printk(KERN_WARNING "ZQoS: Request UID mismatch in BE queue - req_uid=%u tenant_uid=%u\n",
                           req_uid, tenant->user_id);
                    list_del_init(&req->queuelist);
                    continue; /* Skip this mismatched request */
                }
            }
            
            if (tenant->tokens >= tokens_needed) {
                tenant->tokens -= tokens_needed;
                list_del_init(&req->queuelist);
                
                /* Track concurrent writes */
                if (req_op(req) == REQ_OP_WRITE) {
                    enforcer->concurrent_writes++;
                }
                
                /* Add BE scheduling trace */
                printk(KERN_DEBUG "ZQoS: BE_DISPATCH tenant_id=%d tokens_used=%u remaining=%u\n",
                       tenant->tenant_id, tokens_needed, tenant->tokens);
                
                blk_execute_rq_nowait(req->q, NULL, req, 1, NULL);
            } else {
                break;
            }
        }
        spin_unlock(&tenant->queue_lock);
    }
    
    spin_unlock(&enforcer->tenants_lock);
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
    zqos_schedule_requests(enforcer);
    
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