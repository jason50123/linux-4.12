/* zQoS Integration with Linux Block Layer */
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/zqos_scheduler.h>

/* External variable declarations */
extern struct zqos_arbiter *global_arbiter;
extern struct workqueue_struct *zqos_wq;

/* Forward declarations */
static void zqos_adjustment_work_fn(struct work_struct *work);
static struct zqos_tenant *zqos_find_tenant_by_bio(struct zqos_enforcer *enforcer,
                                                   struct bio *bio);
static struct zqos_tenant *zqos_find_tenant_by_request(struct zqos_enforcer *enforcer,
                                                      struct request *rq);

/* zQoS request data */
struct zqos_request_data {
    struct zqos_tenant *tenant;
    ktime_t enqueue_time;
    u32 io_size;
};

/**
 * zqos_init_request - Initialize request data for zQoS
 * @q: request queue
 * @rq: request to initialize
 *
 * Allocates and initializes zQoS-specific request data structure.
 * Sets enqueue time and stores in request private data.
 */
static void zqos_init_request(struct request_queue *q, struct request *rq)
{
    struct zqos_request_data *rq_data;
    
    rq_data = kzalloc(sizeof(*rq_data), GFP_ATOMIC);
    if (rq_data) {
        rq_data->enqueue_time = ktime_get();
        rq->elv.priv[0] = rq_data;
    }
}

/**
 * zqos_merged_requests - Handle request merging
 * @q: request queue
 * @rq: primary request
 * @next: request to merge
 *
 * Simplified implementation: removes next request from queue.
 */
static void zqos_merged_requests(struct request_queue *q, struct request *rq, 
                                struct request *next)
{
    /* Simplified implementation: only remove next request */
    list_del_init(&next->queuelist);
}

/**
 * zqos_dispatch - Dispatch requests from zQoS queues
 * @q: request queue
 * @force: force dispatch flag
 *
 * Simplified implementation: dispatches from first available tenant.
 *
 * Return: 1 if request dispatched, 0 otherwise
 */
static int zqos_dispatch(struct request_queue *q, int force)
{
    struct zqos_enforcer *enforcer = q->queuedata;
    struct zqos_tenant *tenant;
    struct request *rq = NULL;
    
    if (!enforcer)
        return 0;
    
    /* Simplified implementation: get request from first tenant */
    spin_lock(&enforcer->tenants_lock);
    list_for_each_entry(tenant, &enforcer->tenants, list) {
        spin_lock(&tenant->queue_lock);
        if (!list_empty(&tenant->request_queue)) {
            rq = list_first_entry(&tenant->request_queue, struct request, queuelist);
            list_del_init(&rq->queuelist);
        }
        spin_unlock(&tenant->queue_lock);
        if (rq)
            break;
    }
    spin_unlock(&enforcer->tenants_lock);
    
    if (rq) {
        elv_dispatch_sort(q, rq);
        return 1;
    }
    
    return 0;
}

/**
 * zqos_add_request - Add request to zQoS queues
 * @q: request queue
 * @rq: request to add
 *
 * Adds request to appropriate tenant queue based on UID tracking.
 * Initializes request data and performs tenant lookup.
 */
static void zqos_add_request(struct request_queue *q, struct request *rq)
{
    struct zqos_enforcer *enforcer = q->queuedata;
    struct zqos_request_data *rq_data;
    struct zqos_tenant *tenant;
    
    if (!enforcer)
        return;
    
    /* Initialize request data */
    zqos_init_request(q, rq);
    rq_data = rq->elv.priv[0];
    
    /* Find corresponding tenant - use request UID tracking */
    tenant = zqos_find_tenant_by_request(enforcer, rq);
    
    /* Add tracepoint for UID tracking */
    if (rq) {
        printk(KERN_DEBUG "ZQoS: TRACE rq_uid=%u bio_uid=%u\n",
               from_kuid_munged(&init_user_ns, rq->rq_uid),
               rq->bio ? from_kuid_munged(&init_user_ns, rq->bio->bi_uid) : 0);
    }
    if (!tenant) {
        /* If no tenant found, create default tenant or use first one */
        spin_lock(&enforcer->tenants_lock);
        if (!list_empty(&enforcer->tenants)) {
            tenant = list_first_entry(&enforcer->tenants, struct zqos_tenant, list);
        }
        spin_unlock(&enforcer->tenants_lock);
    }
    
    if (tenant && rq_data) {
        rq_data->tenant = tenant;
        printk(KERN_INFO "ZQoS: Assign request to tenant %d, type=%s\n", 
               tenant->tenant_id, tenant->type == TENANT_TYPE_LC ? "LC" : "BE");
        if (rq->bio)
            rq_data->io_size = bio_sectors(rq->bio) * 512;
        
        spin_lock(&tenant->queue_lock);
        list_add_tail(&rq->queuelist, &tenant->request_queue);
        spin_unlock(&tenant->queue_lock);
    }
}

/**
 * zqos_completed_request - Handle request completion
 * @q: request queue
 * @rq: completed request
 *
 * Updates tenant and device statistics, performs UID verification
 * for accurate accounting. Calculates latency and updates metrics.
 */
static void zqos_completed_request(struct request_queue *q, struct request *rq)
{
    struct zqos_enforcer *enforcer = q->queuedata;
    struct zqos_request_data *rq_data = rq->elv.priv[0];
    ktime_t latency;
    uid_t rq_uid, tenant_uid;
    
    if (!rq_data)
        return;
    
    /* Calculate latency */
    latency = ktime_sub(ktime_get(), rq_data->enqueue_time);
    
    /* Update statistics */
    if (rq_data->tenant) {
        struct zqos_tenant *tenant = rq_data->tenant;
        
        /* Verify UID consistency for accurate accounting */
        rq_uid = from_kuid_munged(&init_user_ns, rq->rq_uid);
        tenant_uid = tenant->user_id;
        
        if (rq_uid != tenant_uid) {
            printk(KERN_WARNING "ZQoS: UID mismatch on completion - rq_uid=%u tenant_uid=%u\n",
                   rq_uid, tenant_uid);
            /* Try to find correct tenant for accounting */
            tenant = zqos_find_tenant_by_request(enforcer, rq);
        }
        
        if (tenant) {
            /* Update IOPS and latency statistics */
            tenant->iops_metric++;
            tenant->tail_latency_metric = ktime_to_us(latency);
            
            /* Add detailed accounting trace */
            printk(KERN_DEBUG "ZQoS: COMPLETE rq_uid=%u tenant_id=%d latency=%lld us\n",
                   rq_uid, tenant->tenant_id, ktime_to_us(latency));
        }
        
        /* Update device statistics */
        enforcer->viops_metric++;
        enforcer->tlat_metric = max(enforcer->tlat_metric, 
                                   (u32)ktime_to_us(latency));
    }
    
    /* If write request, decrease concurrent write count */
    if (req_op(rq) == REQ_OP_WRITE && enforcer->concurrent_writes > 0) {
        enforcer->concurrent_writes--;
    }
    
    kfree(rq_data);
}

/**
 * zqos_insert_requests - Insert requests into zQoS queues (multi-queue)
 * @hctx: hardware queue context
 * @rq_list: list of requests to insert
 * @at_head: insert at head flag
 *
 * Multi-queue implementation for inserting requests. Performs UID tracking
 * and tenant assignment for each request.
 */
static void zqos_insert_requests(struct blk_mq_hw_ctx *hctx, struct list_head *rq_list, bool at_head)
{
    struct request_queue *q = hctx->queue;
    struct zqos_enforcer *enforcer = q->elevator->elevator_data;
    struct request *rq, *next;
    struct zqos_tenant *tenant;
    struct zqos_request_data *rq_data;
    
    printk(KERN_DEBUG "ZQoS: Insert requests, enforcer=%p\n", enforcer);
    printk(KERN_DEBUG "ZQoS: Check if tenant list is empty during insert: %s\n", 
           list_empty(&enforcer->tenants) ? "Yes" : "No");
    
    if (!enforcer) {
        printk(KERN_WARNING "ZQoS: enforcer is NULL!\n");
        return;
    }
    
    list_for_each_entry_safe(rq, next, rq_list, queuelist) {
        list_del_init(&rq->queuelist);
        
        /* Allocate and initialize request data */
        rq_data = kzalloc(sizeof(*rq_data), GFP_ATOMIC);
        if (rq_data) {
            rq_data->enqueue_time = ktime_get();
            rq->elv.priv[0] = rq_data;
        }
        
        /* Find corresponding tenant - use request UID tracking */
        tenant = zqos_find_tenant_by_request(enforcer, rq);
        
        /* Add detailed UID tracking messages */
        if (rq) {
            printk(KERN_INFO "ZQoS: INSERT_REQ rq_uid=%u bio_uid=%u tenant_id=%d\n",
                   from_kuid_munged(&init_user_ns, rq->rq_uid),
                   rq->bio ? from_kuid_munged(&init_user_ns, rq->bio->bi_uid) : 0,
                   tenant ? tenant->tenant_id : -1);
        }
        if (!tenant) {
            /* Use first tenant as default */
            spin_lock(&enforcer->tenants_lock);
            if (!list_empty(&enforcer->tenants)) {
                tenant = list_first_entry(&enforcer->tenants, struct zqos_tenant, list);
            }
            spin_unlock(&enforcer->tenants_lock);
        }
        
        if (tenant && rq_data) {
            rq_data->tenant = tenant;
            if (rq->bio)
                rq_data->io_size = bio_sectors(rq->bio) * 512;
            
            spin_lock(&tenant->queue_lock);
            if (at_head)
                list_add(&rq->queuelist, &tenant->request_queue);
            else
                list_add_tail(&rq->queuelist, &tenant->request_queue);
            spin_unlock(&tenant->queue_lock);
        }
    }
}

/**
 * zqos_dispatch_request - Dispatch single request (multi-queue)
 * @hctx: hardware queue context
 *
 * Multi-queue implementation for dispatching requests.
 * Simplified implementation: dispatches from first available tenant.
 *
 * Return: request to dispatch or NULL
 */
static struct request *zqos_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
    struct request_queue *q = hctx->queue;
    struct zqos_enforcer *enforcer = q->elevator->elevator_data;
    struct zqos_tenant *tenant;
    struct request *rq = NULL;
    
    if (!enforcer)
        return NULL;
    
    printk(KERN_DEBUG "ZQoS: Attempt to dispatch request, enforcer=%p\n", enforcer);
    printk(KERN_DEBUG "ZQoS: Check if tenant list is empty during dispatch: %s\n", 
           list_empty(&enforcer->tenants) ? "Yes" : "No");
    /* Simplified implementation: get from first tenant with requests */
    spin_lock(&enforcer->tenants_lock);
    list_for_each_entry(tenant, &enforcer->tenants, list) {
        spin_lock(&tenant->queue_lock);
        if (!list_empty(&tenant->request_queue)) {
            rq = list_first_entry(&tenant->request_queue, struct request, queuelist);
            list_del_init(&rq->queuelist);
            printk(KERN_INFO "ZQoS: Dispatch request to tenant %d\n", tenant->tenant_id);
        }
        spin_unlock(&tenant->queue_lock);
        if (rq)
            break;
    }
    spin_unlock(&enforcer->tenants_lock);
    
    if (!rq) {
        printk(KERN_DEBUG "ZQoS: No dispatchable requests\n");
    }
    
    return rq;
}

/**
 * zqos_has_work - Check if scheduler has work (multi-queue)
 * @hctx: hardware queue context
 *
 * Checks if any tenant has pending requests.
 *
 * Return: true if work available, false otherwise
 */
static bool zqos_has_work(struct blk_mq_hw_ctx *hctx)
{
    struct request_queue *q = hctx->queue;
    struct zqos_enforcer *enforcer = q->elevator->elevator_data;
    struct zqos_tenant *tenant;
    bool has_work = false;
    
    if (!enforcer)
        return false;
    
    spin_lock(&enforcer->tenants_lock);
    list_for_each_entry(tenant, &enforcer->tenants, list) {
        if (!list_empty(&tenant->request_queue)) {
            has_work = true;
            break;
        }
    }
    spin_unlock(&enforcer->tenants_lock);
    
    return has_work;
}

/**
 * zqos_init_sched - Initialize zQoS scheduler
 * @q: request queue
 * @e: elevator type
 *
 * Initializes zQoS scheduler for a request queue. Creates enforcer
 * structure and default tenant.
 *
 * Return: 0 on success, negative error code on failure
 */
static int zqos_init_sched(struct request_queue *q, struct elevator_type *e)
{
    struct elevator_queue *eq;
    struct zqos_enforcer *enforcer;
    
    printk(KERN_INFO "ZQoS: Initialize scheduler for queue %p\n", q);
    
    eq = elevator_alloc(q, e);
    if (!eq) {
        printk(KERN_ERR "ZQoS: elevator_alloc failed\n");
        return -ENOMEM;
    }
    
    enforcer = kzalloc(sizeof(*enforcer), GFP_KERNEL);
    if (!enforcer) {
        kobject_put(&eq->kobj);
        return -ENOMEM;
    }
    
    INIT_LIST_HEAD(&enforcer->tenants);
    spin_lock_init(&enforcer->tenants_lock);
    INIT_WORK(&enforcer->adjustment_work, zqos_adjustment_work_fn);
    
    /* Set default values */
    enforcer->dev_viops = 10000; /* Default VIOPS */
    
    eq->elevator_data = enforcer;
    q->elevator = eq;
    
    /* Create default tenant */
    {
        struct zqos_tenant *default_tenant = kmalloc(sizeof(struct zqos_tenant), GFP_KERNEL);
        if (default_tenant) {
            default_tenant->tenant_id = 1;
            default_tenant->user_id = 0;  /* root user */
            default_tenant->type = TENANT_TYPE_LC;
            default_tenant->iops_slo = 10000;
            default_tenant->tail_latency_slo = 2000; /* 2ms */
            default_tenant->read_ratio = 70;
            default_tenant->viops = 5000;
            default_tenant->viops_metric = 0;
            default_tenant->tokens = 1000;
            default_tenant->backup_tokens = 500;
            INIT_LIST_HEAD(&default_tenant->request_queue);
            spin_lock_init(&default_tenant->queue_lock);
            
            spin_lock(&enforcer->tenants_lock);
            list_add(&default_tenant->list, &enforcer->tenants);
            spin_unlock(&enforcer->tenants_lock);
            
            printk(KERN_INFO "ZQoS: init_sched created default tenant %d (LC)\n", default_tenant->tenant_id);
        } else {
            printk(KERN_ERR "ZQoS: init_sched failed to allocate default tenant memory\n");
        }
    }
    
    return 0;
}

/**
 * zqos_exit_sched - Clean up zQoS scheduler
 * @eq: elevator queue
 *
 * Cleans up zQoS scheduler resources including work queue and enforcer.
 */
static void zqos_exit_sched(struct elevator_queue *eq)
{
    struct zqos_enforcer *enforcer = eq->elevator_data;
    
    if (enforcer) {
        cancel_work_sync(&enforcer->adjustment_work);
        kfree(enforcer);
    }
}


/* zQoS elevator operations - Multi-Queue version */
static struct elevator_type zqos_iosched = {
    .ops.mq = {
        .insert_requests = zqos_insert_requests,
        .dispatch_request = zqos_dispatch_request,
        .has_work = zqos_has_work,
        .init_sched = zqos_init_sched,
        .exit_sched = zqos_exit_sched,
    },
    .uses_mq = true,
    .elevator_name = "zqos",
    .elevator_owner = THIS_MODULE,
};

/**
 * zqos_register_device - Register zQoS scheduler to specific device
 * @q: request queue
 * @model: device model
 *
 * Registers zQoS scheduler to a specific device with device model.
 * Creates enforcer and default tenant.
 *
 * Return: 0 on success, negative error code on failure
 */
int zqos_register_device(struct request_queue *q, 
                        struct zqos_device_model *model)
{
    struct zqos_enforcer *enforcer;
    int ret;
    
    enforcer = kzalloc(sizeof(*enforcer), GFP_KERNEL);
    if (!enforcer)
        return -ENOMEM;
    
    INIT_LIST_HEAD(&enforcer->tenants);
    spin_lock_init(&enforcer->tenants_lock);
    INIT_WORK(&enforcer->adjustment_work, zqos_adjustment_work_fn);
    
    enforcer->model = model;
    enforcer->dev_viops = model->viops_tlat_curves[7][5].viops; /* Default value */
    
    q->queuedata = enforcer;
    
    /* Set elevator */
    ret = elevator_init(q, "zqos");
    if (ret) {
        kfree(enforcer);
        return ret;
    }
    
    /* Create default tenant */
    {
        struct zqos_tenant *default_tenant = kmalloc(sizeof(struct zqos_tenant), GFP_KERNEL);
        if (default_tenant) {
            default_tenant->tenant_id = 1;
            default_tenant->user_id = 0;  /* root user */
            default_tenant->type = TENANT_TYPE_LC;
            default_tenant->iops_slo = 10000;
            default_tenant->tail_latency_slo = 2000; /* 2ms */
            default_tenant->read_ratio = 70;
            default_tenant->viops = 5000;
            default_tenant->viops_metric = 0;
            default_tenant->tokens = 1000;
            default_tenant->backup_tokens = 500;
            INIT_LIST_HEAD(&default_tenant->request_queue);
            spin_lock_init(&default_tenant->queue_lock);
            
            spin_lock(&enforcer->tenants_lock);
            list_add(&default_tenant->list, &enforcer->tenants);
            spin_unlock(&enforcer->tenants_lock);
            
            printk(KERN_INFO "ZQoS: Created default tenant %d (LC)\n", default_tenant->tenant_id);
            printk(KERN_INFO "ZQoS: enforcer tenants list not empty: %s\n", 
                   list_empty(&enforcer->tenants) ? "No" : "Yes");
        } else {
            printk(KERN_ERR "ZQoS: Failed to allocate default tenant memory\n");
        }
    }
    
    /* Add to global list */
    write_lock(&global_arbiter->enforcers_lock);
    list_add(&enforcer->list, &global_arbiter->enforcers);
    write_unlock(&global_arbiter->enforcers_lock);
    
    return 0;
}

/**
 * zqos_adjustment_work_fn - Adjustment work function
 * @work: work struct
 *
 * Simplified adjustment logic for VIOPS based on metrics.
 */
static void zqos_adjustment_work_fn(struct work_struct *work)
{
    struct zqos_enforcer *enforcer = 
        container_of(work, struct zqos_enforcer, adjustment_work);
    
    /* Simplified adjustment logic */
    if (enforcer && enforcer->model) {
        /* Basic VIOPS adjustment */
        if (enforcer->viops_metric > enforcer->dev_viops * 90 / 100) {
            enforcer->dev_viops = enforcer->dev_viops * 105 / 100;
        }
    }
}

/**
 * zqos_find_tenant_by_request - Find tenant by request UID
 * @enforcer: zQoS enforcer
 * @rq: request
 *
 * Primary function for tenant lookup, prioritizes request UID.
 * Creates new tenant dynamically if not found.
 *
 * Return: tenant pointer or NULL
 */
static struct zqos_tenant *zqos_find_tenant_by_request(struct zqos_enforcer *enforcer,
                                                      struct request *rq)
{
    struct zqos_tenant *tenant = NULL;
    uid_t uid;
    
    if (!rq) {
        /* If no request, use default tenant */
        spin_lock(&enforcer->tenants_lock);
        if (!list_empty(&enforcer->tenants)) {
            tenant = list_first_entry(&enforcer->tenants, 
                                     struct zqos_tenant, list);
        }
        spin_unlock(&enforcer->tenants_lock);
        return tenant;
    }
    
    /* Extract user ID from request first */
    uid = from_kuid_munged(&init_user_ns, rq->rq_uid);
    
    /* If request UID invalid, try extracting from bio */
    if (uid == (uid_t)-1 && rq->bio) {
        uid = from_kuid_munged(&init_user_ns, rq->bio->bi_uid);
    }
    
    /* Find corresponding tenant by UID */
    spin_lock(&enforcer->tenants_lock);
    list_for_each_entry(tenant, &enforcer->tenants, list) {
        if (tenant->user_id == uid) {
            spin_unlock(&enforcer->tenants_lock);
            printk(KERN_DEBUG "ZQoS: Found UID %u corresponding tenant %d (via rq_uid)\n", 
                   uid, tenant->tenant_id);
            return tenant;
        }
    }
    
    /* If no corresponding tenant found, create one dynamically */
    spin_unlock(&enforcer->tenants_lock);
    
    /* Create new tenant */
    tenant = kmalloc(sizeof(struct zqos_tenant), GFP_ATOMIC);
    if (tenant) {
        /* Set tenant ID based on UID, ensure uniqueness */
        tenant->tenant_id = uid + 1000;  /* Avoid conflict with default tenant */
        tenant->user_id = uid;
        
        /* Determine tenant type by UID */
        if (uid == 0) {
            tenant->type = TENANT_TYPE_LC;  /* root user has higher priority */
            tenant->iops_slo = 15000;
            tenant->tail_latency_slo = 1000;  /* 1ms */
            tenant->viops = 8000;
        } else {
            tenant->type = TENANT_TYPE_BE;
            tenant->iops_slo = 5000;
            tenant->tail_latency_slo = 5000;  /* 5ms */
            tenant->viops = 3000;
        }
        
        tenant->read_ratio = 70;
        tenant->viops_metric = 0;
        tenant->tokens = 1000;
        tenant->backup_tokens = 500;
        INIT_LIST_HEAD(&tenant->request_queue);
        spin_lock_init(&tenant->queue_lock);
        
        spin_lock(&enforcer->tenants_lock);
        list_add(&tenant->list, &enforcer->tenants);
        spin_unlock(&enforcer->tenants_lock);
        
        printk(KERN_INFO "ZQoS: Created new tenant %d (%s) for UID %u [rq_uid=%u]\n", 
               tenant->tenant_id, 
               tenant->type == TENANT_TYPE_LC ? "LC" : "BE",
               uid, from_kuid_munged(&init_user_ns, rq->rq_uid));
    }
    
    return tenant;
}

/**
 * zqos_find_tenant_by_bio - Find tenant by bio (legacy compatibility)
 * @enforcer: zQoS enforcer
 * @bio: bio structure
 *
 * Legacy function for backward compatibility.
 * Creates temporary request structure for compatibility.
 *
 * Return: tenant pointer or NULL
 */
static struct zqos_tenant *zqos_find_tenant_by_bio(struct zqos_enforcer *enforcer,
                                                   struct bio *bio)
{
    /* Create a temporary request structure for compatibility */
    struct request temp_rq;
    memset(&temp_rq, 0, sizeof(temp_rq));
    if (bio) {
        temp_rq.rq_uid = bio->bi_uid;
        temp_rq.bio = bio;
        return zqos_find_tenant_by_request(enforcer, &temp_rq);
    }
    return zqos_find_tenant_by_request(enforcer, NULL);
}

/**
 * zqos_blk_init - Initialize zQoS block layer module
 *
 * Initializes zQoS core and registers elevator.
 *
 * Return: 0 on success, negative error code on failure
 */
static int __init zqos_blk_init(void)
{
    int ret;
    
    ret = zqos_init();
    if (ret)
        return ret;
    
    ret = elv_register(&zqos_iosched);
    if (ret) {
        zqos_exit();
        return ret;
    }
    
    return 0;
}

/**
 * zqos_blk_exit - Clean up zQoS block layer module
 *
 * Unregisters elevator and cleans up zQoS core.
 */
static void __exit zqos_blk_exit(void)
{
    elv_unregister(&zqos_iosched);
    zqos_exit();
}

module_init(zqos_blk_init);
module_exit(zqos_blk_exit);