/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (c) 2017 Aligned Vision, Inc.  All rights reserved.
 *
 * Description:
 *	Aligned Vision LaserVision2 driver.  Refer to laser_api.h for commands
 *      available to control the LaserVision2 device.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/interrupt.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/laser_api.h>
#include <linux/laser_dev.h>
#include <linux/serial_reg.h>

#define LG_VERSION 	 "0.4"
#define DEV_NAME_LV2   	 "lv2"

enum point_type {
  LV2_XPOINT=1,
  LV2_YPOINT,
  LV2_XYPOINT,
};

struct lv2_dev {
  // dev stuff first
  struct miscdevice       miscdev;
  struct mutex            lg_mutex;
  spinlock_t              lock;
  struct kref             ref;
  wait_queue_head_t       wait;
  struct list_head        free;
  struct list_head        used;
  struct completion       completion;
  struct device           *dev;
  void __iomem	          *iobase;
  struct dentry           *dbg_entry;
  // Start of actual lv2 private data
  struct timeval          last_ts;
  uint8_t                 *pSenseBuff;
  uint32_t                sense_buff_size;
  uint8_t                 pad[3];
};

static inline void lv2_get_xydata_ltcval(int16_t *output_val, int16_t input_val)
{
  if (!input_val)                                       // Value 0 is represented as 0x8000 by LTC1597
    *output_val = LTC1597_BIPOLAR_OFFSET_PLUS;
  else if (input_val < 0)                               // Negative values are 0x1->0x7FFF 
    *output_val = (input_val & LTC1597_BIPOLAR_OFFSET_NEG);
  else
    {
      if (input_val >= LTC1597_BIPOLAR_OFFSET_PLUS)    // Positive values are 0x8000->0xFFFF
	*output_val = input_val;
      else
	*output_val = input_val | LTC1597_BIPOLAR_OFFSET_PLUS;
    }
  return;
}
static inline void lv2_send_to_dac(int16_t point, uint8_t strobe_on, uint8_t strobe_off, uint8_t point_type)
{
    int16_t       dac_val;
    int8_t        hi_point;
    int8_t        lo_point;
    
    if (point_type == 0)
      {
	printk("bad data point type\n");
	return;
      }
    // Adjust data for producing correct LTC1597 output voltage to DAC
    // This function also avoids fault condition.
    lv2_get_xydata_ltcval((int16_t *)&dac_val, point);

    // Data is applied to DAC input after lo byte is written
    // so sequence is important.  hi byte then lo byte.
    hi_point = (int8_t)(dac_val >> 8) & 0xFF;
    lo_point = (int8_t)(dac_val & 0xFF);

    // write to X or Y
    // FIXME---PAH---May want to add both for time savings
    if (point_type & LV2_XPOINT)
      {
	outb(hi_point, LG_IO_XH);
	outb(lo_point, LG_IO_XL);
      }
    else
      {
	outb(hi_point, LG_IO_YH);
	outb(lo_point, LG_IO_YL);
      }
    // Let data WRITE to DAC input register operation take place
    outb(strobe_on, LG_IO_CNTRL1);
    // Strobe bit 0->1 latches data,
    // Strobe bit 1->0 writes data to DAC
    outb(strobe_off, LG_IO_CNTRL1);
    return;
}
static inline void lv2_send_xy_to_dac(struct lv2_xypoints *xyData, uint8_t strobe_on, uint8_t strobe_off, uint8_t point_type, uint8_t beam_on_off)
{
    int16_t       dac_val;
    int8_t        hi_xpoint;
    int8_t        lo_xpoint;
    int8_t        hi_ypoint;
    int8_t        lo_ypoint;
    uint8_t       beam_setting;

    beam_setting = inb(LG_IO_CNTRL2);  // Turn off beam
    if (beam_on_off)
      beam_setting |= LASERENABLE | BRIGHTBEAM;  // light move, enable laser.
    else
      beam_setting &= LASERDISABLE;  // Dark move, disable laser.

    outb(beam_setting, LG_IO_CNTRL2);
    
    if (point_type == 0)
      {
	printk("bad data point type\n");
	return;
      }
    // Adjust data for producing correct LTC1597 output voltage to DAC
    // This function also avoids fault condition.
    // Data is applied to DAC input after lo byte is written
    // so sequence is important.  hi byte then lo byte.

    // convert xpoint
    lv2_get_xydata_ltcval((int16_t *)&dac_val, xyData->xPoint);
    hi_xpoint = (int8_t)(dac_val >> 8) & 0xFF;
    lo_xpoint = (int8_t)(dac_val & 0xFF);

    // convert ypoint
    lv2_get_xydata_ltcval((int16_t *)&dac_val, xyData->yPoint);
    hi_ypoint = (int8_t)(dac_val >> 8) & 0xFF;
    lo_ypoint = (int8_t)(dac_val & 0xFF);

    // write X & Y to DAC thru FPGA
    outb(hi_xpoint, LG_IO_XH);
    outb(lo_xpoint, LG_IO_XL);
    outb(hi_ypoint, LG_IO_YH);
    outb(lo_ypoint, LG_IO_YL);

    // Let data WRITE to DAC input register operation take place
    outb(strobe_on, LG_IO_CNTRL1);
    // Strobe bit 0->1 latches data,
    // Strobe bit 1->0 writes data to DAC
    outb(strobe_off, LG_IO_CNTRL1);
    return;
}
static uint8_t lv2_sense_point(int16_t point, uint8_t point_type)
{
    uint8_t       beam_setting;

    beam_setting = inb(LG_IO_CNTRL2);  // Turn off beam
    // Turn on beam, bright intensity
    beam_setting |= LASERENABLE | BRIGHTBEAM;  // light move, enable laser.
    outb(beam_setting, LG_IO_CNTRL2);  // Turn off beam

    // Write x-data to DAC
    lv2_send_to_dac(point, STROBE_ON_LASER_ON, STROBE_OFF_LASER_ON, point_type);
    return(inb(TFPORTRL));
}
void lv2_traverse_x(struct lv2_dev *priv, struct lv2_sense_info *pSenseInfo)
{
    struct write_sense_data    *pSenseData=(struct write_sense_data *)priv->pSenseBuff;
    int16_t                    point;
    uint8_t                    i;
    uint8_t                    beam_setting;

    // About to write to sensor buffer
    spin_lock(&priv->lock);
    printk("Start SenseX: startX=%x, step=%d, count=%d\n", pSenseInfo->data, pSenseInfo->step, pSenseInfo->loop_count);
    for (i = 0; i < pSenseInfo->loop_count; i++)
      {
	point = pSenseInfo->data + (i * pSenseInfo->step);
	pSenseData[i].sense_val = lv2_sense_point(point, LV2_XPOINT);
	pSenseData[i].point = point;
	printk("SenseX point=%x: data[%d]=%x\n",point, i, pSenseData[i].sense_val);
      }
    // Turn beam off
    beam_setting = inb(LG_IO_CNTRL2);  // Turn off beam
    beam_setting &= LASERDISABLE;
    outb(beam_setting, LG_IO_CNTRL2);  // Turn off beam
    mdelay(10);
    return;
}

void lv2_traverse_y(struct lv2_dev *priv, struct lv2_sense_info *pSenseInfo)
{
    struct write_sense_data    *pSenseData=(struct write_sense_data *)priv->pSenseBuff;
    int16_t    point;
    uint8_t    i;
    uint8_t    beam_setting;
    
    // About to write to sensor buffer, so lock now.
    spin_lock(&priv->lock);
    printk("Start SenseY: startY=%x, step=%d, count=%d\n", pSenseInfo->data, pSenseInfo->step, pSenseInfo->loop_count);
    for (i = 0; i < pSenseInfo->loop_count; i++)
      {
	point = pSenseInfo->data + (i * pSenseInfo->step);
	pSenseData[i].sense_val = lv2_sense_point(point, LV2_YPOINT);
	pSenseData[i].point = point;
	printk("Sense ypoint[%d]=%x: data=%x\n",i, point, pSenseData[i].sense_val);
      }
    // Turn beam off
    beam_setting = inb(LG_IO_CNTRL2);  // Turn off beam
    beam_setting &= LASERDISABLE;
    outb(beam_setting, LG_IO_CNTRL2);  // Turn off beam
    mdelay(30);
    return;
}
void lv2_sense_one_ypoint(struct lv2_dev *priv, struct lv2_sense_one_info *pSenseInfo)
{
    struct write_sense_data    *pSenseData=(struct write_sense_data *)priv->pSenseBuff;
    int16_t    point;
    
    // About to write to sensor buffer, so lock now.
    spin_lock(&priv->lock);
    point = pSenseInfo->data;
    pSenseData[pSenseInfo->index].sense_val = lv2_sense_point(point, LV2_YPOINT);
    pSenseData[pSenseInfo->index].point = point;
    printk("Sense yPoint[%d]=%x: data=%x\n",pSenseInfo->index, point, pSenseData[pSenseInfo->index].sense_val);
    return;
}
void lv2_sense_one_xpoint(struct lv2_dev *priv, struct lv2_sense_one_info *pSenseInfo)
{
    struct write_sense_data    *pSenseData=(struct write_sense_data *)priv->pSenseBuff;
    int16_t    point;
    
    // About to write to sensor buffer, so lock now.
    spin_lock(&priv->lock);
    point = pSenseInfo->data;
    pSenseData[pSenseInfo->index].sense_val = lv2_sense_point(point, LV2_YPOINT);
    pSenseData[pSenseInfo->index].point = point;
    printk("Sense xPoint[%d]=%x: data=%x\n",pSenseInfo->index, point, pSenseData[pSenseInfo->index].sense_val);
    return;
}
static void lv2_move_xdata(struct lv2_xypoints *xyData, uint8_t strobe_on, uint8_t strobe_off, uint8_t beam_on_off)
{
    uint8_t       beam_setting;

    beam_setting = inb(LG_IO_CNTRL2);  // Turn off beam
    if (beam_on_off)
      beam_setting |= LASERENABLE | BRIGHTBEAM;  // light move, enable laser.
    else
      beam_setting &= LASERDISABLE;            // dark move, disable laser
    outb(beam_setting, LG_IO_CNTRL2);  // Apply CNTRL2 setting
    lv2_send_to_dac(xyData->xPoint, strobe_on, strobe_off, LV2_XPOINT);
    return;
}
static void lv2_move_ydata(struct lv2_xypoints *xyData, uint8_t strobe_on, uint8_t strobe_off, uint8_t beam_on_off)
{
    uint8_t       beam_setting;

    beam_setting = inb(LG_IO_CNTRL2);  // Turn off beam
    if (beam_on_off)
      beam_setting |= LASERENABLE | BRIGHTBEAM;  // light move, enable laser.
    else
      beam_setting &= LASERDISABLE;    // dark move, disable laser.
    outb(beam_setting, LG_IO_CNTRL2);  // Apply CNTRL2 settings
    lv2_send_to_dac(xyData->yPoint, strobe_on, strobe_off, LV2_YPOINT);
    return;
}
static void lv2_move_xydata(struct lv2_xypoints *xyData, uint8_t strobe_on, uint8_t strobe_off, uint8_t beam_on_off)
{
    lv2_move_xdata(xyData, strobe_on, strobe_off, beam_on_off);
    lv2_move_ydata(xyData, strobe_on, strobe_off, beam_on_off);
    return;
}
static void lv2_move_xydata_dark(struct lv2_xypoints *xyData)
{
    lv2_move_xydata(xyData, STROBE_ON_LASER_OFF, STROBE_OFF_LASER_OFF, 0);
    return;
}
static void lv2_move_xydata_lite(struct lv2_xypoints *xyData)
{
  lv2_move_xydata(xyData, STROBE_ON_LASER_ON, STROBE_OFF_LASER_ON, 1);
    return;
}      
static int lv2_proc_cmd(struct cmd_rw *p_cmd_data, struct lv2_dev *priv)
{
    if (!priv)
      return(-ENODEV);

    switch(p_cmd_data->base.hdr.cmd)
      {
      case CMDW_LV2_TRAVERSEX:
	lv2_traverse_x(priv, (struct lv2_sense_info *)&p_cmd_data->base.cmd_data.senseData);
	break;
      case CMDW_LV2_TRAVERSEY:
	lv2_traverse_y(priv, (struct lv2_sense_info *)&p_cmd_data->base.cmd_data.senseData);
	break;
      case CMDW_LV2_SENSE_XPOINT:
	lv2_sense_one_xpoint(priv, (struct lv2_sense_one_info *)&p_cmd_data->base.cmd_data.senseData);
	break;
      case CMDW_LV2_SENSE_YPOINT:
	lv2_sense_one_ypoint(priv, (struct lv2_sense_one_info *)&p_cmd_data->base.cmd_data.senseData);
	break;
      case CMDW_LV2_MVXYDARK:
	lv2_move_xydata_dark((struct lv2_xypoints *)&p_cmd_data->base.cmd_data.xyPoints);
	break;
      case CMDW_LV2_MVXYLITE:
	lv2_move_xydata_lite((struct lv2_xypoints *)&p_cmd_data->base.cmd_data.xyPoints);
	break;
      default:
	printk(KERN_ERR "\nAV-LV2:  CMDW %d option not found", p_cmd_data->base.hdr.cmd);
	break;
      }
    return(0);
};
/******************************************************************
*                                                                 *
* lv2_read()                                                      *
* Description:   This function is used to process read commands.  *
*                The read buffer contains results for target      *
*                find phases.                                     *
*                                                                 *
******************************************************************/
ssize_t lv2_read(struct file *file, char __user *buffer, size_t count, loff_t *f_pos)
{
    struct lv2_dev *priv = file->private_data;

    // The buffer is filled on a write command to traverse x or y
    // up, down, left, or right.  Sense data is collected for each
    // traverse command executed, so need to lock during traverse
    // Only unlock on error here or once buffer is read.
    if (!priv)
      {
	spin_unlock(&priv->lock);
	return(-EBADF);
      }    
    if ((count<=0) || (count > priv->sense_buff_size))
      {
	spin_unlock(&priv->lock);
	return(-EINVAL);
      }
    /* sensor scan has been done */
    if (copy_to_user(buffer, priv->pSenseBuff, count))
      {
	spin_unlock(&priv->lock);
	return(-EFAULT);
      }
    // All done with sensing, prep for next one.
    memset(priv->pSenseBuff, 0, priv->sense_buff_size);
    spin_unlock(&priv->lock);
    return(count);
}

ssize_t lv2_write(struct file *file, const char __user *buffer, size_t count, loff_t *f_pos)
{
    struct lv2_dev           *priv;
    struct cmd_rw            *cmd_data;
    struct cmd_hdr           *pHdr;
    int                       rc;

    priv = (struct lv2_dev *)file->private_data;
    if (!priv)
      return(-EBADF);
  
    // Get command data
    cmd_data = kzalloc(sizeof(struct cmd_rw), GFP_KERNEL);
    if (!cmd_data)
      {
	pr_err("kzalloc() failed for laser\n");
	return(-ENOMEM);
      }

    if ((count <= 0)|| (count > sizeof(struct cmd_rw)))
      {
	kfree(cmd_data);
	return -EINVAL;
      }
    if(copy_from_user((char *)cmd_data, buffer, count))
      {
	kfree(cmd_data);
	return -EFAULT;
      }
    pHdr = (struct cmd_hdr *)cmd_data;
    // Validate command type
    if (!pHdr->cmd || (pHdr->cmd > CMD_LAST))
      {
	printk(KERN_ERR "\nAV-LV2: lg_write unknown command %d", pHdr->cmd);
	kfree(cmd_data);
	return(-EINVAL);
      }
    rc = lv2_proc_cmd(cmd_data, priv);
    kfree(cmd_data);
    return(count);
} 

static int lv2_dev_init(struct lv2_dev *lv2_devp)
{
    struct   lv2_xypoints   xyData;

    if (!lv2_devp)
      return(-EINVAL);
  
    do_gettimeofday(&lv2_devp->last_ts);

    /* move to 0,0 position, laser disabled, beam off */
    memset((uint8_t *)&xyData, 0, sizeof(struct lv2_xypoints));
    lv2_move_xydata_dark((struct lv2_xypoints *)&xyData);

    // Initialize buffers to 0
    lv2_devp->sense_buff_size = MAX_TGFIND_BUFFER;
    lv2_devp->pSenseBuff = kzalloc(lv2_devp->sense_buff_size, GFP_KERNEL);
    if (lv2_devp->pSenseBuff == NULL)
      {
	printk("AV-LV2:  Unable to malloc sensor buffer\n");
	return(-ENOMEM);
      }
      
    // Start with READY-LED ON TO INDICATE NOT READY TO RUN.
    outb(RDYLEDON, LG_IO_CNTRL2);
    printk("AV-LV2:  lv2_dev_init() succeeded\n");
    return(0);
}
static int lv2_open(struct inode *inode, struct file *file)
{
  return 0;
}
int lv2_release(struct inode *_inode, struct file *f)
{
  return 0;
}

static const struct file_operations lv2_fops = {
  .owner	    = THIS_MODULE,
  .llseek           = no_llseek,
  .read             =  lv2_read,        /* lg_read */
  .write            = lv2_write,       /* lg_write */
  .open             = lv2_open,        /* lg_open */
  .release          = lv2_release,     /* lg_release */
};
struct miscdevice lv2_device = {
  .minor = MISC_DYNAMIC_MINOR,
  .name = DEV_NAME_LV2,
  .fops = &lv2_fops,
};

static int lv2_dev_probe(struct platform_device *plat_dev)
{
    struct lv2_dev *lv2_dev;
    int            rc;

    // allocate mem for struct device will work with
    lv2_dev = kzalloc(sizeof(struct lv2_dev), GFP_KERNEL);
    if (!lv2_dev)
      {
	pr_err("kzalloc() failed for laser device struct\n");
	return(-ENOMEM);
      }

    memset((char *)lv2_dev,0,sizeof(struct lv2_dev));
    platform_set_drvdata(plat_dev, lv2_dev);
    lv2_dev->dev = &plat_dev->dev;

    // We don't use kref or mutex locks yet.
    kref_init(&lv2_dev->ref);
    mutex_init(&lv2_dev->lg_mutex);

    dev_set_drvdata(lv2_dev->dev, lv2_dev);
    spin_lock_init(&lv2_dev->lock);
    INIT_LIST_HEAD(&lv2_dev->free);
    INIT_LIST_HEAD(&lv2_dev->used);
    init_waitqueue_head(&lv2_dev->wait);

    // Setup misc device
    lv2_dev->miscdev.minor = lv2_device.minor;
    lv2_dev->miscdev.name = DEV_NAME_LV2;
    lv2_dev->miscdev.fops = lv2_device.fops;
    rc = misc_register(&lv2_dev->miscdev);
    if (rc)
      {
	printk(KERN_ERR "AV-LV2:  Failed to register Laser misc_device, err %d \n", rc);
	kfree(lv2_dev);
	return(rc);
      }

    printk(KERN_INFO "\nAV-LV2:laser misc-device created\n");

    // Obtain IO space for device
    if (!request_region(LG_BASE, LASER_REGION, DEV_NAME_LV2))
      {
	kfree(lv2_dev);
	misc_deregister(&lv2_dev->miscdev);
	printk(KERN_CRIT "\nAV-LV2:  Unable to get IO regs");
	return(-EBUSY);
      }

    rc = lv2_dev_init(lv2_dev);
    if (rc)
      {
	printk(KERN_ERR "AV-LV2:  Failed to initialize Laser device, err %d \n", rc);
	kfree(lv2_dev);
	return(rc);
      }
      
    // All initialization done, so enable timer
    printk(KERN_INFO "\nAV-LV2: LV2 Device installed and initialized.\n");
    return(0);
}
static int lv2_pdev_remove(struct platform_device *pdev)
{
  struct lv2_dev *lv2_dev = platform_get_drvdata(pdev);
  struct device *this_device;
  
  if (!lv2_dev)
    return(-EBADF);

  this_device = lv2_dev->miscdev.this_device;
  release_region(LG_BASE, LASER_REGION);
  misc_deregister(&lv2_dev->miscdev);
  kfree(lv2_dev);
  return(0);
}

static struct platform_device lv2_dev = {
  .name   = DEV_NAME_LV2,
  .id     = 0,
  .num_resources = 0,
};
static struct platform_driver lv2_platform_driver = {
  .probe   = lv2_dev_probe,
  .remove  = lv2_pdev_remove,
  .driver  = {
    .name  = DEV_NAME_LV2,
  },
};
static int __init lv2_init(void)
{
  int rc;

  rc = platform_driver_register(&lv2_platform_driver);
  if (rc)
    {
      printk(KERN_ERR "AV-LV2:  Unable to register platform driver lv2, ret %d\n", rc);
      return(rc);
    }
  rc = platform_device_register(&lv2_dev);
  if (rc)
    {
      printk(KERN_ERR "AV-LV2:  Unable to register platform device lv2, ret %d\n", rc);
      return(rc);
    }
  printk(KERN_INFO "\nAV-LV2:LaserVision2 platform device driver installed.\n");
  return(rc);
}

static void __exit lv2_exit(void)
{
  platform_device_unregister(&lv2_dev);
  platform_driver_unregister(&lv2_platform_driver);
  return;
}
module_init(lv2_init);
module_exit(lv2_exit);

MODULE_AUTHOR("Patricia A. Holden for Aligned Vision");
MODULE_DESCRIPTION("Driver for Laser Vision 2");
MODULE_LICENSE("GPL");