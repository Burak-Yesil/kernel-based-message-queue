/*
 * scull.c -- the bare scull char module
 *
 * Copyright (C) 2001 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/cdev.h>
#include <linux/uaccess.h>	/* copy_*_user */
#include <linux/semaphore.h>
#include <linux/mutex.h>

#include "scull.h"		/* local definitions */

/*
 * Our parameters which can be set at load time.
 */

static int scull_major =   SCULL_MAJOR;
static int scull_minor =   0;
static int scull_fifo_elemsz = SCULL_FIFO_ELEMSZ_DEFAULT; /* ELEMSZ */
static int scull_fifo_size   = SCULL_FIFO_SIZE_DEFAULT;   /* N      */

module_param(scull_major, int, S_IRUGO);
module_param(scull_minor, int, S_IRUGO);
module_param(scull_fifo_size, int, S_IRUGO);
module_param(scull_fifo_elemsz, int, S_IRUGO);

MODULE_AUTHOR("byesil");
MODULE_LICENSE("Dual BSD/GPL");

static struct cdev scull_cdev;		/* Char device structure */


DEFINE_MUTEX(scull_mutex);
static struct semaphore writer;
static struct semaphore reader;
static char *scull_message_queue;
static char *head, *end;

/*
 * Open and close
 */

static int scull_open(struct inode *inode, struct file *filp)
{
	printk(KERN_INFO "scull open\n");
	return 0;          /* success */
}

static int scull_release(struct inode *inode, struct file *filp)
{
	printk(KERN_INFO "scull close\n");
	return 0;
}

/*
 * Read and Write
 */
static ssize_t scull_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    // Get the current data size and pointer to the data
    printk(KERN_INFO "scull read\n");
    char *dataPtr = head + sizeof(ssize_t);
    ssize_t dataSize = *(ssize_t *)head;

    // Acquire semaphore and mutex; return -ERESTARTSYS if interrupted
    if (down_interruptible(&reader) || mutex_lock_interruptible(&scull_mutex))
    {
        up(&writer);
        return -ERESTARTSYS;
    }

    // If the requested count is greater than the data size, set count to data size
    count = (count > dataSize) ? dataSize : count;

    // Copy the data to the user buffer; return -EFAULT if copying fails
    if (copy_to_user(buf, dataPtr, count))
    {
        up(&writer);
        mutex_unlock(&scull_mutex);
        return -EFAULT;
    }

    // Update the head pointer to the next data chunk
    head += sizeof(ssize_t) + scull_fifo_elemsz;
    head = scull_message_queue + (head - scull_message_queue) % (scull_fifo_size * (sizeof(ssize_t) + scull_fifo_elemsz));

    // Release the mutex and semaphore
    mutex_unlock(&scull_mutex);
    up(&writer);

    // Return the number of bytes read
    return count;
}

static ssize_t scull_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    printk(KERN_INFO "scull write\n");
    char *dataPtr = end + sizeof(ssize_t);
    ssize_t *sizePtr = (ssize_t *)end;

    // Acquire semaphore; return -ERESTARTSYS if interrupted
    if (down_interruptible(&writer))
    {
        return -ERESTARTSYS;
    }

    // Acquire mutex; return -ERESTARTSYS if interrupted
    if (mutex_lock_interruptible(&scull_mutex))
    {
        up(&reader);
        return -ERESTARTSYS;
    }

    // Check message size, update count if necessary, and copy message to kernel space
    count = (count > scull_fifo_elemsz) ? scull_fifo_elemsz : count;
    if (!copy_from_user(dataPtr, buf, count))
    {
        *sizePtr = count;
        end += sizeof(ssize_t) + scull_fifo_elemsz;
        end = scull_message_queue + (end - scull_message_queue) % (scull_fifo_size * (sizeof(ssize_t) + scull_fifo_elemsz));
    }
    else
    {
        count = -EFAULT;
    }

    mutex_unlock(&scull_mutex);
    up(&reader);

    return count;
}



/*
 * The ioctl() implementation
 */
static long scull_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{

	int err = 0;
	int retval = 0;
    
	/*
	 * extract the type and number bitfields, and don't decode
	 * wrong cmds: return ENOTTY (inappropriate ioctl) before access_ok()
	 */
	if (_IOC_TYPE(cmd) != SCULL_IOC_MAGIC) return -ENOTTY;
	if (_IOC_NR(cmd) > SCULL_IOC_MAXNR) return -ENOTTY;

	err = !access_ok((void __user *)arg, _IOC_SIZE(cmd));
	if (err) return -EFAULT;

	switch(cmd) {
	case SCULL_IOCGETELEMSZ:
		return scull_fifo_elemsz;

	default:  /* redundant, as cmd was checked against MAXNR */
		return -ENOTTY;
	}
	return retval;

}

struct file_operations scull_fops = {
	.owner 		= THIS_MODULE,
	.unlocked_ioctl = scull_ioctl,
	.open 		= scull_open,
	.release	= scull_release,
	.read 		= scull_read,
	.write 		= scull_write,
};

/*
 * Finally, the module stuff
 */

/*
 * The cleanup function is used to handle initialization failures as well.
 * Thefore, it must be careful to work correctly even if some of the items
 * have not been initialized
 */
void scull_cleanup_module(void)
{
	dev_t devno = MKDEV(scull_major, scull_minor);

	/* TODO: free FIFO safely here */
	kfree(scull_message_queue);

	/* Get rid of the char dev entry */
	cdev_del(&scull_cdev);

	/* cleanup_module is never called if registering failed */
	unregister_chrdev_region(devno, 1);
}

int scull_init_module(void)
{
	int result;
	dev_t dev = 0;

	/*
	 * Get a range of minor numbers to work with, asking for a dynamic
	 * major unless directed otherwise at load time.
	 */
	if (scull_major) {
		dev = MKDEV(scull_major, scull_minor);
		result = register_chrdev_region(dev, 1, "scull");
	} else {
		result = alloc_chrdev_region(&dev, scull_minor, 1, "scull");
		scull_major = MAJOR(dev);
	}
	if (result < 0) {
		printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
		return result;
	}

	cdev_init(&scull_cdev, &scull_fops);
	scull_cdev.owner = THIS_MODULE;
	result = cdev_add (&scull_cdev, dev, 1);
	/* Fail gracefully if need be */
	if (result) {
		printk(KERN_NOTICE "Error %d adding scull character device", result);
		goto fail;
	}

	/* TODO: allocate FIFO correctly here */
    scull_message_queue = kmalloc(scull_fifo_size * (scull_fifo_elemsz + sizeof(size_t)) * sizeof(char), GFP_KERNEL);
    if (!scull_message_queue) {
        printk(KERN_WARNING "scull: failed to allocate FIFO buffer\n");
        result = -ENOMEM;
        goto fail;
    }
	head = scull_message_queue;
	end = scull_message_queue;

	/*initialize semaphores*/
	sema_init(&reader, 0);
	sema_init(&writer, scull_fifo_size);
	

	printk(KERN_INFO "scull: FIFO SIZE=%u, ELEMSZ=%u\n", scull_fifo_size, scull_fifo_elemsz);

	return 0; /* succeed */

  fail:
	scull_cleanup_module();
	return result;
}

module_init(scull_init_module);
module_exit(scull_cleanup_module);
