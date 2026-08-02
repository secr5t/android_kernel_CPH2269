#define pr_fmt(fmt) "boostpool: " fmt

#include <asm/page.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/sizes.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/proc_fs.h>
#include <linux/vmstat.h>
#include <linux/oom.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
#include <../../../../kernel/sched/sched.h>

#include "oplus_ion_boost_pool.h"

#define MAX_BOOST_POOL_HIGH (2048 * 256)
#define NUM_ORDERS 3

static const unsigned int orders[NUM_ORDERS] = {8, 4, 0};
static bool boost_pool_enable = true;
static wait_queue_head_t kcrit_scene_wait;
static int kcrit_scene_flag = 0;
static int ori_low_pages;

static inline unsigned int order_to_size(int order)
{
	return PAGE_SIZE << order;
}

static inline int order_to_index(int order)
{
	int i;
	for (i = 0; i < NUM_ORDERS; i++)
		if (orders[i] == order)
			return i;
	return 0;
}

static int boost_pool_nr_pages(struct ion_boost_pool *pool)
{
	if (!pool) {
		pr_err("%s: pool is NULL!\n", __func__);
		return 0;
	}
	return pool->high;
}

void boost_pool_dump(struct ion_boost_pool *pool)
{
	if (!pool)
		return;

	pr_info("Name:%s: %dMib, low: %dMib, high:%dMib\n",
		pool->name,
		boost_pool_nr_pages(pool) >> 8,
		pool->low >> 8,
		pool->high >> 8);
}

static int boost_pool_kworkthread(void *p)
{
	struct ion_boost_pool *pool = p;
	int ret;

	if (!pool) {
		pr_err("%s: p is NULL!\n", __func__);
		return 0;
	}

	while (true) {
		ret = wait_event_interruptible(pool->waitq,
					       (pool->wait_flag == 1));
		if (ret < 0)
			continue;

		pool->wait_flag = 0;
	}

	return 0;
}

struct page *boost_pool_allocate(struct ion_boost_pool *pool,
				 unsigned long size,
				 unsigned int max_order)
{
	int i;
	struct page *page = NULL;

	if (!pool) {
		pr_err("%s: pool is NULL!\n", __func__);
		return NULL;
	}

	for (i = 0; i < NUM_ORDERS; i++) {
		if (size < order_to_size(orders[i]))
			continue;
		if (max_order < orders[i])
			continue;

		page = alloc_pages(GFP_KERNEL | __GFP_ZERO, orders[i]);
		if (!page)
			continue;

		return page;
	}

	return NULL;
}

void boost_pool_wakeup_process(struct ion_boost_pool *pool)
{
	if (!boost_pool_enable || !pool)
		return;

	pool->wait_flag = 1;
	wake_up_interruptible(&pool->waitq);
}

int boost_pool_free(struct ion_boost_pool *pool, struct page *page,
		    int order)
{
	if (!boost_pool_enable)
		return -1;

	if (order == 0 || !pool || !page)
		return -1;

	if (boost_pool_nr_pages(pool) < MAX_POOL_SIZE) {
		__free_pages(page, order);
		return 0;
	}

	return -1;
}

int boost_pool_shrink(struct ion_boost_pool *boost_pool,
		      void *pool, gfp_t gfp_mask,
		      int nr_to_scan)
{
	int pool_max_shrink;
	int other_free = global_zone_page_state(NR_FREE_PAGES);
	int other_file = global_node_page_state(NR_ACTIVE_FILE) + global_node_page_state(NR_INACTIVE_FILE);

	if (!boost_pool || !pool)
		return 0;

	if (boost_pool->tsk && boost_pool->tsk->pid == current->pid)
		return 0;

	pool_max_shrink = boost_pool_nr_pages(boost_pool);

	if ((other_free + other_file > totalram_pages / 10) ||
	    (pool_max_shrink <= ori_low_pages))
		return 0;

	if (nr_to_scan)
		nr_to_scan = min(nr_to_scan, 1024);
	else
		nr_to_scan = 1024;

	if ((pool_max_shrink - nr_to_scan) < boost_pool->low)
		boost_pool->low = boost_pool->high = pool_max_shrink - nr_to_scan;

	return 0;
}

void boost_pool_dec_high(struct ion_boost_pool *pool, int nr_pages)
{
	if (unlikely(nr_pages < 0) || !pool)
		return;

	pool->high = max(pool->low, pool->high - nr_pages);
}

static int bind_task_min_cap_cpus(struct task_struct *tsk)
{
	struct cpumask mask;
	int i, end_cpu = 6;

	cpumask_clear(&mask);
	for (i = 0; i < end_cpu; i++)
		cpumask_set_cpu(i, &mask);

	pr_info("bind %s on cpu[0-%d].\n", tsk->comm, end_cpu);
	return sched_setaffinity(tsk->pid, &mask);
}

static int boost_pool_proc_show(struct seq_file *s, void *v)
{
	struct ion_boost_pool *boost_pool = s->private;

	seq_printf(s, "Name:%s: %dMib, low: %dMib high: %dMib\n",
		   boost_pool->name,
		   boost_pool_nr_pages(boost_pool) >> 8,
		   boost_pool->low >> 8,
		   boost_pool->high >> 8);
	return 0;
}

static int boost_pool_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, boost_pool_proc_show, PDE_DATA(inode));
}

static ssize_t boost_pool_proc_write(struct file *file,
				     const char __user *buf,
				     size_t count, loff_t *ppos)
{
	char buffer[13];
	int err, nr_pages;
	struct ion_boost_pool *boost_pool = PDE_DATA(file_inode(file));

	if (IS_ERR_OR_NULL(boost_pool))
		return -EFAULT;

	memset(buffer, 0, sizeof(buffer));
	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	err = kstrtoint(strstrip(buffer), 0, &nr_pages);
	if (err)
		return err;

	if (nr_pages == 0) {
		boost_pool->high = boost_pool->low;
		return count;
	}

	if (nr_pages < 0 || nr_pages >= MAX_BOOST_POOL_HIGH ||
	    nr_pages <= boost_pool->low)
		return -EINVAL;

	kcrit_scene_flag = 1;
	wake_up_interruptible(&kcrit_scene_wait);
	boost_pool->high = nr_pages;
	boost_pool_wakeup_process(boost_pool);
	return count;
}

static const struct file_operations boost_pool_proc_ops = {
	.owner          = THIS_MODULE,
	.open           = boost_pool_proc_open,
	.read           = seq_read,
	.write          = boost_pool_proc_write,
	.llseek         = seq_lseek,
	.release        = single_release,
};

static int boost_pool_low_proc_show(struct seq_file *s, void *v)
{
	struct ion_boost_pool *boost_pool = s->private;
	seq_printf(s, "boostpool low %d.\n", boost_pool->low >> 8);
	return 0;
}

static int boost_pool_low_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, boost_pool_low_proc_show, PDE_DATA(inode));
}

static ssize_t boost_pool_low_proc_write(struct file *file,
					 const char __user *buf,
					 size_t count, loff_t *ppos)
{
	char buffer[13];
	int err, nr_pages;
	struct ion_boost_pool *boost_pool = PDE_DATA(file_inode(file));

	if (IS_ERR_OR_NULL(boost_pool))
		return -EFAULT;

	memset(buffer, 0, sizeof(buffer));
	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	err = kstrtoint(strstrip(buffer), 0, &nr_pages);
	if (err)
		return err;

	if (nr_pages <= 0 || nr_pages >= MAX_BOOST_POOL_HIGH)
		return -EINVAL;

	boost_pool->low = boost_pool->high = nr_pages;
	return count;
}

static const struct file_operations boost_pool_low_proc_ops = {
	.owner          = THIS_MODULE,
	.open           = boost_pool_low_proc_open,
	.read           = seq_read,
	.write          = boost_pool_low_proc_write,
	.llseek         = seq_lseek,
	.release        = single_release,
};

struct ion_boost_pool *_boost_pool_create_internal(long first_arg, ...)
{
	struct task_struct *tsk;
	struct ion_boost_pool *boost_pool;
	char buf[128];
	char *name = "default";
	struct proc_dir_entry *root_dir = NULL;
	unsigned int ion_flag = 0;
	unsigned int nr_pages = 0;
	va_list args;

	va_start(args, first_arg);
	ion_flag = va_arg(args, unsigned int);
	nr_pages = va_arg(args, unsigned int);
	root_dir = va_arg(args, struct proc_dir_entry *);
	if (!root_dir || IS_ERR(root_dir)) {
		va_end(args);
		return NULL;
	}
	name = va_arg(args, char *);
	va_end(args);

	boost_pool = kzalloc(sizeof(*boost_pool), GFP_KERNEL);
	if (!boost_pool)
		return NULL;

	ori_low_pages = boost_pool->high = boost_pool->low = nr_pages;
	boost_pool->name = name ? name : "boost_pool";
	boost_pool->usage = ion_flag;

	boost_pool->proc_info = proc_create_data(boost_pool->name, 0666,
						 root_dir,
						 &boost_pool_proc_ops,
						 boost_pool);
	if (IS_ERR_OR_NULL(boost_pool->proc_info))
		goto free_heap;

	snprintf(buf, sizeof(buf), "%s_low", boost_pool->name);
	boost_pool->proc_low_info = proc_create_data(buf, 0666,
						     root_dir,
						     &boost_pool_low_proc_ops,
						     boost_pool);
	if (IS_ERR_OR_NULL(boost_pool->proc_low_info))
		goto destroy_proc_info;

	init_waitqueue_head(&boost_pool->waitq);
	tsk = kthread_run(boost_pool_kworkthread, boost_pool,
			  "bp_%s", boost_pool->name);
	if (IS_ERR(tsk))
		goto destroy_proc_low_info;

	boost_pool->tsk = tsk;
	bind_task_min_cap_cpus(tsk);
	boost_pool_wakeup_process(boost_pool);

	return boost_pool;

destroy_proc_low_info:
	if (boost_pool->proc_low_info)
		proc_remove(boost_pool->proc_low_info);
destroy_proc_info:
	if (boost_pool->proc_info)
		proc_remove(boost_pool->proc_info);
free_heap:
	kfree(boost_pool);
	return NULL;
}

static unsigned int kcrit_scene_proc_poll(struct file *file, poll_table *table)
{
	int mask = 0;
	poll_wait(file, &kcrit_scene_wait, table);
	if (kcrit_scene_flag == 1) {
		mask |= POLLIN | POLLRDNORM;
		kcrit_scene_flag = 0;
	}
	return mask;
}

static int kcrit_scene_proc_open(struct inode *inode, struct file *file)
{
	return nonseekable_open(inode, file);
}

static int kcrit_scene_proc_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations kcrit_scene_proc_fops = {
	.owner = THIS_MODULE,
	.open = kcrit_scene_proc_open,
	.release = kcrit_scene_proc_release,
	.poll = kcrit_scene_proc_poll,
};

static __init int kcrit_scene_init(void)
{
	init_waitqueue_head(&kcrit_scene_wait);
	proc_create("kcritical_scene", S_IRUGO, NULL,
		    &kcrit_scene_proc_fops);
	return 0;
}
fs_initcall(kcrit_scene_init);
module_param_named(debug_boost_pool_enable, boost_pool_enable, bool, 0644);
