/*
 * monitor.c - Jackfruit Kernel Memory Monitor (LKM)
 *
 * Creates /dev/container_monitor (major=200).
 * Major 200 is confirmed free on this kernel.
 *
 * Load:   sudo insmod monitor.ko
 *         sudo mknod /dev/container_monitor c 200 0
 *         sudo chmod 666 /dev/container_monitor
 * Unload: sudo rmmod monitor
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/timer.h>
#include <linux/pid.h>
#include "monitor_ioctl.h"

#define DEVICE_NAME  "container_monitor"
#define DEVICE_MAJOR 200   /* free on this kernel — confirmed via /proc/devices */

struct monitored_proc {
    int              pid;
    unsigned long    soft_limit;     /* KB */
    unsigned long    hard_limit;     /* KB */
    bool             soft_limit_hit;
    struct list_head list;
};

static LIST_HEAD(monitor_list);
static DEFINE_MUTEX(monitor_lock);
static struct timer_list monitor_timer;

static void check_memory(struct timer_list *t)
{
    struct monitored_proc *entry, *tmp;
    struct task_struct    *task;
    unsigned long          rss_kb;

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
        rcu_read_lock();
        task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);
        rcu_read_unlock();

        if (!task) {
            printk(KERN_INFO "[Monitor] PID %d gone, removing\n", entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (!task->mm) continue;

        rss_kb = get_mm_rss(task->mm) << (PAGE_SHIFT - 10);

        if (rss_kb > entry->hard_limit) {
            printk(KERN_ALERT
                   "[Monitor] HARD LIMIT: PID %d RSS=%luKB > hard=%luKB — KILLING\n",
                   entry->pid, rss_kb, entry->hard_limit);
            rcu_read_lock();
            task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);
            if (task)
                kill_pid(find_vpid(entry->pid), SIGKILL, 1);
            rcu_read_unlock();
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (rss_kb > entry->soft_limit && !entry->soft_limit_hit) {
            printk(KERN_WARNING
                   "[Monitor] SOFT LIMIT: PID %d RSS=%luKB > soft=%luKB — WARNING\n",
                   entry->pid, rss_kb, entry->soft_limit);
            entry->soft_limit_hit = true;
        }
    }

    mutex_unlock(&monitor_lock);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));
}

static long mon_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    if (cmd == IOCTL_REGISTER_CONTAINER) {
        struct container_limits  lim;
        struct monitored_proc   *new_proc;

        if (copy_from_user(&lim,
                           (struct container_limits __user *)arg,
                           sizeof(lim)))
            return -EFAULT;

        if (lim.pid <= 0 || lim.hard_limit == 0)
            return -EINVAL;

        new_proc = kmalloc(sizeof(*new_proc), GFP_KERNEL);
        if (!new_proc) return -ENOMEM;

        new_proc->pid            = lim.pid;
        new_proc->soft_limit     = lim.soft_limit;
        new_proc->hard_limit     = lim.hard_limit;
        new_proc->soft_limit_hit = false;
        INIT_LIST_HEAD(&new_proc->list);

        mutex_lock(&monitor_lock);
        list_add(&new_proc->list, &monitor_list);
        mutex_unlock(&monitor_lock);

        printk(KERN_INFO
               "[Monitor] Registered PID %d  soft=%luKB  hard=%luKB\n",
               lim.pid, lim.soft_limit, lim.hard_limit);
        return 0;
    }
    return -ENOTTY;
}

static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = mon_ioctl,
};

static int __init monitor_init(void)
{
    int ret = register_chrdev(DEVICE_MAJOR, DEVICE_NAME, &fops);
    if (ret < 0) {
        printk(KERN_ALERT "[Monitor] register_chrdev(%d) failed: %d\n",
               DEVICE_MAJOR, ret);
        return ret;
    }
    timer_setup(&monitor_timer, check_memory, 0);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));
    printk(KERN_INFO "[Monitor] Loaded OK — major=%d /dev/%s\n",
           DEVICE_MAJOR, DEVICE_NAME);
    return 0;
}

static void __exit monitor_exit(void)
{
    struct monitored_proc *entry, *tmp;
    del_timer_sync(&monitor_timer);
    mutex_lock(&monitor_lock);
    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&monitor_lock);
    unregister_chrdev(DEVICE_MAJOR, DEVICE_NAME);
    printk(KERN_INFO "[Monitor] Unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jackfruit Team");
MODULE_DESCRIPTION("Container memory monitor — soft/hard limits");
