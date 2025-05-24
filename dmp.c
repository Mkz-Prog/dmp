#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/stat.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Device Mapper Proxy Target with Statistics");

struct dmp_target
{
    struct dm_dev *dev;
    sector_t start;
};

struct dmp_global_stats
{
    atomic64_t read_reqs;
    atomic64_t write_reqs;
    atomic64_t read_bytes;
    atomic64_t write_bytes;
};

static struct dmp_global_stats global_stats;

static struct kobject *stat_kobj;

static ssize_t volumes_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    u64 read_reqs = atomic64_read(&global_stats.read_reqs);
    u64 write_reqs = atomic64_read(&global_stats.write_reqs);
    u64 read_bytes = atomic64_read(&global_stats.read_bytes);
    u64 write_bytes = atomic64_read(&global_stats.write_bytes);

    u64 total_reqs = read_reqs + write_reqs;
    u64 total_bytes = read_bytes + write_bytes;

    u64 read_avg_size = (read_reqs > 0) ? read_bytes / read_reqs : 0;
    u64 write_avg_size = (write_reqs > 0) ? write_bytes / write_reqs : 0;
    u64 total_avg_size = (total_reqs > 0) ? total_bytes / total_reqs : 0;

    return scnprintf(buf, PAGE_SIZE,
                     "read:\n"
                     "  reqs: %llu\n"
                     "  avg size: %llu\n"
                     "write:\n"
                     "  reqs: %llu\n"
                     "  avg size: %llu\n"
                     "total:\n"
                     "  reqs: %llu\n"
                     "  avg size: %llu\n",
                     read_reqs, read_avg_size,
                     write_reqs, write_avg_size,
                     total_reqs, total_avg_size);
}

static struct kobj_attribute volumes_attribute =
    __ATTR(volumes, S_IRUGO, volumes_show, NULL);

static struct attribute *stat_attrs[] = {
    &volumes_attribute.attr,
    NULL,
};

static struct attribute_group stat_attr_group = {
    .attrs = stat_attrs,
};

static int dmp_map(struct dm_target *ti, struct bio *bio, union map_info *map_context)
{
    struct dmp_target *dmp_t = (struct dmp_target *)ti->private;
    unsigned int bio_size = bio->bi_iter.bi_size;

    if (bio_data_dir(bio) == WRITE)
    {
        atomic64_inc(&global_stats.write_reqs);
        atomic64_add(bio_size, &global_stats.write_bytes);
    }
    else
    {
        atomic64_inc(&global_stats.read_reqs);
        atomic64_add(bio_size, &global_stats.read_bytes);
    }

    bio->bi_bdev = dmp_t->dev->bdev;

    return DM_MAPIO_REMAPPED;
}

static int dmp_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
    struct dmp_target *dmp_t;
    unsigned long long start;

    if (argc != 2)
    {
        ti->error = "dm-dmp: Invalid argument count. Expected 2 (device_path, offset)";
        printk(KERN_ERR "dm-dmp: Invalid argument count (%u)\n", argc);
        return -EINVAL;
    }

    dmp_t = kmalloc(sizeof(struct dmp_target), GFP_KERNEL);
    if (!dmp_t)
    {
        ti->error = "dm-dmp: Cannot allocate dmp_target context";
        printk(KERN_ERR "dm-dmp: kmalloc failed\n");
        return -ENOMEM;
    }

    if (sscanf(argv[1], "%llu", &start) != 1)
    {
        ti->error = "dm-dmp: Invalid device sector (offset)";
        printk(KERN_ERR "dm-dmp: Invalid offset argument: %s\n", argv[1]);
        goto bad;
    }
    dmp_t->start = (sector_t)start;

    if (dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &dmp_t->dev))
    {
        ti->error = "dm-dmp: Device lookup failed";
        printk(KERN_ERR "dm-dmp: dm_get_device failed for %s\n", argv[0]);
        goto bad;
    }

    ti->private = dmp_t;

    printk(KERN_INFO "dm-dmp: Device instance created for %s with offset %llu\n", argv[0], start);
    return 0;

bad:
    kfree(dmp_t);
    return -EINVAL;
}

static void dmp_dtr(struct dm_target *ti)
{
    struct dmp_target *dmp_t = (struct dmp_target *)ti->private;

    printk(KERN_INFO "dm-dmp: Device instance being destroyed.\n");

    dm_put_device(ti, dmp_t->dev);

    kfree(dmp_t);
}

static struct target_type dmp_target_type = {
    .name = "dmp",
    .version = {1, 0, 0},
    .module = THIS_MODULE,
    .ctr = dmp_ctr,
    .dtr = dmp_dtr,
    .map = dmp_map,
};

static int __init dmp_init(void)
{
    int result;
    struct kobject *module_kobj;

    printk(KERN_INFO "dm-dmp: Initializing module\n");

    atomic64_set(&global_stats.read_reqs, 0);
    atomic64_set(&global_stats.write_reqs, 0);
    atomic64_set(&global_stats.read_bytes, 0);
    atomic64_set(&global_stats.write_bytes, 0);
    printk(KERN_INFO "dm-dmp: Global stats initialized.\n");

    result = dm_register_target(&dmp_target_type);
    if (result < 0)
    {
        printk(KERN_ERR "dm-dmp: Error registering target: %d\n", result);
        return result;
    }
    printk(KERN_INFO "dm-dmp: Target 'dmp' registered.\n");

    module_kobj = &THIS_MODULE->mkobj.kobj;
    if (!module_kobj)
    {
        printk(KERN_ERR "dm-dmp: Failed to get module kobject.\n");
        goto sysfs_err;
    }

    stat_kobj = kobject_create_and_add("stat", module_kobj);
    if (!stat_kobj)
    {
        printk(KERN_ERR "dm-dmp: Failed to create stat kobject.\n");
        goto sysfs_err;
    }

    result = sysfs_create_group(stat_kobj, &stat_attr_group);
    if (result)
    {
        printk(KERN_ERR "dm-dmp: Failed to create sysfs group: %d\n", result);
        goto sysfs_err;
    }

    printk(KERN_INFO "dm-dmp: Sysfs entries created at /sys/module/dmp/stat/volumes.\n");
    return 0;

sysfs_err:

    if (stat_kobj)
    {
        sysfs_remove_group(stat_kobj, &stat_attr_group);
        kobject_put(stat_kobj);
    }

    dm_unregister_target(&dmp_target_type);
    printk(KERN_ERR "dm-dmp: Sysfs setup failed, module init aborted.\n");
    return -EFAULT;
}

static void __exit dmp_exit(void)
{
    printk(KERN_INFO "dm-dmp: Cleaning up module\n");

    sysfs_remove_group(stat_kobj, &stat_attr_group);
    kobject_put(stat_kobj);

    dm_unregister_target(&dmp_target_type);

    printk(KERN_INFO "dm-dmp: Module exited.\n");
}

module_init(dmp_init);
module_exit(dmp_exit);