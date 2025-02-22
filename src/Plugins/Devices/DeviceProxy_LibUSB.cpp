/*
 * This file is part of USBProxy.
 */

#include <iostream>
#include <sstream>
#include <string>
#include <iomanip>
#include <algorithm>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <thread>
#include "DeviceProxy_LibUSB.h"
#include "TRACE.h"
#include "HexString.h"
#include "get_tid.h"

using namespace std;

#define hex2(VALUE) setfill('0') << setw(2) << hex << (unsigned)VALUE << dec
#define hex4(VALUE) setfill('0') << setw(4) << hex << VALUE << dec
#define MAX_ATTEMPTS 5

int resetCount = 1;

static DeviceProxy_LibUSB *proxy;



extern "C" {
// for handling events of hotplug.

static void disconnect_notifier( DeviceProxy_LibUSB* deviceProxy) {
	deviceProxy->please_stop = true;
	if (nullptr !=deviceProxy->disconnectNotifierCallback) {
		deviceProxy->disconnectNotifierCallback();
	} else {
		// todo from old code not sure why?
		sleep(1);
		kill(0, SIGHUP);
	}
}

static int hotplug_callback(struct libusb_context *ctx, struct libusb_device *dev, libusb_hotplug_event envet,
		void *user_data) {
	disconnect_notifier((DeviceProxy_LibUSB*)user_data);
	return 0;
}

DeviceProxy * get_deviceproxy_plugin(ConfigParser *cfg) {
	proxy = new DeviceProxy_LibUSB(cfg);
	return (DeviceProxy *) proxy;
}

void destroy_plugin() {
	delete proxy;
}
}

DeviceProxy_LibUSB::DeviceProxy_LibUSB(int vendorId, int productId, bool includeHubs)
	: libusbThread()
	, please_stop(false)
	, rxAttempt(0)
{
	context = NULL;
	dev_handle = NULL;

	// for handling events of hotplug.
	callback_handle = -1;

	privateContext = true;
	privateDevice = true;
	desired_vid = vendorId;
	desired_pid = productId;
	desired_hubs = includeHubs;

	for (unsigned int i = 0; i < sizeof(epInterfaces) / sizeof(*epInterfaces); ++i) {
		epInterfaces[i].interface = 0;
		epInterfaces[i].defined = false;
		epInterfaces[i].claimed = false;
	}
}

DeviceProxy_LibUSB::DeviceProxy_LibUSB(ConfigParser *cfg) :
		DeviceProxy(*cfg)
		, libusbThread()
		, please_stop(false)
		, rxAttempt(0)
	{
	int vendorId, productId;
	string str;

	str = cfg->get("vendorId");
	if (str == "")
		vendorId = LIBUSB_HOTPLUG_MATCH_ANY;
	else
		vendorId = stoi(str, nullptr, 16);
	cerr << "vendorId = " << hex4(vendorId) << endl;

	str = cfg->get("productId");
	if (str == "")
		productId = LIBUSB_HOTPLUG_MATCH_ANY;
	else
		productId = stoi(str, nullptr, 16);
	cerr << "productId = " << hex4(productId) << endl;

	str = cfg->get("DeviceProxy::nice");
	if (str == "")
		nice = 0;
	else
		nice = stoi(str, nullptr, 10);
	cerr << "DeviceProxy::nice = " << nice << endl;

	bool includeHubs = false;

	context = NULL;
	dev_handle = NULL;

	// for handling events of hotplug.
	callback_handle = -1;

	privateContext = true;
	privateDevice = true;
	desired_vid = vendorId;
	desired_pid = productId;
	desired_hubs = includeHubs;

	for (unsigned int i = 0; i < sizeof(epInterfaces) / sizeof(*epInterfaces); ++i) {
		epInterfaces[i].interface = 0;
		epInterfaces[i].defined = false;
		epInterfaces[i].claimed = false;
	}
}

DeviceProxy_LibUSB::~DeviceProxy_LibUSB() {
	// for handling events of hotplug.
	please_stop = true;
	if (context && callback_handle != -1) {
		libusb_hotplug_deregister_callback(context, callback_handle);
	}

	if (privateDevice && dev_handle) {
		libusb_close(dev_handle);
	}
	if (privateContext && context) {
		libusb_exit(context);
	}
	if (libusbThread.joinable()) {
		libusbThread.join();
	}
}

void DeviceProxy_LibUSB::setNice(unsigned nice_)
{
	nice = nice_;
}

int DeviceProxy_LibUSB::connect(int timeout) {
	return connect(desired_vid, desired_pid, desired_hubs);
}


#if 0 // not used
int DeviceProxy_LibUSB::connect(libusb_device* dvc, libusb_context* _context) {
	if (dev_handle) {
		cerr << "LibUSB already connected." << endl;
		return 0;
	}
	privateContext = false;
	context = _context;
	int rc = libusb_open(dvc, &dev_handle);
	if (rc != LIBUSB_SUCCESS) {
		if (debugLevel) {
			cerr << "Error opening device: " << libusb_strerror((libusb_error) rc) << endl;
		}
		dev_handle = NULL;
		return rc;
	}
	if (debugLevel) {
		char * device_desc = toString();
		cout << "Connected to device: " << device_desc << endl;
		free(device_desc);
	}
	return 0;
}

int DeviceProxy_LibUSB::connect(libusb_device_handle* devh, libusb_context* _context) {
	if (dev_handle) {
		cerr << "LibUSB already connected." << endl;
		return 0;
	}
	privateContext = false;
	privateDevice = false;
	context = _context;
	dev_handle = devh;
	if (debugLevel) {
		char * device_desc = toString();
		cout << "Connected to device: " << device_desc << endl;
		free(device_desc);
	}
	return 0;
}
#endif

void DeviceProxy_LibUSB::libusbEventLoop() {
	fprintf(stderr,"Starting libusbEventLoop thread (%ld) \n",gettid());
	while(!please_stop) {
		libusb_handle_events(context);
	}
	fprintf(stderr,"Finished libusbEventLoop thread (%ld) \n",gettid());
}

int DeviceProxy_LibUSB::connect(int vendorId, int productId, bool includeHubs) {
	if (dev_handle) {
		cerr << "LibUSB already connected." << endl;
		return 0;
	}
	privateContext = true;
	privateDevice = true;
	libusb_init(&context);
	// todo change this setting ?

	libusb_set_debug(context,  std::min(static_cast<int>(debugLevel * 2 + 1), 4));

	libusb_device **list = NULL;
	libusb_device *found = NULL;

	ssize_t cnt = libusb_get_device_list(context, &list);
	if (cnt < 0) {
		if (debugLevel) {
			cerr << "Error retrieving device list: " << libusb_strerror((libusb_error) cnt) << endl;
		}
		libusb_exit(context);
		return cnt;
	}

	ssize_t i = 0;

	struct libusb_device_descriptor desc;
	int rc = 0;

	for (i = 0; i < cnt; i++) {
		libusb_device *dvc = list[i];
		rc = libusb_get_device_descriptor(dvc, &desc);
		if (rc != LIBUSB_SUCCESS) {
			if (debugLevel) {
				cerr << "Error retrieving device descriptor: " << libusb_strerror((libusb_error) rc) << endl;
			}
		} else {
			if ((includeHubs || desc.bDeviceClass != LIBUSB_CLASS_HUB)
					&& (vendorId == desc.idVendor || vendorId == LIBUSB_HOTPLUG_MATCH_ANY)
					&& (productId == desc.idProduct || productId == LIBUSB_HOTPLUG_MATCH_ANY)) {
				found = dvc;
				break;
			}
		}
	}

	if (found == NULL) {
		if (debugLevel) {
			cerr << "No device found." << endl;
		}
		libusb_free_device_list(list, 1);
		libusb_exit(context);
		return ENODEV;
	} else {
		rc = libusb_open(found, &dev_handle);
		if (rc != LIBUSB_SUCCESS) {
			if (debugLevel) {
				cerr << "Error opening device handle: " << libusb_strerror((libusb_error) rc) << endl;
			}
			dev_handle = NULL;
			libusb_free_device_list(list, 1);
			libusb_exit(context);
			return rc;
		}
		rc = libusb_reset_device(dev_handle);
		if (rc != LIBUSB_SUCCESS) {
			if (debugLevel) {
				cerr << "Error opening device handle: " << libusb_strerror((libusb_error) rc) << endl;
			}
			dev_handle = NULL;
			libusb_free_device_list(list, 1);
			libusb_exit(context);
			return rc;
		}
	}

	libusb_free_device_list(list, 1);

	// begin
	rc = libusb_set_auto_detach_kernel_driver(dev_handle, 1);
	if (rc != LIBUSB_SUCCESS) {
		cerr << "libusb_set_auto_detach_kernel_driver() failed: " << libusb_strerror((libusb_error) rc) << endl;
		libusb_close(dev_handle);
		libusb_exit(context);
		return rc;
	}
	//end

	//check that device is responsive
	/*unsigned char unused[4];
	rc = libusb_get_string_descriptor(dev_handle, 0, 0, unused, sizeof(unused));
	if (rc < 0) {
		cerr << "Device unresponsive: " << libusb_strerror((libusb_error) rc) << endl;
		libusb_close(dev_handle);
		libusb_exit(context);
		return rc;
	}*/

	if (debugLevel) {
		char * device_desc = toString();
		cout << "Connected to device: " << device_desc << endl;
		free(device_desc);
	}


	// Todo: I don't know how much of a performance impact the libusb idle
	// loop has on performance.  Woud checking read/write status then
	// calling the call back routines on a disconnect be a better solution?

	// for handling events of hotplug.
	if (callback_handle == -1 ) {

		rc = libusb_hotplug_register_callback(
			context,
			LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
			(libusb_hotplug_flag) 0,
			desc.idVendor,
			desc.idProduct,
			LIBUSB_HOTPLUG_MATCH_ANY,
			hotplug_callback,
			this,
			&callback_handle);

		if (rc != LIBUSB_SUCCESS) {
			cerr << "Error registering callback" << endl;
			libusb_close(dev_handle);
			libusb_exit(context);
			return rc;
		}
		else
		{
			please_stop = false;
			libusbThread = std::thread(&DeviceProxy_LibUSB::libusbEventLoop, this);
		}
	}
	return 0;
}

void DeviceProxy_LibUSB::disconnect() {
	// for handling events of hotplug.
	if (context && callback_handle != -1) {
		libusb_hotplug_deregister_callback(context, callback_handle);
	}
	callback_handle = -1;

	if (privateDevice && dev_handle) {
		libusb_close(dev_handle);
	}
	dev_handle = NULL;
	if (privateContext && context) {
		libusb_exit(context);
	}
	context = NULL;
}

void DeviceProxy_LibUSB::reset() {
	int rc = libusb_reset_device(dev_handle);
	if (rc == LIBUSB_ERROR_NOT_FOUND) {
		disconnect();
	}

	if (rc != LIBUSB_SUCCESS) {
		cerr << "Error resetting device: " << libusb_strerror((libusb_error) rc) << endl;
	}
}

bool DeviceProxy_LibUSB::is_connected() {
	if (dev_handle) {
		return true;
	} else {
		return false;
	}
}

bool DeviceProxy_LibUSB::is_highspeed() {
	libusb_device* dvc = libusb_get_device(dev_handle);
	int speed = libusb_get_device_speed(dvc);
	return (speed == LIBUSB_SPEED_HIGH) || (speed == LIBUSB_SPEED_SUPER);
}

char* DeviceProxy_LibUSB::toString() {
	unsigned char str_mfr[128] = "N/A";
	unsigned char str_prd[128] = "N/A";
	struct libusb_device_descriptor desc;
	libusb_device* dvc = libusb_get_device(dev_handle);
	int rc = libusb_get_device_descriptor(dvc, &desc);
	if (rc != LIBUSB_SUCCESS) {
		if (debugLevel) {
			cerr << "Error retrieving device descriptor: " << libusb_strerror((libusb_error) rc) << endl;
		}
	}
	uint8_t address = libusb_get_device_address(dvc);
	if (desc.iManufacturer) {
		rc = libusb_get_string_descriptor_ascii(dev_handle, desc.iManufacturer, str_mfr, sizeof(str_mfr));
		if (rc < 0) {
			if (debugLevel) {
				cerr << "Error retrieving string descriptor: " << libusb_strerror((libusb_error) rc) << endl;
			}
		}
	}
	if (desc.iProduct) {
		rc = libusb_get_string_descriptor_ascii(dev_handle, desc.iProduct, str_prd, sizeof(str_prd));
		if (rc < 0) {
			if (debugLevel) {
				cerr << "Error retrieving string descriptor: " << libusb_strerror((libusb_error) rc) << endl;
			}
		}
	}
	stringstream ss;
	ss << hex4(desc.idVendor) << ":" << hex4(desc.idProduct) << "@" << hex2(address) << " " << str_mfr << " - "
			<< str_prd << endl;
	return strdup(ss.str().c_str());
}

int DeviceProxy_LibUSB::control_request(const usb_ctrlrequest *setup_packet, int *nbytes, unsigned char * dataptr,
		int timeout) {
	if (debugLevel > 1) {
		char* str_hex = hex_string((void*) setup_packet, sizeof(*setup_packet));
		cout << "LibUSB> " << str_hex << endl;
		free(str_hex);
	}
	int rc = libusb_control_transfer(dev_handle, setup_packet->bRequestType, setup_packet->bRequest,
			setup_packet->wValue, setup_packet->wIndex, dataptr, setup_packet->wLength, timeout);
	if (rc < 0) {
		if (debugLevel) {
			cerr << "Error sending setup packet: " << libusb_strerror((libusb_error) rc) << endl;
		}
		if (rc == LIBUSB_ERROR_PIPE)
			return -1;
		return rc;
	}
	if (debugLevel > 1) {
		char* str_hex = hex_string((void*) dataptr, rc);
		cout << "LibUSB< " << str_hex << endl;
		free(str_hex);
	}
	*nbytes = rc;
	return 0;
}

uint8_t DeviceProxy_LibUSB::get_address() {
	libusb_device* dvc = libusb_get_device(dev_handle);
	return libusb_get_device_address(dvc);
}

bool DeviceProxy_LibUSB::endpoint_interface_claimed(uint8_t endpoint) {

	if (!epInterfaces[endpoint & 0x0F].defined) {
		cerr << "No interface defined for endpoint: " << hex2(endpoint) << endl;
		return false;
	}

	if (!epInterfaces[endpoint & 0x0F].claimed) {
		return false;
	}

	return true;
}

void DeviceProxy_LibUSB::send_data(uint8_t endpoint, uint8_t attributes, uint16_t maxPacketSize, uint8_t* dataptr,
		int length) {

	if (!endpoint_interface_claimed(endpoint)) {
		//do not try to send if interface wasn't claimed successfully
		return;
	}

	int transferred;
	int attempt = 0;
	int rc = LIBUSB_SUCCESS;

	switch (attributes & USB_ENDPOINT_XFERTYPE_MASK) {
	case USB_ENDPOINT_XFER_CONTROL:
		cerr << "Can't send on a control endpoint." << endl;
		break;
	case USB_ENDPOINT_XFER_ISOC:
		//TODO handle isochronous
		cerr << "Isochronous endpoints unhandled." << endl;
		break;
	case USB_ENDPOINT_XFER_BULK:
		do {
			rc = libusb_bulk_transfer(dev_handle, endpoint, dataptr, length, &transferred, 0);
			//TODO retry transfer if incomplete
			if (transferred != length)
				cerr << "Incomplete Bulk transfer on EP" << hex2(endpoint) << endl;
			if (rc == LIBUSB_SUCCESS && debugLevel > 2)
				cerr << "Sent " << transferred << " bytes (Bulk) to libusb EP" << hex2(endpoint) << endl;
			if ((rc == LIBUSB_ERROR_PIPE || rc == LIBUSB_ERROR_TIMEOUT))
				libusb_clear_halt(dev_handle, endpoint);
			attempt++;
		} while ((rc == LIBUSB_ERROR_PIPE || rc == LIBUSB_ERROR_TIMEOUT || transferred != length) && (attempt < MAX_ATTEMPTS) && (!please_stop));
		break;
	case USB_ENDPOINT_XFER_INT:
		rc = libusb_interrupt_transfer(dev_handle, endpoint, dataptr, length, &transferred, 0);
		//TODO retry transfer if incomplete
		if (transferred != length)
			cerr << "Incomplete Interrupt transfer on EP" << hex2(endpoint) << endl;
		if (rc == LIBUSB_SUCCESS && debugLevel > 2)
			cerr << "Sent " << transferred << " bytes (Int) to libusb EP" << hex2(endpoint) << endl;
		break;
	}
	if ((rc != LIBUSB_SUCCESS) && (!please_stop)) {
		// todo we get this error message when connection is lost... how to filter?
		cerr << "Transfer error sending on EP" << hex2(endpoint) << " (xfertype "
				<< unsigned(attributes & USB_ENDPOINT_XFERTYPE_MASK) << ")" << ": "
				<< libusb_strerror((libusb_error) rc) << endl;
	}
}

void DeviceProxy_LibUSB::receive_data(uint8_t endpoint, uint8_t attributes, uint16_t maxPacketSize, uint8_t ** dataptr,
		int* length, int timeout) {

	if (!endpoint_interface_claimed(endpoint)) {
		//do not try to receive if interface wasn't claimed successfully
		return;
	}

	int rc = LIBUSB_SUCCESS;
	*dataptr = nullptr;
	*length = 0;


	//if (timeout < 10)
		//timeout = 10;	//TODO: explain this!

	/*
	 * Infinite timeout to avoid stalling with mass storages, temporarily assigned here.
	 * Need to check if this is useful or only do-while is needed. But for now it works.
	 */
	timeout = 0;

	switch (attributes & USB_ENDPOINT_XFERTYPE_MASK) {
	case USB_ENDPOINT_XFER_CONTROL:
		cerr << "Can't send on a control endpoint." << endl;
		break;

	case USB_ENDPOINT_XFER_ISOC:
		//TODO handle isochronous
		cerr << "Isochronous endpoints unhandled." << endl;
		break;

	case USB_ENDPOINT_XFER_BULK:
		// On a system where there is lots of output and very
		// little input (i.e. a printer), continuously doing reads
		// can severely impact performance.  As a simple solution to
		// resolve this we throttle the polling for "DeviceProxy::nice"
		// ms between reads.  This depends heavily upon the application.

		// Zero Lenth Packets (ZLP) can be a problem.  If all ZLPs are
		// passed the performance will suffer and buffers may
		// overflow.  ZLP cannot be completely blocked as they are
		// required in some cases by the usb protocols (i.e. used for
		// bulk transfers that are less than the requested size and a
		// multiple of the maximum packet size).  I have also found that
		// some drivers expect ZLP for no data and will have problems
		// if they just get NAKs (gadgetfs default way to present no
		// data).
		//
		// Two methods are used to reduce ZLPs.  relay_read only sends a
		// ZLP after a non ZLP, or when the sendQuee is empty.
		// The flow of ZLP can is also  limited  with
		// DeviceProxy::nice.  DeviceProxy::nice reduces the device
		// polling rate which  also reduces the ZLP rate.
		//
		// todo do we want to change this so we only read ( at nice 0)
		// when the receive queue is empty???  will it cause worse or
		// better latencies? how feasible is it? (how should we wait for
		// an empty queue)
		//
		// todo: review changing the operations to asynchronous to allow
		// cancelling of operations.  this alos applies to send_data


		if ((0 == rxAttempt) && (0 != nice)){
			std::this_thread::sleep_for(std::chrono::milliseconds(nice));
			rxAttempt++;
			return;
		}

		*dataptr = (uint8_t *) malloc(maxPacketSize * 8);
		rc = libusb_bulk_transfer(dev_handle, endpoint, *dataptr, maxPacketSize, length, timeout);
		if (rc == LIBUSB_SUCCESS && debugLevel > 2) {
			cerr << "received bulk msg (" << *length << " bytes)" << endl;
		}
		if ((rc == LIBUSB_ERROR_PIPE || rc == LIBUSB_ERROR_TIMEOUT)) {
			libusb_clear_halt(dev_handle, endpoint);
		}
		rxAttempt++;
		break;

	case USB_ENDPOINT_XFER_INT:
		*dataptr = (uint8_t *) malloc(maxPacketSize);
		rc = libusb_interrupt_transfer(dev_handle, endpoint, *dataptr, maxPacketSize, length, timeout);
		if (rc == LIBUSB_SUCCESS && debugLevel > 2)
			cerr << "received int msg (" << *length << " bytes)" << endl;
		break;
	}
	if (rc != LIBUSB_SUCCESS) {
		free(*dataptr);
		*dataptr = nullptr;
		*length = 0;
	} else {
		rxAttempt = 0;
	}

	if ((rc != LIBUSB_SUCCESS) && (rxAttempt > MAX_ATTEMPTS)) {
		cerr << "Transfer error receiving on EP" << hex2(endpoint) << " (xfertype "
				<< unsigned(attributes & USB_ENDPOINT_XFERTYPE_MASK) << ")" << ": "
				<< libusb_strerror((libusb_error) rc) << endl;
		rxAttempt = 0;
	}
}

void DeviceProxy_LibUSB::set_endpoint_interface(uint8_t endpoint, uint8_t interface) {
	epInterfaces[endpoint & 0x0F].defined = true;
	epInterfaces[endpoint & 0x0F].interface = interface;
}

void DeviceProxy_LibUSB::claim_interface(uint8_t interface) {
	if (is_connected()) {
		int rc = libusb_claim_interface(dev_handle, interface);
		if (rc != LIBUSB_SUCCESS) {
			cerr << "Error claiming interface " << (unsigned) interface << ": " << libusb_strerror((libusb_error) rc)
					<< endl;
		} else {
			for (unsigned int i = 0; i < sizeof(epInterfaces) / sizeof(*epInterfaces); ++i) {
				if (epInterfaces[i].defined && epInterfaces[i].interface == interface) {
					epInterfaces[i].claimed = true;
				}
			}
		}
	}
}

void DeviceProxy_LibUSB::release_interface(uint8_t interface) {
	if (is_connected()) {
		int rc = libusb_release_interface(dev_handle, interface);
		if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_NOT_FOUND && rc != LIBUSB_ERROR_NO_DEVICE) {
			cerr << "Error releasing interface " <<
			    (unsigned) interface << ": " << rc << " " <<
			    libusb_strerror((libusb_error) rc) << endl;
		} else {
			for (unsigned int i = 0; i < sizeof(epInterfaces) / sizeof(*epInterfaces); ++i) {
				if (epInterfaces[i].defined && epInterfaces[i].interface == interface) {
					epInterfaces[i].claimed = false;
				}
			}
		}
	}
}




