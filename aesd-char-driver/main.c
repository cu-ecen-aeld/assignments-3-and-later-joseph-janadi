/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"
#include "aesd_ioctl.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Joseph Janadi"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    struct aesd_dev *dev;
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle read
     */
    struct aesd_dev *dev = filp->private_data;

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    // Get starting entry and its byte offset
    int cur_entry_idx = dev->head;
    struct entry cur_entry = dev->ring_buf[cur_entry_idx];
    loff_t byte_idx = *f_pos;
    int num_entries = dev->count - 1;   // TODO: Must subtract 1 to work, but why?
    while (num_entries && byte_idx >= cur_entry.size) {
        byte_idx -= cur_entry.size;
        cur_entry_idx = (cur_entry_idx + 1) % SIZE_RING_BUF;
        cur_entry = dev->ring_buf[cur_entry_idx];
        num_entries--;
    }

    // Copy bytes from entry to user space buf
    size_t bto_copy = cur_entry.size - byte_idx;
    if (count < bto_copy) {
        bto_copy = count;
    }
    retval = bto_copy;
    // Update f_pos
    *f_pos = *f_pos + bto_copy;
    if (copy_to_user(buf, cur_entry.p + byte_idx, bto_copy)) {
        retval = -EFAULT;
    }

    mutex_unlock(&dev->lock);

    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle write
     */
    struct aesd_dev *dev = filp->private_data;

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }
    // (Re)allocate entry buffer
    char *new_entry_buf = krealloc(dev->entry_buf.p, dev->entry_buf.size + count, GFP_KERNEL);
    if (new_entry_buf == NULL) {
        PDEBUG("Failed to (re)allocate entry_buf");
        return -ENOMEM;
    }
    dev->entry_buf.p = new_entry_buf;
    dev->entry_buf.size += count;

    // Copy from user buf to kernel entry_buf
    unsigned long bto_copy = count;
    int idx;
    while (bto_copy) {
        // Adjust f_pos
        *f_pos = *f_pos + bto_copy;
        idx = dev->entry_buf.size - bto_copy;
        bto_copy = copy_from_user(dev->entry_buf.p + idx, buf, bto_copy);
    }
    retval = count;

    // If packet complete, add to ring_buf
    if (dev->entry_buf.p[dev->entry_buf.size - 1] == '\n') {
        // If ring_buf full, free entry @ write_pos & increment read pointer
        if (dev->count == SIZE_RING_BUF) {
            kfree(dev->ring_buf[dev->write_pos].p);
            dev->head = (dev->head + 1) % SIZE_RING_BUF;
        }
        // Update entry
        dev->ring_buf[dev->write_pos].p = dev->entry_buf.p;
        dev->ring_buf[dev->write_pos].size = dev->entry_buf.size;
        // Increment write pointer
        dev->write_pos = (dev->write_pos + 1) % SIZE_RING_BUF;
        // Increment count (num entries)
        if (dev->count < SIZE_RING_BUF) {
            dev->count++;
        }
        // Null entry_buf for next write
        dev->entry_buf.p = NULL;
        dev->entry_buf.size = 0;
    }
    mutex_unlock(&dev->lock);

    return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    // Get total size
    loff_t size = 0;
    struct entry cur_entry;
    int cur_entry_idx = dev->head;
    for (int i = 0; i < dev->count; i++) {
        cur_entry = dev->ring_buf[cur_entry_idx];
        size += cur_entry.size;
        cur_entry_idx = (cur_entry_idx + 1) % SIZE_RING_BUF;
    }

    // Get new f_pos
    PDEBUG("Offset %lld from %lld on file of size %lld", offset, filp->f_pos, size);
    loff_t retval = fixed_size_llseek(filp, offset, whence, size);

    mutex_unlock(&dev->lock);

    return retval;
}

static long aesd_adjust_file_offset(struct file *filp, unsigned int write_cmd, unsigned int write_cmd_offset)
{
    struct aesd_dev *dev = filp->private_data;

    if (write_cmd > SIZE_RING_BUF) {    // write_cmd out of range
        return -EINVAL;
    }

    long offset = 0;
    struct entry cur_entry;
    int cur_entry_idx = dev->head;
    // Add sizes of previous entries to offset
    for (int i = 0; i < write_cmd; i++) {
        cur_entry = dev->ring_buf[cur_entry_idx];
        offset += cur_entry.size;
        cur_entry_idx = (cur_entry_idx + 1) % SIZE_RING_BUF;
    }
    cur_entry = dev->ring_buf[cur_entry_idx];

    if (write_cmd_offset > cur_entry.size) {    // write_cmd_offset out of range
        return -EINVAL;
    }

    offset += write_cmd_offset;
    filp->f_pos = offset;

    return offset;
}

long aesd_ioctl(struct file *filp, unsigned int request, unsigned long arg)
{
    long retval;

    switch (request) {
        case AESDCHAR_IOCSEEKTO:
            struct aesd_seekto seekto;
            if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)) != 0) {
                retval = EFAULT;
            }
            else {
                retval = aesd_adjust_file_offset(filp, seekto.write_cmd, seekto.write_cmd_offset);
            }
            break;
        default:
            retval = EINVAL;
    }

    return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .llseek =   aesd_llseek,
    .unlocked_ioctl =   aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    mutex_init(&aesd_device.lock);
    aesd_device.count = 0;
    aesd_device.write_pos = 0;
    aesd_device.head = 0;
    aesd_device.entry_buf.p = NULL;
    aesd_device.entry_buf.size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */
    for (int i = 0; i < SIZE_RING_BUF; i++) {
        if (aesd_device.ring_buf[i].p != NULL) {
            kfree(aesd_device.ring_buf[i].p);
        }
    }

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
