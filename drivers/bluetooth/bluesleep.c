/*

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 2 as
   published by the Free Software Foundation.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.  


   Copyright (C) 2006-2007 - Motorola
   Copyright (c) 2008-2009, Code Aurora Forum. All rights reserved.

   Date         Author           Comment
   -----------  --------------   --------------------------------
   2006-Apr-28	Motorola	 The kernel module for running the Bluetooth(R)
				 Sleep-Mode Protocol from the Host side
   2006-Sep-08  Motorola         Added workqueue for handling sleep work.
   2007-Jan-24  Motorola         Added mbm_handle_ioi() call to ISR.

*/

#include <linux/module.h>	/* kernel module definitions */
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>

#include <linux/wakelock.h>
#include <linux/irq.h>
#include <linux/param.h>
#include <linux/bitops.h>
#include <linux/termios.h>
#include <mach/gpio.h>
#include <mach/msm_serial_hs.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h> /* event notifications */
#include "hci_uart.h"

#define BT_SLEEP_DBG
#ifndef BT_SLEEP_DBG
#define BT_DBG(fmt, arg...)
#endif
#undef	BT_DBG
#define BT_DBG(fmt, arg...)  printk(KERN_INFO "%s: " fmt "\n" , __func__ , ## arg)//huanggd tmp
/*
 * Defines
 */
static void hsuart_power_work(struct work_struct *work);


DECLARE_DELAYED_WORK(sleep_workqueue, hsuart_power_work);
#define hsuart_power_delay()     schedule_delayed_work(&sleep_workqueue, HZ/2)




#define VERSION		"1.1"
#define PROC_DIR	"bluetooth/sleep"

struct bluesleep_info {
	unsigned host_wake;
	unsigned ext_wake;
	unsigned host_wake_irq;
	struct uart_port *uport;
	struct wake_lock wake_lock;
};


#define TX_TIMER_INTERVAL		3
#define DELAY_TURNON_UARTCLK	1

/* state variable names and bit positions */
#define BT_PROTO	0x01
#define BT_TXDATA	0x02
#define UART_ASLEEP	0x04

enum
{
    BE_SLEEPING = 0,
    WAKE_FIRST,
    WAKE_SECOND,
};/*Wolfman 20120222*/

/* global pointer to a single hci device. */
static struct hci_dev *bluesleep_hdev;

static struct bluesleep_info *bsi;

/* module usage */
static atomic_t open_count = ATOMIC_INIT(1);

/*
 * Local function prototypes
 */

static int bluesleep_hci_event(struct notifier_block *this,
			    unsigned long event, void *data);

/*
 * Global variables
 */
  
/** Global state flags */
static unsigned long flags;
/** Transmission timer */
static struct timer_list tx_timer;

/** Lock for state transitions */
static spinlock_t rw_lock;

static volatile  int  g_is_reg=0;
static volatile int wakeup_first = BE_SLEEPING;/*Wolfman 20120222*/


/** Notifier block for HCI events */
struct notifier_block hci_event_nblock = {
	.notifier_call = bluesleep_hci_event,
};

struct proc_dir_entry *bluetooth_dir, *sleep_dir;

/*20120213*/
static struct tasklet_struct hostwake_task;

/*
 * Local functions
 */

static void ChangeWakeupFirstStatus(int new_st)
{
	BT_DBG("WakeupFirst old status: %d, new status: %d", wakeup_first, new_st);
	wakeup_first = new_st;
}

static int GetWakeupFirstStatus(void)
{
	return wakeup_first;
}

static void hsuart_power(int on)
{
	if (on) {
		if (bsi->uport)
		{
			printk("20101117_hsuart on.\n");
		 	msm_hs_request_clock_on(bsi->uport);
			msm_hs_set_mctrl(bsi->uport, TIOCM_RTS);
		}

	} else {	
		if (bsi->uport)
		{
			printk("20101117_hsuart off.\n");
			msm_hs_set_mctrl(bsi->uport, 0);
			msm_hs_request_clock_off(bsi->uport);
		}
	}
}


static void hsuart_power_work(struct work_struct *work)
{
	if (test_bit(UART_ASLEEP, &flags))
	{
		clear_bit(UART_ASLEEP, &flags);
		hsuart_power(1);
	}
	else
	{
		BT_DBG("Uart already on!!!");
	}
}

/**
 * Handles proper timer action when outgoing data is delivered to the
 * HCI line discipline. Sets BT_TXDATA.
 */
static void bluesleep_outgoing_data(void)
{
	unsigned long irq_flags;

	spin_lock_irqsave(&rw_lock, irq_flags);
	/* if the tx side is sleeping... */
	if (gpio_get_value(bsi->ext_wake)) {

		BT_DBG("tx was sleeping");

		gpio_set_value(bsi->ext_wake, 0);
	}
    if (test_bit(UART_ASLEEP, &flags))
	{
		clear_bit(UART_ASLEEP, &flags);
		hsuart_power(1);
		BT_DBG("Uart turn on in %s",__func__);
	}

	if(GetWakeupFirstStatus() == BE_SLEEPING)/*If host sending data, no need waite any more, Wolfman 20120223*/
	{
		ChangeWakeupFirstStatus(WAKE_SECOND);
	}

	spin_unlock_irqrestore(&rw_lock, irq_flags);
	//BT_DBG("%s is called", __func__);
}

/**
 * Handles HCI device events.
 * @param this Not used.
 * @param event The event that occurred.
 * @param data The HCI device associated with the event.
 * @return <code>NOTIFY_DONE</code>.
 */
static int bluesleep_hci_event(struct notifier_block *this,
				unsigned long event, void *data)
{
	struct hci_dev *hdev = (struct hci_dev *) data;
	struct hci_uart *hu;
	struct uart_state *state;
  //	printk("*bosj* bluesleep_hci_event() is running .\n");

	if (!hdev)
		return NOTIFY_DONE;

	switch (event) {
	case HCI_DEV_REG:
		if (!bluesleep_hdev) {
			bluesleep_hdev = hdev;
			hu  = (struct hci_uart *) hdev->driver_data;
			state = (struct uart_state *) hu->tty->driver_data;
			bsi->uport = state->uart_port;
			g_is_reg=1;   //bt is really ok!
		}
		break;
	case HCI_DEV_UNREG:
		bluesleep_hdev = NULL;
		bsi->uport = NULL;
		g_is_reg=0;
		break;
	case HCI_DEV_WRITE:
		bluesleep_outgoing_data();
		break;
	}

	return NOTIFY_DONE;
}

void  bluesleep_start_sleep_timer(void)
{
	mod_timer(&tx_timer, jiffies + (TX_TIMER_INTERVAL * HZ));
	if(!wake_lock_active((&bsi->wake_lock)))
	{
		BT_DBG("wake_lock is called");
		wake_lock(&bsi->wake_lock);
	}
}
EXPORT_SYMBOL(bluesleep_start_sleep_timer);

/*20120213*/
static void bluesleep_hostwake_task(unsigned long data)
{
	BT_DBG("%s is called", __func__);
	if(!wake_lock_active((&bsi->wake_lock)))
	{
		wake_lock(&bsi->wake_lock);
		BT_DBG("wake_lock is called in %s",__func__);
	}
	/*gpio_set_value(bsi->ext_wake, 0);*///20120213 				
	if(GetWakeupFirstStatus() == WAKE_FIRST)/*Wolfman 20120222*/
	{
	    ChangeWakeupFirstStatus(WAKE_SECOND);
		BT_DBG("Uart will delay on...");
	    //msleep(300);
	 //   set_current_state(TASK_INTERRUPTIBLE);
		//schedule_timeout((DELAY_TURNON_UARTCLK * HZ));
	//	schedule_timeout((DELAY_TURNON_UARTCLK * HZ));
		hsuart_power_delay();
		return;
	} 
	if (test_bit(UART_ASLEEP, &flags))
	{
		clear_bit(UART_ASLEEP, &flags);
		hsuart_power(1);//20120213	
		BT_DBG("Uart turn on in %s",__func__);
	}
}

/*
 * Handles transmission timer expiration.
 * @param data Not used.
 */
static void bluesleep_tx_timer_expire(unsigned long data)
{
	unsigned long irq_flags;

	spin_lock_irqsave(&rw_lock, irq_flags);

	BT_DBG("Tx timer expired");

	gpio_set_value(bsi->ext_wake, 1);

	/* Bob 0531.begin */
	 if (!test_bit(UART_ASLEEP, &flags))
     	{
       		set_bit(UART_ASLEEP, &flags);//<<-----------------------------
          if (bsi->uport&&msm_hs_tx_empty(bsi->uport))
          	{
          		//set_bit(UART_ASLEEP, &flags);
          		BT_DBG("going to sleep...");
          		hsuart_power(0);
          	}
	   }
      /* Bob */   			
	spin_unlock_irqrestore(&rw_lock, irq_flags);

	if(wake_lock_active((&bsi->wake_lock)))/*20120213*/
	{
		BT_DBG("wake_unlock is called");
		ChangeWakeupFirstStatus(BE_SLEEPING);/*Wolfman 20120222*/
		wake_unlock(&bsi->wake_lock);
	}
}

/**
 * Schedules a tasklet to run when receiving an interrupt on the
 * <code>HOST_WAKE</code> GPIO pin.
 * @param irq Not used.
 * @param dev_id Not used.
 */
static irqreturn_t bluesleep_hostwake_isr(int irq, void *dev_id)
{

	unsigned long irq_flags;
	int host_wake;

	spin_lock_irqsave(&rw_lock, irq_flags);

	host_wake = gpio_get_value(bsi->host_wake);
//	printk("******before host_wake is %d\n",host_wake);
	irq_set_irq_type(irq, host_wake ? IRQF_TRIGGER_LOW:IRQF_TRIGGER_HIGH );//�ж����ͽ�����
	   printk("******host_wake is %d\n",host_wake);
	//printk("****** (host_wake ? IRQF_TRIGGER_LOW:IRQF_TRIGGER_HIGH)  is %d",(host_wake ? IRQF_TRIGGER_LOW:IRQF_TRIGGER_HIGH));
  
      if(g_is_reg)
      {
	  	/*  changed by Bob */
		#if 0
          	if (host_wake)
              {
          		BT_DBG("[I]sleeping up...\n");
          
                  if (!test_bit(UART_ASLEEP, &flags))
                  {
          			set_bit(UART_ASLEEP, &flags);//<<-----------------------------
          			if (bsi->uport&&msm_hs_tx_empty(bsi->uport))
          			{
          				//set_bit(UART_ASLEEP, &flags);
          				BT_DBG("going to sleep...");
          				hsuart_power(0);
          			}
          		}
          
              }
          	else
		#endif
			if (host_wake == 0)
              {
          		BT_DBG("[I]waking up...\n");
#if 0          
          		if (test_bit(UART_ASLEEP, &flags))
          		{
				clear_bit(UART_ASLEEP, &flags);
				if(GetWakeupFirstStatus() == BE_SLEEPING)/*Wolfman 20120222*/
				    ChangeWakeupFirstStatus(WAKE_FIRST);
				tasklet_schedule(&hostwake_task);
          		}
			else
			{
				BT_DBG("ASLEEP = 0");
			}
#endif
			if(GetWakeupFirstStatus() == BE_SLEEPING)/*Wolfman 20120222*/	
			{
				ChangeWakeupFirstStatus(WAKE_FIRST);
			}
			tasklet_schedule(&hostwake_task);
			/*
			else
			{ 
				if (test_bit(UART_ASLEEP, &flags))
				{
					clear_bit(UART_ASLEEP, &flags);
					hsuart_power(1);
				}
			}
			*/
          	}
      	}
	spin_unlock_irqrestore(&rw_lock, irq_flags);

	return IRQ_HANDLED;
}

/**
 * Starts the Sleep-Mode Protocol on the Host.
 * @return On success, 0. On error, -1, and <code>errno</code> is set
 * appropriately.
 */
static int bluesleep_start(void)
{
	int retval;
	unsigned long irq_flags;
//	printk("*bosj* bluesleep_start() is running .\n");

	spin_lock_irqsave(&rw_lock, irq_flags);

	if (test_bit(BT_PROTO, &flags)) {
		spin_unlock_irqrestore(&rw_lock, irq_flags);
		return 0;
	}

	spin_unlock_irqrestore(&rw_lock, irq_flags);

	if (!atomic_dec_and_test(&open_count)) {
		atomic_inc(&open_count);
		return -EBUSY;
	}

	/* start the timer */

	mod_timer(&tx_timer, jiffies + (TX_TIMER_INTERVAL*HZ));

	/* assert BT_WAKE */
	gpio_set_value(bsi->ext_wake, 0);
	retval = request_irq(bsi->host_wake_irq, bluesleep_hostwake_isr,
				IRQF_TRIGGER_HIGH,
				"bluetooth hostwake", NULL);
	if (retval  < 0) {
		BT_ERR("Couldn't acquire BT_HOST_WAKE IRQ");
		goto fail;
	}

	retval = enable_irq_wake(bsi->host_wake_irq);
	
	if (retval < 0) {
		BT_ERR("Couldn't enable BT_HOST_WAKE as wakeup interrupt");
		free_irq(bsi->host_wake_irq, NULL);
		goto fail;
	}

	set_bit(BT_PROTO, &flags);
	return 0;
fail:
	del_timer(&tx_timer);
	atomic_inc(&open_count);

	return retval;
}

/**
 * Stops the Sleep-Mode Protocol on the Host.
 */
static void bluesleep_stop(void)
{
	unsigned long irq_flags;

	spin_lock_irqsave(&rw_lock, irq_flags);

	if (!test_bit(BT_PROTO, &flags)) {
		spin_unlock_irqrestore(&rw_lock, irq_flags);
		return;
	}

	/* assert BT_WAKE */
	gpio_set_value(bsi->ext_wake, 0);
	del_timer(&tx_timer);/*20120216*/
	clear_bit(BT_PROTO, &flags);

	if (test_bit(UART_ASLEEP, &flags)) {
		clear_bit(UART_ASLEEP, &flags);
		hsuart_power(1);
	}

	atomic_inc(&open_count);

	spin_unlock_irqrestore(&rw_lock, irq_flags);
	if (disable_irq_wake(bsi->host_wake_irq))
		BT_ERR("Couldn't disable hostwake IRQ wakeup mode\n");
	free_irq(bsi->host_wake_irq, NULL);

	if(wake_lock_active(&bsi->wake_lock))
	{
		wake_unlock(&bsi->wake_lock);
	}
	//while(wake_lock_active(&bsi->wake_lock))
	//	;
	//del_timer(&tx_timer);
	ChangeWakeupFirstStatus(BE_SLEEPING);
	g_is_reg=0;
	//wake_lock_destroy(&bsi->wake_lock);
}
/**
 * Read the <code>BT_WAKE</code> GPIO pin value via the proc interface.
 * When this function returns, <code>page</code> will contain a 1 if the
 * pin is high, 0 otherwise.
 * @param page Buffer for writing data.
 * @param start Not used.
 * @param offset Not used.
 * @param count Not used.
 * @param eof Whether or not there is more data to be read.
 * @param data Not used.
 * @return The number of bytes written.
 */
static int bluepower_read_proc_btwake(char *page, char **start, off_t offset,
					int count, int *eof, void *data)
{
	*eof = 1;
//	printk("*bosj* bluesleep_start() is running,and ext_wake is %d .\n",bsi->ext_wake);
	return sprintf(page, "btwake:%u\n", gpio_get_value(bsi->ext_wake));
}

/**
 * Write the <code>BT_WAKE</code> GPIO pin value via the proc interface.
 * @param file Not used.
 * @param buffer The buffer to read from.
 * @param count The number of bytes to be written.
 * @param data Not used.
 * @return On success, the number of bytes written. On error, -1, and
 * <code>errno</code> is set appropriately.
 */
static int bluepower_write_proc_btwake(struct file *file, const char *buffer,
					unsigned long count, void *data)
{
	char *buf;

	if (count < 1)
		return -EINVAL;

	buf = kmalloc(count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	//printk("*bosj* bluepower_write_proc_btwake() is running,and ext_wake is %d .\n",bsi->ext_wake);

	if (copy_from_user(buf, buffer, count)) {
		kfree(buf);
		return -EFAULT;
	}

	if (buf[0] == '0') {
		gpio_set_value(bsi->ext_wake, 0);
	} else if (buf[0] == '1') {
		gpio_set_value(bsi->ext_wake, 1);
	} else {
		kfree(buf);
		return -EINVAL;
	}

	kfree(buf);
	return count;
}

/**
 * Read the <code>BT_HOST_WAKE</code> GPIO pin value via the proc interface.
 * When this function returns, <code>page</code> will contain a 1 if the pin
 * is high, 0 otherwise.
 * @param page Buffer for writing data.
 * @param start Not used.
 * @param offset Not used.
 * @param count Not used.
 * @param eof Whether or not there is more data to be read.
 * @param data Not used.
 * @return The number of bytes written.
 */
static int bluepower_read_proc_hostwake(char *page, char **start, off_t offset,
					int count, int *eof, void *data)
{
	*eof = 1;
//	printk("*bosj* bluepower_read_proc_hostwake() is running,and ext_wake is %d .\n",bsi->ext_wake);

	return sprintf(page, "hostwake: %u \n", gpio_get_value(bsi->host_wake));
}


/**
 * Read the low-power status of the Host via the proc interface.
 * When this function returns, <code>page</code> contains a 1 if the Host
 * is asleep, 0 otherwise.
 * @param page Buffer for writing data.
 * @param start Not used.
 * @param offset Not used.
 * @param count Not used.
 * @param eof Whether or not there is more data to be read.
 * @param data Not used.
 * @return The number of bytes written.
 */
static int bluesleep_read_proc_asleep(char *page, char **start, off_t offset,
					int count, int *eof, void *data)
{
	unsigned int asleep;

	asleep = test_bit(UART_ASLEEP, &flags) ? 1 : 0;
	*eof = 1;
	
	
	return sprintf(page, "asleep: %u\n", asleep);
}

/**
 * Read the low-power protocol being used by the Host via the proc interface.
 * When this function returns, <code>page</code> will contain a 1 if the Host
 * is using the Sleep Mode Protocol, 0 otherwise.
 * @param page Buffer for writing data.
 * @param start Not used.
 * @param offset Not used.
 * @param count Not used.
 * @param eof Whether or not there is more data to be read.
 * @param data Not used.
 * @return The number of bytes written.
 */
static int bluesleep_read_proc_proto(char *page, char **start, off_t offset,
					int count, int *eof, void *data)
{
	unsigned int proto;

	proto = test_bit(BT_PROTO, &flags) ? 1 : 0;
	*eof = 1;
	return sprintf(page, "proto: %u\n", proto);
}

/**
 * Modify the low-power protocol used by the Host via the proc interface.
 * @param file Not used.
 * @param buffer The buffer to read from.
 * @param count The number of bytes to be written.
 * @param data Not used.
 * @return On success, the number of bytes written. On error, -1, and
 * <code>errno</code> is set appropriately.
 */
static int bluesleep_write_proc_proto(struct file *file, const char *buffer,
					unsigned long count, void *data)
{
	char proto;

	if (count < 1)
		return -EINVAL;

	if (copy_from_user(&proto, buffer, 1))
		return -EFAULT;

	if (proto == '0')
		bluesleep_stop();
	else
		
		bluesleep_start();

	/* claim that we wrote everything */
	return count;
}

static int __devinit bluesleep_probe(struct platform_device *pdev)
{
	int ret;
	struct resource *res;

	bsi = kzalloc(sizeof(struct bluesleep_info), GFP_KERNEL);
	//printk(" ***bosj** run");
	if (!bsi)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_IO,
				"gpio_host_wake");
	if (!res) {
		BT_ERR("couldn't find host_wake gpio\n");
		ret = -ENODEV;
		goto free_bsi;
	} 
	bsi->host_wake = res->start;

	ret = gpio_request(bsi->host_wake, "bt_host_wake");
	if (ret)
		goto free_bsi;
	ret = gpio_direction_input(bsi->host_wake);
	if (ret)
		goto free_bt_host_wake;

	res = platform_get_resource_byname(pdev, IORESOURCE_IO,
				"gpio_ext_wake");
	if (!res) {
		BT_ERR("couldn't find ext_wake gpio\n");
		ret = -ENODEV;
		goto free_bt_host_wake;
	}
	bsi->ext_wake = res->start;

	ret = gpio_request(bsi->ext_wake, "bt_ext_wake");
	if (ret)
		goto free_bt_host_wake;
	/* assert bt wake */
	ret = gpio_direction_output(bsi->ext_wake, 0);
	if (ret)
		goto free_bt_ext_wake;

	bsi->host_wake_irq = platform_get_irq_byname(pdev, "host_wake");
	if (bsi->host_wake_irq < 0) {
		BT_ERR("couldn't find host_wake irq\n");
		ret = -ENODEV;
		goto free_bt_ext_wake;
	}
       wake_lock_init(&bsi->wake_lock, WAKE_LOCK_SUSPEND, "bluesleep");

	return 0;

free_bt_ext_wake:
	gpio_free(bsi->ext_wake);
free_bt_host_wake:
	gpio_free(bsi->host_wake);
free_bsi:
	kfree(bsi);
	return ret;
}

static int bluesleep_remove(struct platform_device *pdev)
{
	/* assert bt wake */
	gpio_set_value(bsi->ext_wake, 0);
	if (test_bit(BT_PROTO, &flags)) {
		if (disable_irq_wake(bsi->host_wake_irq))
			BT_ERR("Couldn't disable hostwake IRQ wakeup mode \n");
		free_irq(bsi->host_wake_irq, NULL);
		del_timer(&tx_timer);
		if (test_bit(UART_ASLEEP, &flags))
			hsuart_power(1);
	}

	gpio_free(bsi->host_wake);
	gpio_free(bsi->ext_wake);
	kfree(bsi);
	return 0;
}

static struct platform_driver bluesleep_driver = {
	.probe = bluesleep_probe,
	.remove = bluesleep_remove,
	.driver = {
		.name = "bluesleep",
		.owner = THIS_MODULE,
	},
};
/**
 * Initializes the module.
 * @return On success, 0. On error, -1, and <code>errno</code> is set
 * appropriately.
 */
static int __init bluesleep_init(void)
{
	int retval;
	struct proc_dir_entry *ent;

	BT_INFO("MSM Sleep Mode Driver Ver %s", VERSION);

	retval = platform_driver_register(&bluesleep_driver);
	if (retval)
		return retval;

	bluesleep_hdev = NULL;

	bluetooth_dir = proc_mkdir("bluetooth", NULL);
	if (bluetooth_dir == NULL) {
		BT_ERR("Unable to create /proc/bluetooth directory");
		return -ENOMEM;
	}

	sleep_dir = proc_mkdir("sleep", bluetooth_dir);
	if (sleep_dir == NULL) {
		BT_ERR("Unable to create /proc/%s directory", PROC_DIR);
		return -ENOMEM;
	}

	/* Creating read/write "btwake" entry */
	ent = create_proc_entry("btwake", 0, sleep_dir);
	if (ent == NULL) {
		BT_ERR("Unable to create /proc/%s/btwake entry", PROC_DIR);
		retval = -ENOMEM;
		goto fail;
	}
	ent->read_proc = bluepower_read_proc_btwake;
	ent->write_proc = bluepower_write_proc_btwake;

	/* read only proc entries */
	if (create_proc_read_entry("hostwake", 0, sleep_dir,
				bluepower_read_proc_hostwake, NULL) == NULL) {
		BT_ERR("Unable to create /proc/%s/hostwake entry", PROC_DIR);
		retval = -ENOMEM;
		goto fail;
	}

	/* read/write proc entries */
	ent = create_proc_entry("proto", 0, sleep_dir);
	if (ent == NULL) {
		BT_ERR("Unable to create /proc/%s/proto entry", PROC_DIR);
		retval = -ENOMEM;
		goto fail;
	}
	ent->read_proc = bluesleep_read_proc_proto;
	ent->write_proc = bluesleep_write_proc_proto;

	/* read only proc entries */
	if (create_proc_read_entry("asleep", 0,
			sleep_dir, bluesleep_read_proc_asleep, NULL) == NULL) {
		BT_ERR("Unable to create /proc/%s/asleep entry", PROC_DIR);
		retval = -ENOMEM;
		goto fail;
	}

	flags = 0; /* clear all status bits */

	/*20120213*/
	tasklet_init(&hostwake_task,bluesleep_hostwake_task,0);

	/* Initialize spinlock. */
	spin_lock_init(&rw_lock);

	/* Initialize timer */
	init_timer(&tx_timer);
	tx_timer.function = bluesleep_tx_timer_expire;
	tx_timer.data = 0;
	hci_register_notifier(&hci_event_nblock);

	return 0;

fail:
	remove_proc_entry("asleep", sleep_dir);
	remove_proc_entry("proto", sleep_dir);
	remove_proc_entry("hostwake", sleep_dir);
	remove_proc_entry("btwake", sleep_dir);
	remove_proc_entry("sleep", bluetooth_dir);
	remove_proc_entry("bluetooth", 0);
	return retval;
}

/**
 * Cleans up the module.
 */
static void __exit bluesleep_exit(void)
{
	hci_unregister_notifier(&hci_event_nblock);
	platform_driver_unregister(&bluesleep_driver);

	remove_proc_entry("asleep", sleep_dir);
	remove_proc_entry("proto", sleep_dir);
	remove_proc_entry("hostwake", sleep_dir);
	remove_proc_entry("btwake", sleep_dir);
	remove_proc_entry("sleep", bluetooth_dir);
	remove_proc_entry("bluetooth", 0);
}

module_init(bluesleep_init);
module_exit(bluesleep_exit);

MODULE_DESCRIPTION("Bluetooth Sleep Mode Driver ver %s " VERSION);
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif