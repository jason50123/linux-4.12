/* zQoS Scheduler for Linux Kernel 4.12 */
#ifndef _ZQOS_SCHEDULER_H
#define _ZQOS_SCHEDULER_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>

/* zQoS related constants */
#define ZQOS_ADJUSTMENT_INTERVAL_MS 200  /* Adjustment interval 200ms */
#define ZQOS_INTERVAL_NUM 3              /* Number of historical records */
#define ZQOS_TOKEN_BUCKET_SIZE 128       /* Token bucket size */

/* Fast scheduling loop parameters (approximate the paper's high-frequency loop) */
#define ZQOS_SCHED_SLICE_US 1000         /* Scheduling slice interval (microseconds) */
#define ZQOS_MAX_DISPATCH_PER_SLICE 64   /* Max requests dispatched per slice */
#define ZQOS_MAX_TOKEN_TIMESPAN_NS (50ULL * NSEC_PER_MSEC)

/* Tail-latency histogram config (approx p99 computation) */
#define ZQOS_TLAT_BUCKETS 64
#define ZQOS_TLAT_US_MAX 1000000 /* 1 second upper bound */

/* Tenant types */
enum tenant_type {
    TENANT_TYPE_LC,  /* Latency-Critical */
    TENANT_TYPE_BE   /* Best-Effort */
};

/* Tenant structure */
struct zqos_tenant {
    struct list_head list;
    int tenant_id;
    uid_t user_id;              /* Associated user ID */
    enum tenant_type type;
    
    /* SLO settings */
    u32 iops_slo;           /* IOPS SLO */
    u32 tail_latency_slo;   /* Tail latency SLO (microseconds) */
    u32 read_ratio;         /* Read ratio (0-100) */
    u64 viops_slo;
    
    /* VIOPS related */
    u64 viops;              /* Allocated VIOPS */
    u64 viops_metric;       /* Actually measured VIOPS */
    bool preemptive;        /* Can preempt BE tenant tokens */
    
    /* Token bucket */
    u32 tokens;
    u32 backup_tokens;
    struct zqos_tenant *backup_from; /* BE tenant chosen for backup tokens */
    u64 token_residual_ns;           /* Fractional token accumulator */
    
    /* Statistics */
    u64 iops_metric;
    u64 tail_latency_metric;
    
    /* Request queue */
    struct list_head request_queue;
    spinlock_t queue_lock;
};

/* Device performance model */
struct zqos_device_model {
    /* Write weight for different usage rates */
    u32 write_weight[10];  /* 10%-100% usage */
    
    /* Size weight for different IO sizes */
    u32 size_weight[6];    /* 4KB, 8KB, 16KB, 32KB, 64KB, 128KB */
    
    /* Optimal concurrent writes */
    u32 optimal_concurrent_writes[6];
    
    /* VIOPS-tail latency curve data */
    struct viops_tlat_point {
        u64 viops;
        u32 tail_latency;
    } **viops_tlat_curves;  /* [usage][read_ratio] */
};

/* Per-device zQoS enforcer */
struct zqos_enforcer {
    struct list_head list;
    int device_id;
    
    /* Device model */
    struct zqos_device_model *model;
    
    /* Device state */
    u32 current_usage;      /* Current usage rate */
    u64 dev_viops;          /* Device available VIOPS */
    u64 viops_metric;       /* Measured VIOPS */
    u32 tlat_metric;        /* Measured tail latency */
    u32 concurrent_writes;  /* Current concurrent writes */
    
    /* Tail latency histogram (for approx p99) */
    u32 tlat_hist[ZQOS_TLAT_BUCKETS];
    u64 tlat_hist_total;
    
    /* Historical records */
    u32 history_tlat[ZQOS_INTERVAL_NUM];
    
    /* Tenant list */
    struct list_head tenants;
    spinlock_t tenants_lock;
    
    /* Work queue */
    struct work_struct adjustment_work;
    struct delayed_work sched_work;
    ktime_t last_sched_time;
    bool sched_active;
};

/* Global arbiter */
struct zqos_arbiter {
    /* All enforcer list */
    struct list_head enforcers;
    rwlock_t enforcers_lock;
    
    /* Adjustment timer */
    struct timer_list adjustment_timer;
    
    /* Statistics */
    u64 total_viops_allocated;
    u64 total_viops_used;
};

/* Function declarations */
int zqos_init(void);
void zqos_exit(void);

/* Tenant management */
int zqos_register_tenant(struct zqos_tenant *tenant);
void zqos_unregister_tenant(struct zqos_tenant *tenant);

/* VIOPS adjustment */
void zqos_adjust_viops(struct zqos_enforcer *enforcer);
void zqos_allocate_viops(struct zqos_enforcer *enforcer);

/* Request scheduling */
int zqos_submit_request(struct zqos_enforcer *enforcer, 
                       struct zqos_tenant *tenant,
                       struct request *req);

void zqos_init_enforcer_runtime(struct zqos_enforcer *enforcer);
void zqos_stop_enforcer_runtime(struct zqos_enforcer *enforcer);

/* Performance model */
u64 zqos_normalize_iops_to_viops(struct zqos_device_model *model,
                                 u32 iops, u32 read_ratio,
                                 u32 *io_size_dist, u32 usage);

#endif /* _ZQOS_SCHEDULER_H */
