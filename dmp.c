#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/slab.h> // Для kmalloc/kfree
#include <linux/atomic.h> // Для атомарных счетчиков
#include <linux/kobject.h> // Для sysfs
#include <linux/sysfs.h> // Для sysfs
#include <linux/stat.h> // Для прав доступа S_IRUGO

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name"); // Укажи свое имя
MODULE_DESCRIPTION("Device Mapper Proxy Target with Statistics");

/*
 * Структура для хранения данных конкретного экземпляра dm-устройства
 * с нашим таргетом.
 */
struct dmp_target {
        struct dm_dev *dev; // Нижележащее устройство
        sector_t start;     // Смещение на нижележащем устройстве (не используется в этой версии)
        // Статистика перенесена в глобальную структуру для агрегации
};

/*
 * Глобальная структура для хранения суммарной статистики
 * по всем активным dm-устройствам с нашим таргетом.
 * Используем атомарные типы для безопасного доступа из разных контекстов.
 */
struct dmp_global_stats {
    atomic64_t read_reqs;
    atomic64_t write_reqs;
    atomic64_t read_bytes; // Суммарный размер данных в запросах чтения
    atomic64_t write_bytes; // Суммарный размер данных в запросах записи
};

static struct dmp_global_stats global_stats; // Экземпляр глобальной статистики

/*
 * kobject для каталога /sys/module/dmp/stat
 */
static struct kobject *stat_kobj;

/*
 * Функция отображения статистики для sysfs файла volumes
 */
static ssize_t volumes_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    u64 read_reqs = atomic64_read(&global_stats.read_reqs);
    u64 write_reqs = atomic64_read(&global_stats.write_reqs);
    u64 read_bytes = atomic64_read(&global_stats.read_bytes);
    u64 write_bytes = atomic64_read(&global_stats.write_bytes);

    u64 total_reqs = read_reqs + write_reqs;
    u64 total_bytes = read_bytes + write_bytes;

    // Вычисляем средний размер, избегая деления на ноль
    u64 read_avg_size = (read_reqs > 0) ? read_bytes / read_reqs : 0;
    u64 write_avg_size = (write_reqs > 0) ? write_bytes / write_reqs : 0;
    u64 total_avg_size = (total_reqs > 0) ? total_bytes / total_reqs : 0;

    // Форматируем вывод в буфер
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

/*
 * Определение sysfs атрибута "volumes"
 */
static struct kobj_attribute volumes_attribute =
    __ATTR(volumes, S_IRUGO, volumes_show, NULL); // Имя файла "volumes", права только на чтение

/*
 * Массив атрибутов в каталоге stat
 */
static struct attribute *stat_attrs[] = {
    &volumes_attribute.attr,
    NULL, // Обязательный завершающий элемент
};

/*
 * Группа атрибутов для каталога stat
 */
static struct attribute_group stat_attr_group = {
    .attrs = stat_attrs,
};

/*
 * Функция map Device Mapper таргета.
 * Вызывается для каждого I/O запроса к dm-устройству.
 * Исправлена сигнатура для совместимости с заголовками ядра 6.1.0-37-amd64,
 * убран submit_bio и добавлен возврат DM_MAPIO_REMAPPED.
 */
static int dmp_map(struct dm_target *ti, struct bio *bio) // <--- Сигнатура с 2 аргументами
{
    // <-- Это строка 109 в вашем коде
    struct dmp_target *dmp_t = (struct dmp_target *) ti->private;

    unsigned int bio_size = bio->bi_iter.bi_size; // Размер данных в текущем сегменте bio в байтах

    // Обновляем глобальную статистику
    if (bio_data_dir(bio) == WRITE) {
        atomic64_inc(&global_stats.write_reqs);
        atomic64_add(bio_size, &global_stats.write_bytes);
    } else {
        atomic64_inc(&global_stats.read_reqs);
        atomic64_add(bio_size, &global_stats.read_bytes);
    }

    // Перенаправляем bio на нижележащее устройство
    // === ЭТА СТРОКА ИСПОЛЬЗУЕТ 'dmp_t', УБЕРИТЕСЬ, ЧТО ОНА ПРИСУТСТВУЕТ! ===
    bio->bi_bdev = dmp_t->dev->bdev;

    // Если бы использовалось смещение:
    // bio->bi_iter.bi_sector = dmp_t->start + bio->bi_iter.bi_sector; // Эта строка тоже использует 'dmp_t'

    // Уведомляем Device Mapper, что bio был изменен и готов к отправке
    return DM_MAPIO_REMAPPED;
}


/*
 * Функция-конструктор Device Mapper таргета.
 * Вызывается при dmsetup create ... dmp ...
 */
static int dmp_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
    struct dmp_target *dmp_t;
    unsigned long long start;

    if (argc != 2) {
        ti->error = "dm-dmp: Invalid argument count. Expected 2 (device_path, offset)";
        printk(KERN_ERR "dm-dmp: Invalid argument count (%u)\n", argc);
        return -EINVAL;
    }

    dmp_t = kmalloc(sizeof(struct dmp_target), GFP_KERNEL);
    if (!dmp_t) {
        ti->error = "dm-dmp: Cannot allocate dmp_target context";
        printk(KERN_ERR "dm-dmp: kmalloc failed\n");
        return -ENOMEM;
    }

    // Читаем смещение (хотя и не используем его в map функции)
    if (sscanf(argv[1], "%llu", &start) != 1) {
        ti->error = "dm-dmp: Invalid device sector (offset)";
        printk(KERN_ERR "dm-dmp: Invalid offset argument: %s\n", argv[1]);
        goto bad;
    }
    dmp_t->start = (sector_t)start;

    // Получаем ссылку на нижележащее устройство
    if (dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &dmp_t->dev)) {
        ti->error = "dm-dmp: Device lookup failed";
        printk(KERN_ERR "dm-dmp: dm_get_device failed for %s\n", argv[0]);
        goto bad;
    }

    ti->private = dmp_t; // Сохраняем наш контекст в dm_target

    printk(KERN_INFO "dm-dmp: Device instance created for %s with offset %llu\n", argv[0], start);
    return 0; // Успех

bad:
    kfree(dmp_t); // Освобождаем память при ошибке
    return -EINVAL;
}

/*
 * Функция-деструктор Device Mapper таргета.
 * Вызывается при dmsetup remove ...
 */
static void dmp_dtr(struct dm_target *ti)
{
    struct dmp_target *dmp_t = (struct dmp_target *) ti->private;

    printk(KERN_INFO "dm-dmp: Device instance being destroyed.\n");

    // Освобождаем ссылку на нижележащее устройство
    dm_put_device(ti, dmp_t->dev);
    // Освобождаем память, выделенную для нашего контекста
    kfree(dmp_t);
}

/*
 * Определение нашего типа Device Mapper таргета.
 */
static struct target_type dmp_target_type = {
    .name = "dmp",          // Имя таргета, используемое в dmsetup
    .version = {1, 0, 0},
    .module = THIS_MODULE,
    .ctr = dmp_ctr,
    .dtr = dmp_dtr,
    .map = dmp_map, // <--- Теперь указывает на функцию с 2 аргументами
};

/*
 * Функция инициализации модуля.
 * Вызывается при insmod.
 */
static int __init dmp_init(void)
{
    int result;
    struct kobject *module_kobj;

    printk(KERN_INFO "dm-dmp: Initializing module\n");

    // Инициализируем глобальную статистику нулями
    atomic64_set(&global_stats.read_reqs, 0);
    atomic64_set(&global_stats.write_reqs, 0);
    atomic64_set(&global_stats.read_bytes, 0);
    atomic64_set(&global_stats.write_bytes, 0);
    printk(KERN_INFO "dm-dmp: Global stats initialized.\n");

    // Регистрируем наш Device Mapper таргет
    result = dm_register_target(&dmp_target_type);
    if (result < 0) {
        printk(KERN_ERR "dm-dmp: Error registering target: %d\n", result);
        return result; // Возвращаем код ошибки
    }
    printk(KERN_INFO "dm-dmp: Target 'dmp' registered.\n");

    // Создаем sysfs интерфейс
    // Получаем kobject модуля (/sys/module/dmp)
    module_kobj = &THIS_MODULE->mkobj.kobj;
    if (!module_kobj) {
        printk(KERN_ERR "dm-dmp: Failed to get module kobject.\n");
        // Даже если не смогли создать sysfs, пробуем зарегистрировать таргет.
        // Но по заданию sysfs нужен, поэтому при ошибке sysfs, init должен фейлиться.
        goto sysfs_err;
    }

    // Создаем каталог 'stat' под /sys/module/dmp/
    stat_kobj = kobject_create_and_add("stat", module_kobj);
    if (!stat_kobj) {
        printk(KERN_ERR "dm-dmp: Failed to create stat kobject.\n");
        goto sysfs_err;
    }

    // Создаем файл 'volumes' в каталоге 'stat'
    result = sysfs_create_group(stat_kobj, &stat_attr_group);
    if (result) {
        printk(KERN_ERR "dm-dmp: Failed to create sysfs group: %d\n", result);
        goto sysfs_err;
    }

    printk(KERN_INFO "dm-dmp: Sysfs entries created at /sys/module/dmp/stat/volumes.\n");
    return 0; // Успех инициализации модуля

sysfs_err:
    // Очистка при ошибке создания sysfs (удаляем созданные kobjects/файлы)
    if (stat_kobj) {
        sysfs_remove_group(stat_kobj, &stat_attr_group);
        kobject_put(stat_kobj); // Удаляет kobject и каталог
        stat_kobj = NULL; // Сбрасываем указатель
    }
    // Снимаем регистрацию таргета, если она была успешна до sysfs ошибки
    dm_unregister_target(&dmp_target_type); // Эта функция безопасна, даже если таргет не был зарегистрирован
    printk(KERN_ERR "dm-dmp: Sysfs setup failed, module init aborted.\n");
    return -EFAULT; // Код ошибки
}

/*
 * Функция завершения работы модуля.
 * Вызывается при rmmod.
 */
static void __exit dmp_exit(void)
{
    printk(KERN_INFO "dm-dmp: Cleaning up module\n");

    // Удаляем sysfs записи (в порядке, обратном созданию)
    // Проверяем stat_kobj, т.к. он мог не создаться при ошибке в init
    if (stat_kobj) {
        sysfs_remove_group(stat_kobj, &stat_attr_group);
        kobject_put(stat_kobj); // Удаляет kobject и каталог stat
        stat_kobj = NULL;
    }


    // Снимаем регистрацию нашего Device Mapper таргета
    dm_unregister_target(&dmp_target_type);

    printk(KERN_INFO "dm-dmp: Module exited.\n");
}

// Регистрация функций инициализации и завершения работы модуля
module_init(dmp_init);
module_exit(dmp_exit);