.. SPDX-License-Identifier: GPL-2.0

Loopback Block Device
=====================

Overview
--------

The loopback device driver allows you to use a regular file as a block device.
You can then create a file system on that block device and mount it just as you
would mount other block devices such as hard drive partitions, CD-ROM drives or
floppy drives. The loop devices are block special device files with major
number 7 and typically called /dev/loop0, /dev/loop1 etc.

To use the loop device, you need the losetup utility, found in the `util-linux
package <https://www.kernel.org/pub/linux/utils/util-linux/>`_.

.. note::
	Note that this loop device has nothing to do with the loopback device \
	used for network connections from the machine to itself.


Parameters
----------

Kernel Command Line Parameters
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	max_loop
		The number of loop block devices that get unconditionally
		pre-created at init time. The default number is configured by
		BLK_DEV_LOOP_MIN_COUNT. Instead of statically allocating a
		predefined number, loop devices can be requested on-demand
		with the /dev/loop-control interface.


Module parameters
~~~~~~~~~~~~~~~~~

	max_part
		Maximum number of partitions per loop device (default: 0).

		If max_part is given, partition scanning is globally enabled
		for all loop devices.

	max_loop
		Maximum number of loop devices that should be initialized
		(default: 8). The default number is configured by
		BLK_DEV_LOOP_MIN_COUNT.


File format drivers
-------------------

The loopback device driver provides an interface for kernel modules to
implement custom file formats. By default, an initialized loop device uses the
**RAW** file format driver.

.. note::
	If you want to create and set up a new loop device with the losetup \
	utility make sure that the suitable file format driver is loaded \
	before.

The following file format drivers are available.


RAW
~~~

The RAW file format driver implements the binary reading and writing of a disk
image file. It supports discarding, asynchrounous IO, flushing and cryptoloop
support.

The driver's kernel module is named *loop_file_fmt_raw*.
