#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>

#define DEVICE_NAME "envchar"
#define ENVCHAR_BUFFER_SIZE 4096

struct envchar_dev {
    struct cdev cdev;
    struct class *class;
    struct device *device;
    dev_t devt;

    char *buffer;
    size_t data_size;   /* number of valid bytes in buffer */
    struct mutex lock;
};

static struct envchar_dev env_dev;

static int envchar_open(struct inode *inode, struct file *filp)
{
    filp->private_data = &env_dev;
    return 0;
}

static int envchar_release(struct inode *inode, struct file *filp)
{
    return 0;
}

/* Simple read: copy from internal buffer */
static ssize_t envchar_read(struct file *filp, char __user *buf,
                            size_t count, loff_t *ppos)
{
    struct envchar_dev *dev = filp->private_data;
    ssize_t ret;

    if (*ppos >= dev->data_size)
        return 0; /* EOF */

    if (count > dev->data_size - *ppos)
        count = dev->data_size - *ppos;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    if (copy_to_user(buf, dev->buffer + *ppos, count)) {
        ret = -EFAULT;
    } else {
        *ppos += count;
        ret = count;
    }

    mutex_unlock(&dev->lock);
    return ret;
}

/* Simple write: append into buffer, reset if no space */
static ssize_t envchar_write(struct file *filp, const char __user *buf,
                             size_t count, loff_t *ppos)
{
    struct envchar_dev *dev = filp->private_data;
    ssize_t ret;

    if (count > ENVCHAR_BUFFER_SIZE)
        count = ENVCHAR_BUFFER_SIZE;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    if (count > ENVCHAR_BUFFER_SIZE - dev->data_size) {
        /* naive behavior for now: reset buffer if not enough room */
        dev->data_size = 0;
    }

    if (copy_from_user(dev->buffer + dev->data_size, buf, count)) {
        ret = -EFAULT;
    } else {
        dev->data_size += count;
        ret = count;
    }

    mutex_unlock(&dev->lock);
    return ret;
}

static const struct file_operations envchar_fops = {
    .owner   = THIS_MODULE,
    .open    = envchar_open,
    .release = envchar_release,
    .read    = envchar_read,
    .write   = envchar_write,
};

static int __init envchar_init(void)
{
    int rc;

    env_dev.buffer = kzalloc(ENVCHAR_BUFFER_SIZE, GFP_KERNEL);
    if (!env_dev.buffer) {
        pr_err("envchar: failed to allocate buffer\n");
        return -ENOMEM;
    }

    mutex_init(&env_dev.lock);

    rc = alloc_chrdev_region(&env_dev.devt, 0, 1, DEVICE_NAME);
    if (rc) {
        pr_err("envchar: alloc_chrdev_region failed (%d)\n", rc);
        goto err_alloc;
    }

    cdev_init(&env_dev.cdev, &envchar_fops);
    env_dev.cdev.owner = THIS_MODULE;

    rc = cdev_add(&env_dev.cdev, env_dev.devt, 1);
    if (rc) {
        pr_err("envchar: cdev_add failed (%d)\n", rc);
        goto err_cdev;
    }

env_dev.class = class_create(DEVICE_NAME);
if (IS_ERR(env_dev.class)) {
    rc = PTR_ERR(env_dev.class);
    pr_err("envchar: class_create failed (%d)\n", rc);
    goto err_class;
}


    env_dev.device = device_create(env_dev.class, NULL, env_dev.devt,
                                   NULL, DEVICE_NAME);
    if (IS_ERR(env_dev.device)) {
        rc = PTR_ERR(env_dev.device);
        pr_err("envchar: device_create failed (%d)\n", rc);
        goto err_device;
    }

    pr_info("envchar: loaded, major=%d minor=%d\n",
            MAJOR(env_dev.devt), MINOR(env_dev.devt));
    return 0;

err_device:
    class_destroy(env_dev.class);
err_class:
    cdev_del(&env_dev.cdev);
err_cdev:
    unregister_chrdev_region(env_dev.devt, 1);
err_alloc:
    kfree(env_dev.buffer);
    return rc;
}

static void __exit envchar_exit(void)
{
    device_destroy(env_dev.class, env_dev.devt);
    class_destroy(env_dev.class);
    cdev_del(&env_dev.cdev);
    unregister_chrdev_region(env_dev.devt, 1);
    kfree(env_dev.buffer);
    pr_info("envchar: unloaded\n");
}

module_init(envchar_init);
module_exit(envchar_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrea Fuggetta");
MODULE_DESCRIPTION("Environment/presence log character device");
