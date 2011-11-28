/**
 ******************************************************************************
 *
 * @file       pios_usb_hid.c   
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * 	        Parts by Thorsten Klose (tk@midibox.org) (tk@midibox.org)
 * @brief      USB commands. Inits USBs, controls USBs & Interupt handlers.
 * @see        The GNU Public License (GPL) Version 3
 * @defgroup   PIOS_USB USB Functions
 * @{
 *
 *****************************************************************************/
/* 
 * This program is free software; you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License as published by 
 * the Free Software Foundation; either version 3 of the License, or 
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY 
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
 * for more details.
 * 
 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */


/* Project Includes */
#include "pios.h"

#if defined(PIOS_INCLUDE_USB_HID)

#include <signal.h>
#include <errno.h>
#include <pios_usb_hid_priv.h>

/* We need a list of USB devices */

#define PIOS_USB_MAX_DEV 1
static int8_t pios_usb_num_devices = 0;

static pios_usb_dev pios_usb_devices[PIOS_USB_MAX_DEV];



/* Provide a COM driver */
static void PIOS_USB_RegisterRxCallback(uint32_t usb_id, pios_com_callback rx_in_cb, uint32_t context);
static void PIOS_USB_RegisterTxCallback(uint32_t usb_id, pios_com_callback tx_out_cb, uint32_t context);
static void PIOS_USB_TxStart(uint32_t usb_id, uint16_t tx_bytes_avail);
static void PIOS_USB_RxStart(uint32_t usb_id, uint16_t rx_bytes_avail);

const struct pios_com_driver pios_usb_com_driver = {
	.tx_start   = PIOS_USB_TxStart,
	.rx_start   = PIOS_USB_RxStart,
	.bind_tx_cb = PIOS_USB_RegisterTxCallback,
	.bind_rx_cb = PIOS_USB_RegisterRxCallback,
};


static pios_usb_dev * find_usb_dev_by_id (uint8_t usb)
{
  if (usb >= pios_usb_num_devices) {
    /* Undefined USB port for this board (see pios_board.c) */
	PIOS_Assert(0);
    return NULL;
  }

  /* Get a handle for the device configuration */
  return &(pios_usb_devices[usb]);
}


static int32_t usbhid_send(usb_dev_handle *device, int32_t endpoint, void *buf, int32_t len, int32_t timeout)
{
	if (!buf) return -EINVAL;
	if (!device) return -EINVAL;
	if (!endpoint) return -EINVAL;

	int32_t ret=usb_interrupt_write(device, endpoint, (char *)buf, len, timeout);
	if (ret<0) {
		fprintf(stderr,"usb tx error %i\n (%s)\n",ret,strerror(-ret));
	}
	return ret;
}

static int32_t usbhid_receive(usb_dev_handle *device, int32_t endpoint, void *buf, int32_t len, int32_t timeout)
{
	if (!buf) return -EINVAL;
	if (!device) return -EINVAL;
	if (!endpoint) return -EINVAL;

	int32_t ret=usb_interrupt_read(device, endpoint, (char *)buf, len, timeout);
	if (ret<0 &&ret!=-ETIMEDOUT) {
		fprintf(stderr,"usb rx error %i : (%s)\n",ret,strerror(-ret));
	}
	return ret;
}

/**
* Open USB socket
*/
uint32_t usbhid_open(pios_usb_dev * usb_dev) {
  /* find and open USB device */
  struct usb_bus *bus;
  struct usb_device *dev;
  struct usb_interface *iface;
  struct usb_interface_descriptor *desc;
  struct usb_endpoint_descriptor *ep;

  usb_init();
  usb_find_busses();
  usb_find_devices();

  uint8_t claimed = 0;
  for (bus = usb_get_busses(); bus; bus = bus->next) {
	for (dev = bus->devices; dev; dev = dev->next) {
		if (dev->descriptor.idVendor != usb_dev->cfg->vendor) continue;
		if (dev->descriptor.idProduct != usb_dev->cfg->product) continue;
		if (!dev->config) continue;
		if (dev->config->bNumInterfaces < 1) continue;
		printf("USB: found device: vid=%04X, pic=%04X, with %d interfaces\n",
	   dev->descriptor.idVendor,
	   dev->descriptor.idProduct,
	   dev->config->bNumInterfaces);
		iface = dev->config->interface;
		usb_dev->device = NULL;
		uint32_t i;
		for (i=0; i<dev->config->bNumInterfaces && iface; i++, iface++)
		{
			desc = iface->altsetting;
			if (!desc) continue;

			printf("  type %d, %d, %d\n", desc->bInterfaceClass, desc->bInterfaceSubClass, desc->bInterfaceProtocol);

			if (desc->bInterfaceClass != 3) continue;
			if (desc->bInterfaceSubClass != 0) continue;
			if (desc->bInterfaceProtocol != 0) continue;

			ep = desc->endpoint;
			usb_dev->endpoint_in = usb_dev->endpoint_out = 0;
			uint32_t n;
			for (n = 0; n < desc->bNumEndpoints; n++, ep++)
			{
				if (ep->bEndpointAddress & 0x80)
				{
					if (!usb_dev->endpoint_in) usb_dev->endpoint_in = ep->bEndpointAddress & 0x7F;
					printf("    IN endpoint %X\n",usb_dev->endpoint_in);
				}
				else
				{
					if (!usb_dev->endpoint_out) usb_dev->endpoint_out = ep->bEndpointAddress;
					printf("    OUT endpoint %X\n",usb_dev->endpoint_out);
				}
			}
			if (!usb_dev->endpoint_in) continue;

			if (!usb_dev->device)
			{
				usb_dev->device = usb_open(dev);
				if (!usb_dev->device)
				{
					printf("  unable to open device\n");
					break;
				}
			}
			printf("  hid interface (generic)\n");
			if (usb_get_driver_np(usb_dev->device, i, (char *)usb_dev->rx_buffer, sizeof(usb_dev->rx_buffer)) >= 0)
			{
				printf("  in use by driver \"%s\"", usb_dev->rx_buffer);
				if (usb_detach_kernel_driver_np(usb_dev->device, i) < 0)
				{
					printf("  unable to detach from kernel");
					continue;
				}
			}

			if (usb_claim_interface(usb_dev->device, i) < 0)
			{
				printf("  unable claim interface %d", i);
				continue;
			}

			int32_t len;
			len = usb_control_msg(usb_dev->device, 0x81, 6, 0x2200, i, (char *)usb_dev->rx_buffer, sizeof(usb_dev->rx_buffer), 250);
			printf("  descriptor, len=%d\n", len);
			if (len < 2)
			{
				usb_release_interface(usb_dev->device, i);
				continue;
			}

			printf("found :)))) \n");

			claimed=1;
			break;
		}

		if (usb_dev->device && !claimed) {
			usb_close(usb_dev->device);
			usb_dev->device = NULL;
		}
	}
  }

  if (!claimed) {
  	return -1;
  }

  return 0;
}

/**
 * TxThread
 */
void * PIOS_USB_TxThread(void * usb_dev_n)
{

	pios_usb_dev * usb_dev = (pios_usb_dev*) usb_dev_n;

	/**
	* com devices never get closed except by application "reboot"
	* we also never give up our mutex except for waiting
	*/
	while(1) {
		int32_t length,remaining;
		bool tx_need_yield = false;

		if (usb_dev->tx_out_cb) {
			do {
				remaining = (usb_dev->tx_out_cb)(usb_dev->tx_out_context, usb_dev->tx_buffer+2, PIOS_USB_RX_BUFFER_SIZE-2, NULL, &tx_need_yield);
				if (remaining>0) {
					usb_dev->tx_buffer[0]=2;
					usb_dev->tx_buffer[1]=(uint8_t)(remaining & 0xff);
					
					length = usbhid_send(usb_dev->device, usb_dev->endpoint_out, usb_dev->tx_buffer, PIOS_USB_RX_BUFFER_SIZE, 100); 
					if (length <0 && length != -EINVAL && length != -ETIMEDOUT) {
						usb_dev->device=NULL;
						break;
					}
				}
			} while (remaining>0);
		}
#if defined(PIOS_INCLUDE_FREERTOS)
		usb_dev->txBytesAvailable = 0;
		while (usb_dev->txBytesAvailable == 0) {
			vPortYield();
		}
#else
		// wait until data is ready to be sent
		pthread_mutex_lock( &usb_dev->mutex );
		pthread_cond_wait( &usb_dev->cond, &usb_dev->mutex );
		pthread_mutex_unlock( &usb_dev->mutex );
#endif	/* PIOS_INCLUDE_FREERTOS */


	}


}

/**
 * RxThread
 */
void * PIOS_USB_RxThread(void * usb_dev_n)
{


	pios_usb_dev * usb_dev = (pios_usb_dev*) usb_dev_n;

	const struct timespec sleeptime = {
		.tv_sec=1,
		.tv_nsec=0,
	};

	/**
	* com devices never get closed except by application "reboot"
	* we also never give up our mutex except for waiting
	*/
	while(1) {
		/**
		 * receive 
		 */
		int32_t received;
		if ((received = usbhid_receive(usb_dev->device,
				usb_dev->endpoint_in,
				&usb_dev->rx_buffer,
				PIOS_USB_RX_BUFFER_SIZE,
				0)) >= 0)
		{
			
			bool rx_need_yield = false;
			if (received==PIOS_USB_RX_BUFFER_SIZE && usb_dev->rx_in_cb) {
				received = usb_dev->rx_buffer[1];
				if (received>PIOS_USB_RX_BUFFER_SIZE-2) received = PIOS_USB_RX_BUFFER_SIZE-2;
				(void) (usb_dev->rx_in_cb)(usb_dev->rx_in_context, usb_dev->rx_buffer+2, received, NULL, &rx_need_yield);
			}

#if defined(PIOS_INCLUDE_FREERTOS)
			if (rx_need_yield) {
				vPortYield();
			}
#endif	/* PIOS_INCLUDE_FREERTOS */

		}
		if (received <0 && received != -EINVAL && received != -ETIMEDOUT) {
			usb_dev->device=NULL;
		}
		// delay if no device
		if (!usb_dev->device) {
			nanosleep(&sleeptime,NULL);
  			usbhid_open(usb_dev);
		}
	}
}



/**
* Init USB
*/
int32_t PIOS_USB_HID_Init(uint32_t * usb_id, const struct pios_usb_hid_cfg * cfg)
{

  pios_usb_dev * usb_dev = &pios_usb_devices[pios_usb_num_devices];

  pios_usb_num_devices++;


  /* initialize */
  usb_dev->rx_in_cb = NULL;
  usb_dev->tx_out_cb = NULL;
  usb_dev->cfg = cfg;
  usb_dev->device = NULL;

  int res=0;

  /* Create transmit thread for this connection */
#if defined(PIOS_INCLUDE_FREERTOS)
  xTaskCreate((pdTASK_CODE)PIOS_USB_RxThread, (const signed char *)"USBHID_Rx_Thread",1024,(void*)usb_dev,(tskIDLE_PRIORITY + 1),&usb_dev->rxThread);
  xTaskCreate((pdTASK_CODE)PIOS_USB_TxThread, (const signed char *)"USBHID_Tx_Thread",1024,(void*)usb_dev,(tskIDLE_PRIORITY + 1),&usb_dev->txThread);
#else
  pthread_create(&usb_dev->rxThread, NULL, PIOS_USB_RxThread, (void*)usb_dev);
  pthread_create(&usb_dev->txThread, NULL, PIOS_USB_TxThread, (void*)usb_dev);
#endif

  printf("USB dev %i opened...\n",pios_usb_num_devices-1);

  *usb_id = pios_usb_num_devices-1;

  return res;
}



static void PIOS_USB_RxStart(uint32_t usb_id, uint16_t rx_bytes_avail)
{
	/**
	 * lazy! (and it doesn't make sense to do any intermediate buffering)
	 */
}


static void PIOS_USB_TxStart(uint32_t usb_id, uint16_t tx_bytes_avail)
{
	pios_usb_dev * usb_dev = find_usb_dev_by_id(usb_id);

	PIOS_Assert(usb_dev);

#if defined(PIOS_INCLUDE_FREERTOS)
	usb_dev->txBytesAvailable = tx_bytes_avail;
#else
	pthread_cond_signal(&usb_dev->cond);
#endif

}

static void PIOS_USB_RegisterRxCallback(uint32_t usb_id, pios_com_callback rx_in_cb, uint32_t context)
{
	pios_usb_dev * usb_dev = find_usb_dev_by_id(usb_id);

	PIOS_Assert(usb_dev);

	/* 
	 * Order is important in these assignments since ISR uses _cb
	 * field to determine if it's ok to dereference _cb and _context
	 */
	usb_dev->rx_in_context = context;
	usb_dev->rx_in_cb = rx_in_cb;
}

static void PIOS_USB_RegisterTxCallback(uint32_t usb_id, pios_com_callback tx_out_cb, uint32_t context)
{
	pios_usb_dev * usb_dev = find_usb_dev_by_id(usb_id);

	PIOS_Assert(usb_dev);

	/* 
	 * Order is important in these assignments since ISR uses _cb
	 * field to determine if it's ok to dereference _cb and _context
	 */
	usb_dev->tx_out_context = context;
	usb_dev->tx_out_cb = tx_out_cb;
}


int32_t PIOS_USB_HID_CheckAvailable(uint8_t id) {
	pios_usb_dev * usb_dev = find_usb_dev_by_id(id);
	if (!usb_dev) return false;
	if (!usb_dev->device) return false;
	return true;
}


#endif
