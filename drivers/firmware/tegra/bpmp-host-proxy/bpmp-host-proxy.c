/**
 * 
 * NVIDIA BPMP Host Proxy Kernel Module
 * (c) 2023 Unikie, Oy
 * (c) 2023 Vadim Likholetov vadim.likholetov@unikie.com
 * 
*/
#include <linux/module.h>	  // Core header for modules.
#include <linux/device.h>	  // Supports driver model.
#include <linux/kernel.h>	  // Kernel header for convenient functions.
#include <linux/fs.h>		  // File-system support.
#include <linux/uaccess.h>	  // User access copy function support.
#include <linux/slab.h>
#include <soc/tegra/bpmp.h>
#include <linux/platform_device.h>
#include "bpmp-host-proxy.h"


#define DEVICE_NAME "bpmp-host"   // Device name.
#define CLASS_NAME  "chardrv"	  // < The device class -- this is a character device driver

MODULE_LICENSE("GPL");						 ///< The license type -- this affects available functionality
MODULE_AUTHOR("Vadim Likholetov");					 ///< The author -- visible when you use modinfo
MODULE_DESCRIPTION("NVidia BPMP Host Proxy Kernel Module"); ///< The description -- see modinfo
MODULE_VERSION("0.1");						 ///< A version number to inform users


#define BPMP_HOST_VERBOSE    1

#define BPMP_MSG_SIZE_MAX (1<<16)
#define BUF_SIZE 1024

/**
 * Put this flag in 0 in order that the BPMP host proxy only allows
 * the allowed BPMP resources to be used by the VMs.
 * 
 * Define this macro in order that the BPMP host proxy allows
 * all the BPMP resources to be accessible by the virtual machines.
 * This option is useful for debugging, but is INSECURE, and it could
 * stop the host. To avoid stop the host use 
 * "clk_ignore_unused pd_ignore_unused" in kernel command line
 * 
*/
/* #define BPMP_HOST_ALLOWS_ALL   1 */

#if BPMP_HOST_VERBOSE
#define deb_info(...)     printk(KERN_INFO DEVICE_NAME ": "__VA_ARGS__)
#else
#define deb_info(...)
#endif

#define deb_error(...)    printk(KERN_ALERT DEVICE_NAME ": "__VA_ARGS__)
#define deb_warn(...)     printk(KERN_WARNING DEVICE_NAME ": "__VA_ARGS__)

struct bpmp_proxy {
	struct tegra_bpmp_message *kbuf;
	void *txbuf;
	void *rxbuf;
};

extern int tegra_bpmp_transfer(struct tegra_bpmp *, struct tegra_bpmp_message *);
extern struct tegra_bpmp *tegra_bpmp_host_device;

/**
 * Important variables that store data and keep track of relevant information.
 */
static int major_number;

static struct class *bpmp_host_proxy_class = NULL;	///< The device-driver class struct pointer
static struct device *bpmp_host_proxy_device = NULL; ///< The device-driver device struct pointer

/**
 * Prototype functions for file operations.
 */
static int open(struct inode *, struct file *);
static int close(struct inode *, struct file *);
static ssize_t read(struct file *, char *, size_t, loff_t *);
static ssize_t write(struct file *, const char *, size_t, loff_t *);

/**
 * File operations structure and the functions it points to.
 */
static struct file_operations fops =
	{
		.owner = THIS_MODULE,
		.open = open,
		.release = close,
		.read = read,
		.write = write,
};

// BPMP allowed resources structure
static struct bpmp_allowed_res bpmp_ares; 

#if BPMP_HOST_VERBOSE
// Usage:
//     hexDump(desc, addr, len, perLine);
//         desc:    if non-NULL, printed as a description before hex dump.
//         addr:    the address to start dumping from.
//         len:     the number of bytes to dump.
//         perLine: number of bytes on each output line.
void static hexDump (
    const char * desc,
    const void * addr,
    const int len
) {
    // Silently ignore silly per-line values.

    int i;
    unsigned char buff[17];
	unsigned char out_buff[4000];
	unsigned char *p_out_buff = out_buff;
    const unsigned char * pc = (const unsigned char *)addr;



    // Output description if given.

    if (desc != NULL) printk ("%s:\n", desc);

    // Length checks.

    if (len == 0) {
        printk(DEVICE_NAME ":   ZERO LENGTH\n");
        return;
    }
    if (len < 0) {
        printk(DEVICE_NAME ":   NEGATIVE LENGTH: %d\n", len);
        return;
    }

	if(len > 400){
        printk(DEVICE_NAME ":   VERY LONG: %d\n", len);
        return;
    }

    // Process every byte in the data.

    for (i = 0; i < len; i++) {
        // Multiple of perLine means new or first line (with line offset).

        if ((i % 16) == 0) {
            // Only print previous-line ASCII buffer for lines beyond first.

            if (i != 0) {
				p_out_buff += sprintf (p_out_buff, "  %s\n", buff);
			}
            // Output the offset of current line.

            p_out_buff += sprintf (p_out_buff,"  %04x ", i);
        }

        // Now the hex code for the specific character.

        p_out_buff += sprintf (p_out_buff, " %02x", pc[i]);

        // And buffer a printable ASCII character for later.

        if ((pc[i] < 0x20) || (pc[i] > 0x7e)) // isprint() may be better.
            buff[i % 16] = '.';
        else
            buff[i % 16] = pc[i];
        buff[(i % 16) + 1] = '\0';
    }

    // Pad out last line if not exactly perLine characters.

    while ((i % 16) != 0) {
        p_out_buff += sprintf (p_out_buff, "   ");
        i++;
    }

    // And print the final ASCII buffer.

    p_out_buff += sprintf (p_out_buff, "  %s\n", buff);

	printk(DEVICE_NAME ": %s", out_buff);
}
#else
	#define hexDump(...)
#endif

static int concat_subnode_prop(struct device_node *np, const char *propname,
			       u32 *values, size_t max_size)
{
	struct device_node *child;
	int total_count = 0, count, ret;

	for_each_child_of_node(np, child) {
		if (!of_property_present(child, propname))
			continue;
		count = of_property_count_u32_elems(child, propname);
		if (count == 0)
			continue;
		if (count < 0)
			return count;

		if (total_count + count > max_size)
			return -ENOMEM;

		ret = of_property_read_u32_array(child, propname,
						 &values[total_count], count);
		if (ret)
			return ret;

		total_count += count;
	}

	return total_count;
}

static void dump_info(void)
{
	int i;

	deb_info("bpmp_ares.clocks_size: %d", bpmp_ares.clocks_size);
	for (i = 0; i < bpmp_ares.clocks_size; i++)
		deb_info("bpmp_ares.clock %d", bpmp_ares.clock[i]);

	deb_info("bpmp_ares.resets_size: %d", bpmp_ares.resets_size);
	for (i = 0; i < bpmp_ares.resets_size; i++)
		deb_info("bpmp_ares.reset %d", bpmp_ares.reset[i]);

	deb_info("bpmp_ares.pd_size: %d", bpmp_ares.pd_size);
	for (i = 0; i < bpmp_ares.pd_size; i++)
		deb_info("bpmp_ares.pd %d", bpmp_ares.pd[i]);
}

/**
 * Initializes module at installation
 */
static int bpmp_host_proxy_probe(struct platform_device *pdev)
{
	int ret;

	deb_info("%s, installing module.", __func__);

	ret = concat_subnode_prop(pdev->dev.of_node, "allowed-clocks",
				  bpmp_ares.clock, BPMP_HOST_MAX_CLOCKS_SIZE);
	if (ret < 0) {
		dev_err(&pdev->dev, "reading allowed-clocks failed with %d", ret);
		return -EINVAL;
	}
	bpmp_ares.clocks_size = ret;

	ret = concat_subnode_prop(pdev->dev.of_node, "allowed-resets",
				  bpmp_ares.reset, BPMP_HOST_MAX_RESETS_SIZE);
	if (ret < 0) {
		dev_err(&pdev->dev, "reading allowed-resets failed with %d", ret);
		return -EINVAL;
	}
	bpmp_ares.resets_size = ret;

	ret = concat_subnode_prop(pdev->dev.of_node, "allowed-power-domains",
				  bpmp_ares.pd, BPMP_HOST_MAX_POWER_DOMAINS_SIZE);
	if (ret < 0) {
		dev_err(&pdev->dev, "reading allowed-power-domains failed with %d", ret);
		return -EINVAL;
	}
	bpmp_ares.pd_size = ret;

	dump_info();

	// Allocate a major number for the device.
	major_number = register_chrdev(0, DEVICE_NAME, &fops);
	if (major_number < 0)
	{
		deb_error("could not register number.\n");
		return major_number;
	}
	deb_info("registered correctly with major number %d\n", major_number);

	// Register the device class
	bpmp_host_proxy_class = class_create(CLASS_NAME);
	if (IS_ERR(bpmp_host_proxy_class))
	{ // Check for error and clean up if there is
		unregister_chrdev(major_number, DEVICE_NAME);
		deb_error("Failed to register device class\n");
		return PTR_ERR(bpmp_host_proxy_class); // Correct way to return an error on a pointer
	}
	deb_info("device class registered correctly\n");

	// Register the device driver
	bpmp_host_proxy_device = device_create(bpmp_host_proxy_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
	if (IS_ERR(bpmp_host_proxy_device))
	{								 // Clean up if there is an error
		class_destroy(bpmp_host_proxy_class); 
		unregister_chrdev(major_number, DEVICE_NAME);
		deb_error("Failed to create the device\n");
		return PTR_ERR(bpmp_host_proxy_device);
	}

	deb_info("device class created correctly\n"); // Made it! device was initialized

	return 0;
}



/*
 * Removes module, sends appropriate message to kernel
 */
static int bpmp_host_proxy_remove(struct platform_device *pdev)
{
	deb_info("removing module.\n");
	device_destroy(bpmp_host_proxy_class, MKDEV(major_number, 0)); // remove the device
	class_unregister(bpmp_host_proxy_class);						  // unregister the device class
	class_destroy(bpmp_host_proxy_class);						  // remove the device class
	unregister_chrdev(major_number, DEVICE_NAME);		  // unregister the major number
	deb_info("Goodbye from the LKM!\n");
	unregister_chrdev(major_number, DEVICE_NAME);
	return 0;
}

/*
 * Opens device module, sends appropriate message to kernel
 */
static int open(struct inode *inodep, struct file *filep)
{
	struct bpmp_proxy *proxy;

	if (!tegra_bpmp_host_device)
		return -ENOENT;

	proxy = (struct bpmp_proxy *) kzalloc(sizeof(*proxy), GFP_KERNEL);
	if (!proxy)
		return -ENOMEM;

	proxy->kbuf = kzalloc(BPMP_MSG_SIZE_MAX, GFP_KERNEL);
	if (!proxy->kbuf)
		goto err_free_proxy;

	proxy->txbuf = kzalloc(BUF_SIZE, GFP_KERNEL);
	if (!proxy->txbuf)
		goto err_free_kbuf;

	proxy->rxbuf = kzalloc(BUF_SIZE, GFP_KERNEL);
	if (!proxy->rxbuf)
		goto err_free_txbuf;

	filep->private_data = proxy;

	return 0;

err_free_txbuf:
	kfree(proxy->txbuf);

err_free_kbuf:
	kfree(proxy->kbuf);

err_free_proxy:
	kfree(proxy);

	return -ENOMEM;
}

/*
 * Closes device module, sends appropriate message to kernel
 */
static int close(struct inode *inodep, struct file *filep)
{
	struct bpmp_proxy *proxy = filep->private_data;

	if (proxy) {
		kfree(proxy->rxbuf);
		kfree(proxy->txbuf);
		kfree(proxy->kbuf);
		kfree(proxy);
		filep->private_data = NULL;
	}

	return 0;
}

/*
 * Reads from device, displays in userspace, and deletes the read data
 */
static ssize_t read(struct file *filep, char *buffer, size_t len, loff_t *offset)
{
	deb_info("read stub");
	return 0;
}

static inline bool allow_reset(struct mrq_reset_request *req)
{
	int i;

	for (i = 0; i < bpmp_ares.resets_size; i++)
		if (bpmp_ares.reset[i] == req->reset_id)
			return true;

	deb_warn("reset not allowed for: %d", req->reset_id);

	return false;
}

static inline bool allow_clock(struct mrq_clk_request *req)
{
	int i;
	uint32_t cmd, id;

	/* TODO: check this */
	cmd = (req->cmd_and_id >> 24) & 0x000F;
	id = req->cmd_and_id & 0x0FFF;

	// If there is a get info command, allow it no matters the ID
	if (cmd == CMD_CLK_GET_MAX_CLK_ID ||
	    cmd == CMD_CLK_GET_ALL_INFO ||
	    cmd == CMD_CLK_GET_PARENT)
		return true;

	for (i = 0; i < bpmp_ares.clocks_size; i++)
		if (bpmp_ares.clock[i] == id)
			return true;

	deb_warn("clock not allowed for: %d, command: %d", id, cmd);

	return false;
}

static inline bool allow_pd(struct mrq_pg_request *req)
{
	int i;

	// If there is a get info command, allow it no matters the ID
	if (req->cmd == CMD_PG_GET_STATE ||
	    req->cmd == CMD_PG_GET_NAME ||
	    req->cmd == CMD_PG_GET_MAX_ID)
		return true;

	for (i = 0; i < bpmp_ares.pd_size; i++)
		if (bpmp_ares.pd[i] == req->id)
			return true;

	deb_warn("pg not allowed for: %d, command: %d", req->id, req->cmd);

	return false;
}

/*
 * Checks if the msg that wants to transmit through the
 * bpmp-host is allowed by the device tree configuration
 */
static bool check_if_allowed(struct tegra_bpmp_message *msg)
{
#ifdef BPMP_HOST_ALLOWS_ALL
	return true;
#endif

	/* Allow get information, DVFS, ISO Client and bandwidth mrqs */
	switch (msg->mrq) {
	case MRQ_PING:
	case MRQ_QUERY_TAG:
	case MRQ_THREADED_PING:
	case MRQ_QUERY_ABI:
	case MRQ_DEBUG:
	case MRQ_EMC_DVFS_LATENCY:
	case MRQ_EMC_DVFS_EMCHUB:
	case MRQ_ISO_CLIENT:
	case MRQ_STRAP:
	case MRQ_BWMGR:
	case MRQ_QUERY_FW_TAG:
		return true;
	case MRQ_RESET:
		return allow_reset((struct mrq_reset_request *) msg->tx.data);
	case MRQ_CLK:
		return allow_clock((struct mrq_clk_request *) msg->tx.data);
	case MRQ_PG:
		return allow_pd((struct mrq_pg_request *) msg->tx.data);
	default:
		break;
	}

	deb_warn("Warning, msg->mrq %d not allowed", msg->mrq);

	return false;
}

/*
 * Writes to the device
 */
static ssize_t write(struct file *filep, const char *buffer, size_t len, loff_t *offset)
{
	int ret;
	struct tegra_bpmp_message *kbuf;
	void *user_txbuf;
	void *user_rxbuf;
	struct bpmp_proxy *proxy = filep->private_data;

	if (len >= BPMP_MSG_SIZE_MAX)
		return -EINVAL;

	kbuf = proxy->kbuf;

	memset(kbuf, 0, len);
	memset(proxy->txbuf, 0, BUF_SIZE);
	memset(proxy->rxbuf, 0, BUF_SIZE);

	// Copy header
	if (copy_from_user(kbuf, buffer, len))
		return -EFAULT;

	deb_info("\nwants to write %zu bytes, with mrq: %d\n", len, kbuf->mrq);
	
	if (kbuf->tx.size > 0) {
		if (copy_from_user(proxy->txbuf, kbuf->tx.data, kbuf->tx.size))
			return -EFAULT;
	}

	if (copy_from_user(proxy->rxbuf, kbuf->rx.data, kbuf->rx.size))
		return -EFAULT;

	user_txbuf = (void *)kbuf->tx.data;
	user_rxbuf = kbuf->rx.data;

	kbuf->tx.data = proxy->txbuf;
	kbuf->rx.data = proxy->rxbuf;

	if (!check_if_allowed(kbuf))
		return -EPERM;

	hexDump (DEVICE_NAME ": kbuf", kbuf, len);
	hexDump (DEVICE_NAME ": txbuf", proxy->txbuf, kbuf->tx.size);

	ret = tegra_bpmp_transfer(tegra_bpmp_host_device,
				  (struct tegra_bpmp_message *)kbuf);
	if (ret)
		return ret;
	if (((struct tegra_bpmp_message *)kbuf)->rx.ret < 0)
		return -EINVAL;

	if (copy_to_user((void *)user_txbuf, kbuf->tx.data, kbuf->tx.size)) 
		return -EFAULT;

	if (copy_to_user((void *)user_rxbuf, kbuf->rx.data, kbuf->rx.size))
		return -EFAULT;

	kbuf->tx.data = user_txbuf;
	kbuf->rx.data = user_rxbuf;
	
	if (copy_to_user((void *)buffer, kbuf, len))
		return -EFAULT;

	return len;
}

static const struct of_device_id bpmp_host_proxy_ids[] = {
	{ .compatible = "nvidia,bpmp-host-proxy" },
	{ }
};

static struct platform_driver bpmp_host_proxy_driver = {
	.driver = {
		.name = "bpmp_host_proxy",
		.of_match_table = bpmp_host_proxy_ids,
	},
	.probe = bpmp_host_proxy_probe,
	.remove = bpmp_host_proxy_remove,
};
builtin_platform_driver(bpmp_host_proxy_driver);
