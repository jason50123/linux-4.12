// bio_layout_check.c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/blk_types.h>
#include <linux/bug.h>

static inline void check_bio_layout(void)
{
    BUILD_BUG_ON(offsetof(struct bio, task_pid) < 
                 sizeof(struct bio) - sizeof(((struct bio *)0)->bi_inline_vecs));
}

static int __init check_bio_layout_init(void)
{
    // 只要這個函式被編譯，即可觸發 compile-time 檢查
    check_bio_layout();
    return 0;
}
// 保證在核心早期階段就執行，若 BUILD_BUG_ON 失敗則直接編譯器報錯
core_initcall(check_bio_layout_init);
