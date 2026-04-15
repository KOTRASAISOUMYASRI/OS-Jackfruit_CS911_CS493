/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * Provided boilerplate:
 *   - device registration and teardown
 *   - timer setup
 *   - RSS helper
 *   - soft-limit and hard-limit event helpers
 *   - ioctl dispatch shell
 *
 * YOUR WORK: Fill in all sections marked // TODO.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/sched/signal.h>

#define DEVICE_NAME "monitor"
#define CLASS_NAME "monitor_class"

static int major;
static struct class *monitor_class = NULL;
static struct device *monitor_device = NULL;

/* IOCTL COMMAND */
#define IOCTL_MONITOR_PID _IOW('a', 'a', int)

/* IOCTL HANDLER */
static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    int pid;
    struct task_struct *task;

    if (cmd == IOCTL_MONITOR_PID) {
        if (copy_from_user(&pid, (int *)arg, sizeof(pid))) {
            printk(KERN_ERR "Failed to copy PID\n");
            return -EFAULT;
        }

        printk(KERN_INFO "[monitor] Received PID: %d\n", pid);

        task = pid_task(find_vpid(pid), PIDTYPE_PID);
        if (task) {
            printk(KERN_INFO "[monitor] Process: %s\n", task->comm);
        } else {
            printk(KERN_INFO "[monitor] Process not found\n");
        }
    }

    return 0;
}

/* FILE OPERATIONS */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* INIT */
static int __init monitor_init(void) {
    printk(KERN_INFO "Monitor module loaded\n");

    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0) {
        printk(KERN_ALERT "Failed to register device\n");
        return major;
    }

    monitor_class = class_create(CLASS_NAME);
    if (IS_ERR(monitor_class)) {
        unregister_chrdev(major, DEVICE_NAME);
        return PTR_ERR(monitor_class);
    }

    monitor_device = device_create(monitor_class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);
    if (IS_ERR(monitor_device)) {
        class_destroy(monitor_class);
        unregister_chrdev(major, DEVICE_NAME);
        return PTR_ERR(monitor_device);
    }

    printk(KERN_INFO "/dev/monitor created\n");
    return 0;
}

/* EXIT */
static void __exit monitor_exit(void) {
    device_destroy(monitor_class, MKDEV(major, 0));
    class_destroy(monitor_class);
    unregister_chrdev(major, DEVICE_NAME);

    printk(KERN_INFO "Monitor module unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Simple Container Monitor");
