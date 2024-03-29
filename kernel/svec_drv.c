/**
* svec_drv.c
* Driver for SVEC carrier.
* 
* Released under GPL v2. (and only v2, not any later version)
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include "svec.h"
#include "xloader_regs.h"

#define DRIVER_NAME	"svec"
#define PFX		DRIVER_NAME ": "
#define BASE_LOADER 0x70000
#define MS_TICKS 1
#define TIMEOUT 1000

/* Module parameters */
char *svec_fw_name = "fmc/svec-init.bin";
/*
module_param_named(fw_name, svec_fw_name, charp, 0444);
*/
static long vmebase1[SVEC_MAX_DEVICES];
static unsigned int vmebase1_num;
static long vmebase2[SVEC_MAX_DEVICES];
static unsigned int vmebase2_num;
static char *fw_name[SVEC_MAX_DEVICES];
static unsigned int fw_name_num;
static int  vector[SVEC_MAX_DEVICES];
static unsigned int vector_num;
static int  level[SVEC_MAX_DEVICES];
static unsigned int level_num;
static int lun[SVEC_MAX_DEVICES] = SVEC_DEFAULT_IDX;
static unsigned int lun_num;


module_param_array(vmebase1, ulong, &vmebase1_num, S_IRUGO);
MODULE_PARM_DESC(vmebase1, "VME Base Adress #1 of the SVEC card");
module_param_array(vmebase2, ulong, &vmebase2_num, S_IRUGO);
MODULE_PARM_DESC(vmebase2, "VME Base Adress #2 of the SVEC card");
module_param_array_named(fw_name, fw_name , charp, &fw_name_num, S_IRUGO);
MODULE_PARM_DESC(fw_name, "firmware file");
module_param_array(vector, int, &vector_num, S_IRUGO);
MODULE_PARM_DESC(vector, "IRQ vector");
module_param_array(level, int, &level_num, S_IRUGO);
MODULE_PARM_DESC(level, "IRQ level");
module_param_array(lun, int, &lun_num, S_IRUGO);
MODULE_PARM_DESC(lun, "Index value for SVEC card");

/* For device creation. Not really sure if it's necessary... */
static dev_t svec_devno;
static struct class *svec_class;

static const struct file_operations svec_fops = {
	.owner = THIS_MODULE,
};

static inline unsigned short swapbe16(unsigned short val)
{
        return (((val & 0xff00) >> 8) | ((val & 0xff) << 8));
}

static inline unsigned int swapbe32(unsigned int val)
{
        return (((val & 0xff000000) >> 24) | ((val & 0xff0000) >> 8) |
                ((val & 0xff00) << 8) | ((val & 0xff) << 24));
}

int map_cr_csr(struct svec_dev *svec)
{
	int rval;

	if (svec->cr_csr != NULL) {
		printk(KERN_ERR PFX "Error: CS/CSR already mapped?\n");
		return -EPERM;
	}
	svec->cr_csr = kzalloc(sizeof(struct vme_mapping), GFP_KERNEL);
	if (!svec->cr_csr) {
		printk(KERN_ERR PFX "kzalloc failed allocating memory for vme_mapping struct\n");
		return -ENOMEM;
	}

	/* CS/CSR mapping*/
	svec->cr_csr->am = 		VME_CR_CSR; /* 0x2f */
	svec->cr_csr->data_width = 	VME_D32;
	svec->cr_csr->vme_addru =	0;
	svec->cr_csr->vme_addrl =	svec->vmebase1;
	svec->cr_csr->sizeu =		0;
	svec->cr_csr->sizel = 	0x80000;
	svec->cr_csr->window_num =	0;
	svec->cr_csr->bcast_select = 0;
	svec->cr_csr->read_prefetch_enabled = 0;

	printk(KERN_ERR PFX "Mapping CR/CSR space\n");
	if (( rval = vme_find_mapping(svec->cr_csr, 1)) != 0) {
        	 printk(KERN_ERR PFX "Failed to map CS_CSR (%d)\n", rval);
	         return -EINVAL;
	}
	printk(KERN_ERR PFX "CR/CSR mapping successful at 0x%p\n", svec->cr_csr->kernel_va);

	return 0;
}

int unmap_cr_csr(struct svec_dev *svec) {

	if (vme_release_mapping(svec->cr_csr, 1)) {
		printk(KERN_ERR PFX "Unmap CR/CSR window failed\n");
		return -EINVAL;
	}
	printk(KERN_ERR PFX "CR/CSR window unmaped\n");
	return 0;
}


#define FUN0ADER  (0x7FF63)
#define INT_LEVEL (0x7ff5b)
#define INTVECTOR (0x7ff5f)
#define WB_32_64 (0x7ff33)
#define BIT_SET_REG (0x7FFFB)
#define BIT_CLR_REG (0x7FFF7)
#define WB32 1
#define WB64 0
#define RESET_CORE 0x80
#define ENABLE_CORE 0x10

int svec_bootloader_unlock (struct svec_dev *svec)
{
	const uint32_t boot_seq[8] = {0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe};
	void *addr;
	int i;

	/* Check if CS/CSR window is mapped */
	if (svec->cr_csr == NULL) {
		printk(KERN_ERR PFX "CS/CSR window not found\n");
		return -EINVAL;
	}

	addr = svec->cr_csr->kernel_va + BASE_LOADER + XLDR_REG_BTRIGR;

	/* Magic sequence: unlock bootloader mode, disable application FPGA */
	for (i=0; i<8; i++)
		iowrite32(swapbe32(boot_seq[i]), addr);

	printk(KERN_ERR PFX "wrote unlock sequence at %x\n", (unsigned int)addr);

	return 0;
}

int svec_bootloader_is_active(struct svec_dev *svec)
{
	uint32_t idc;
	char *buf = (char *)&idc;
	void *addr;

	/* Check if CS/CSR window is mapped */
	if (svec->cr_csr == NULL) {
		printk(KERN_ERR PFX "CS/CSR window not found\n");
		return -EINVAL;
	}

	addr = svec->cr_csr->kernel_va + BASE_LOADER + XLDR_REG_IDR;

	idc = swapbe32(ioread32(addr));
	idc = htonl(idc);
    

	if(strncmp(buf, "SVEC", 4) == 0)
	{
		printk(KERN_INFO PFX "IDCode value %x [%s].\n", idc, buf);
		/* Bootloader active. Unlocked */ 
		return 1;
	}
	else
		printk(KERN_INFO PFX "IDCode value %x.\n", idc);

	/* Bootloader not active. Locked */
	return 0;
}

static u8 csr_read(void *base, u32 offset)
{
	char *p = base;
	u8 value;

	offset -= offset % 4;
	value = ioread32be(p + offset);
	return value;
}

static void csr_write(u8 value, void *base, u32 offset)
{
	offset -= offset % 4;
	iowrite32be(value, base + offset);
}

void setup_csr_fa0(void *base, u32 vme, unsigned vector, unsigned level)
{
	u8 fa[4];		/* FUN0 ADER contents */

	/* reset the core */
	csr_write(RESET_CORE, base, BIT_SET_REG);
	msleep(10);

	/* disable the core */
	csr_write(ENABLE_CORE, base, BIT_CLR_REG);

	/* default to 32bit WB interface */
	csr_write(WB32, base, WB_32_64);

	/* set interrupt vector and level */
	csr_write(vector, base, INTVECTOR);
	csr_write(level, base, INT_LEVEL);

	/* do address relocation for FUN0 */
	fa[0] = (vme >> 24) & 0xFF;
	fa[1] = (vme >> 16) & 0xFF;
	fa[2] = (vme >> 8 ) & 0xFF;
	fa[3] = (VME_A32_USER_DATA_SCT & 0x3F) << 2;
			/* DFSR and XAM are zero */

	csr_write(fa[0], base, FUN0ADER);
	csr_write(fa[1], base, FUN0ADER + 4);
	csr_write(fa[2], base, FUN0ADER + 8);
	csr_write(fa[3], base, FUN0ADER + 12);

	/* enable module, hence make FUN0 available */
	csr_write(ENABLE_CORE, base, BIT_SET_REG);

	pr_debug("base: %02x%02x%02x%02x\n", csr_read(base, FUN0ADER),
		csr_read(base, FUN0ADER+4), csr_read(base, FUN0ADER+8),
		csr_read(base, FUN0ADER+12));
	pr_debug("vector: %02x\n", csr_read(base, INTVECTOR));
	pr_debug("level: %02x\n", csr_read(base, INT_LEVEL));
	pr_debug("int_enable: %02x\n", csr_read(base, 0x7ff57));
	pr_debug("wb32: %02x\n", csr_read(base, WB_32_64));
	pr_debug("vme_core: %02x\n", csr_read(base, 0x7c));
}

int svec_load_fpga(struct svec_dev *svec, const void *blob, int size)
{

	const uint32_t *data = blob;
	void *loader_addr; /* FPGA loader virtual address */
	uint32_t n;
	uint32_t rval;
	int xldr_fifo_r0;  /* Bitstream data input control register */
	int xldr_fifo_r1;  /* Bitstream data input register */
	int i;
	int err = 0;

	/* Check if we have something to do... */
	if ((data == NULL) || (size == 0)){
		printk(KERN_ERR PFX "%s: data to be load is NULL\n", __func__);
		return -EINVAL;
	}

	/* Check if CS/CSR window is mapped */
	if (svec->cr_csr == NULL) {
		printk(KERN_ERR PFX "CS/CSR window not found\n");
		return -EINVAL;
	}

	/* Unlock (activate) bootloader */
	if (svec_bootloader_unlock(svec)) {
		printk(KERN_ERR PFX "Bootloader unlock failed\n");
		return -EINVAL;
	}

	/* Check if bootloader is active */
	if (!svec_bootloader_is_active(svec)) {
		printk(KERN_ERR PFX "Bootloader locked after unlock!\n");
		return -EINVAL;
	}

	/* FPGA loader virtual address */
	loader_addr = svec->cr_csr->kernel_va + BASE_LOADER;

	iowrite32(swapbe32(XLDR_CSR_SWRST), loader_addr + XLDR_REG_CSR); 
	iowrite32(swapbe32(XLDR_CSR_START | XLDR_CSR_MSBF), loader_addr + XLDR_REG_CSR); 

	i = 0;
	while(i < size) {
		rval = swapbe32(ioread32(loader_addr + XLDR_REG_FIFO_CSR));
		if (!(rval & XLDR_FIFO_CSR_FULL)) {
			n = (size-i>4?4:size-i);
			xldr_fifo_r0 = (n - 1) | ((n<4) ? XLDR_FIFO_R0_XLAST : 0);
			xldr_fifo_r1 = htonl(data[i>>2]); 

			iowrite32(swapbe32(xldr_fifo_r0), loader_addr + XLDR_REG_FIFO_R0);
			iowrite32(swapbe32(xldr_fifo_r1), loader_addr + XLDR_REG_FIFO_R1);
    			i+=n;
		}
	}

	while(1) 
	{
		rval = swapbe32(ioread32(loader_addr + XLDR_REG_CSR));
		if(rval & XLDR_CSR_DONE) {
			err = rval & XLDR_CSR_ERROR ? -EINVAL : 0;
    	    		printk(KERN_ERR PFX "Bitstream loaded, status: %s\n", 
				(err < 0 ? "ERROR" : "OK"));

			/* give the VME bus control to App FPGA */
			pr_debug("giving up control to app FPGA\n");
			iowrite32(swapbe32(XLDR_CSR_EXIT), loader_addr + XLDR_REG_CSR);
			break;
		}
	}

	return err;
}

static int __devexit svec_remove(struct device *pdev, unsigned int ndev)
{
	struct svec_dev *card = dev_get_drvdata(pdev); 
	
	printk(KERN_ERR PFX "%s\n", __func__);
	
	/*
	int ret;
	dev_t devno = MKDEV(MAJOR(svec_devno), 0);

	ret = vme_free_irq(vector[ndev]);
	if (ret)
		dev_warn(pdev, "Cannot free irq %d, err %d\n", vector[ndev], ret);
	*/
	/* avoid deleting unregistered devices */
	/*
	if (card) {
		if (card->cdev.owner == THIS_MODULE) {
			printk(KERN_ERR PFX "unregister_chrdev_region\n");
			unregister_chrdev_region(devno, lun_num);
		}
	}
	else
		printk(KERN_ERR PFX "card is null!\n");
	*/
	
	unmap_cr_csr(card);

	return 0;
}
/*
static int svec_irq(void *arg)
{
	return 0;
}
*/
static int __devinit svec_match(struct device *pdev, unsigned int ndev)
{
	printk(KERN_ERR PFX "%s\n", __func__);
	/* TODO */

	if (ndev >= vmebase1_num)
        	 return 0;
	if (ndev >= vector_num || ndev >= level_num) {
        	 dev_warn(pdev, "irq vector/level missing\n");
	         return 0;
	}

	return 1;
}

int svec_load_fpga_file(struct svec_dev *svec, const char *name)
{
	struct device *dev = svec->dev;
	const struct firmware *fw;
	int err = 0;

	dev_info(dev, "%s: file %s\n", __func__, name);

	if (name == NULL) {
		printk (KERN_ERR PFX "%s: file name is NULL\n", __func__);
	}

	err = request_firmware(&fw, name, dev);

	if (err < 0) {
        	dev_err(dev, "request firmware \"%s\": error %i\n", name, err);
	        return err;
	}
	printk (KERN_ERR PFX "got file \"%s\", %i (0x%x) bytes\n",
        	name, fw->size, fw->size);

	err = svec_load_fpga(svec, (uint32_t *)fw->data, fw->size);
	release_firmware(fw);

	return err;
}

static int __devinit svec_probe(struct device *pdev, unsigned int ndev)
{
	struct svec_dev *svec;
	const char *name;
	dev_t devno;
	int error = 0;

	if (lun[ndev] >= SVEC_MAX_DEVICES) {
		printk(KERN_ERR PFX "Card lun %d out of range [0..%d]\n",
			lun[ndev], SVEC_MAX_DEVICES -1);
		return -EINVAL;
	}

	printk (KERN_ERR PFX "probe for device %02d\n", ndev);

	svec = kzalloc(sizeof(*svec), GFP_KERNEL);
	if (svec == NULL) {
		printk(KERN_ERR PFX "Cannot allocate memory for svec card struct\n");
		return -ENOMEM;
	}
	
	/* Initialize struct fields*/
	svec->lun = lun[ndev];
	svec->vmebase1 = vmebase1[ndev];
	svec->vmebase2 = vmebase2[ndev];
	svec->vector = vector[ndev];
	svec->level = level[ndev];
	svec->fw_name = fw_name[ndev];
	svec->dev = pdev;
	svec->cr_csr = NULL;

	/* Map CS/CSR space */
	error = map_cr_csr(svec);
	if (error)
		goto failed;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
	name = pdev->bus_id;
#else
        name = dev_name(pdev);
#endif
	strlcpy(svec->driver, DRIVER_NAME, sizeof(svec->driver));
	snprintf(svec->description, sizeof(svec->description),
		"SVEC at VME-A32 0x%08lx - 0x%08lx irqv %d irql %d",
		svec->vmebase1, svec->vmebase2, vector[ndev], level[ndev]);

	printk (KERN_ERR PFX "%s\n", svec->description);

	/*Create cdev, just to know the carrier is there */
	devno = MKDEV(MAJOR(svec_devno), ndev);

	cdev_init(&svec->cdev, &svec_fops);
	svec->cdev.owner = THIS_MODULE;
	error = cdev_add(&svec->cdev, devno, 1);
	if (error) {
		dev_err(pdev, "Error %d adding cdev %d\n", error, ndev);
		goto failed;
	}
/*
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
        svec->dev = device_create(svec_class, pdev, devno, NULL, "svec.%i", ndev);
#else
        svec->dev = device_create(svec_class, pdev, devno, "svec.%i", ndev);
#endif 

	if (IS_ERR(pdev)) {
		error = PTR_ERR(svec->dev);
		dev_err(pdev, "Error %d creating device %d\n", error, ndev);
		svec->dev = NULL;
		goto device_create_failed;
	}
*/

	dev_set_drvdata(svec->dev, svec);
	error = svec_create_sysfs_files(svec);
	if (error) {
		printk(KERN_ERR PFX "Error creating sysfs files\n");
		goto failed;
	}

	/* Load the golden FPGA binary to read the eeprom */
	error = svec_load_fpga_file(svec, svec->fw_name);
	if (error)
		goto failed;

	/* configure and activate function 0 */
	setup_csr_fa0(svec->cr_csr->kernel_va, vmebase2[ndev],
				vector[ndev], level[ndev]);
	
	return 0;
/*
device_create_failed:
	cdev_del(&svec->cdev);
 */

failed:
	kfree(svec);
	return -EINVAL;
}

static struct vme_driver svec_driver = {
        .match          = svec_match,
        .probe          = svec_probe,
        .remove         = __devexit_p(svec_remove),
        .driver         = {
       		.name   = DRIVER_NAME,
        },
};

static int __init svec_init(void)
{
	int error = 0;

	/* TODO: Check parameters */

	/* Device creation for the carrier, just in case... */
	error = alloc_chrdev_region(&svec_devno, 0, lun_num, "svec");
	if (error) {
		printk(KERN_ERR PFX "Failed to allocate chrdev region\n");
		goto out;
	}
		
	svec_class = class_create(THIS_MODULE, "svec");
	if (IS_ERR(svec_class)) {
		printk(KERN_ERR PFX "Failed to create svec class\n");
		error = PTR_ERR(svec_class);
		goto out;
	}
        
	error = vme_register_driver(&svec_driver, lun_num);
	if (error) {
		printk(KERN_ERR PFX "Cannot register vme driver - lun [%d]\n", lun_num);
		class_destroy(svec_class);
	}

out:
	return error;
}

static void __exit svec_exit(void)
{
	pr_debug("%s\n", __func__);
	vme_unregister_driver(&svec_driver);
	class_destroy(svec_class);
	unregister_chrdev_region(svec_devno, lun_num);
	pr_debug(PFX "class_destroy\n");
}


module_init(svec_init);
module_exit(svec_exit);

MODULE_AUTHOR("");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("svec driver");
