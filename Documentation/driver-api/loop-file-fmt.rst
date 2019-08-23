.. SPDX-License-Identifier: GPL-2.0

===========================================
Loopback block device file format subsystem
===========================================

This document outlines the file format subsystem used in the loopback block
device module. This subsystem deals with the abstraction of direct file access
to allow the implementation of various disk file formats. The subsystem can
handle ...

   - read
   - write
   - discard
   - flush
   - sector size

... operations of a loop device.

Therefore, the subsystem provides an internal API for the loop device module to
access its functionality and exports a file format driver API to implement any
file format driver for loop devices.


Use the file format subsystem
=============================

At the moment, the file format subsystem is only intended to be used from the
loopback device module to provide a specific file format implementation per
configured loop device. Therefore, the loop device module can use the following
internal file format API functions to set up loop file formats and access the
file format subsystem.


Internal subsystem API
----------------------

.. kernel-doc:: drivers/block/loop/loop_file_fmt.h
   :functions: loop_file_fmt_alloc loop_file_fmt_free \
               loop_file_fmt_set_lo loop_file_fmt_get_lo
               loop_file_fmt_init loop_file_fmt_exit \
               loop_file_fmt_read loop_file_fmt_read_aio \
               loop_file_fmt_write loop_file_fmt_write_aio \
               loop_file_fmt_discard loop_file_fmt_flush \
               loop_file_fmt_sector_size loop_file_fmt_change


Finite state machine
--------------------

To prevent a misuse of the internal file format API, the file format subsystem
implements an finite state machine. The state machine consists of two states
and a transition for each internal API function. The state
*file_fmt_uninitialized* of a loop file format denotes that the file format is
already allocated but not initialized. After the initialization, the file
format's state is set to *file_fmt_initialized*. In this state, all IO related
file format operations can be accessed.

.. note:: If an internal API call does not succeed the file format's state \
          does not change accordingly to its transition and remains in the \
          original state before the API call.

The entire implemented finite state machine looks like the following:

.. kernel-render:: DOT
   :alt: loop file format states
   :caption: File format states and transitions

   digraph file_fmt_states {
       rankdir = LR;
       node [ shape = point,        label = "" ] ENTRY, EXIT;
       node [ shape = circle,       label = "file_fmt_uninitialized" ] UN;
       node [ shape = doublecircle, label = "file_fmt_initialized" ]   IN;
       subgraph helper {
           rank = "same";
           ENTRY -> UN   [ label = "loop_file_fmt_alloc()" ];
           UN    -> EXIT [ label = "loop_file_fmt_free()" ];
       }
       UN    -> IN   [ label = "loop_file_fmt_init()" ];
       IN    -> UN   [ label = "loop_file_fmt_exit()" ];
       IN    -> IN   [ label = "loop_file_fmt_read()\nloop_file_fmt_read_aio()\nloop_file_fmt_write()\n loop_file_fmt_write_aio()\nloop_file_fmt_discard()\nloop_file_fmt_flush()\nloop_file_fmt_sector_size()\nloop_file_fmt_change()" ];
   }


Write file format drivers
=========================

A file format driver for the loop file format subsystem is implemented as
kernel module. In the kernel module's code, the file format driver structure is
statically allocated and must be initialized. An example definition would look
like::

   struct loop_file_fmt_driver raw_file_fmt_driver = {
       .name          = "RAW",
       .file_fmt_type = LO_FILE_FMT_RAW,
       .ops           = &raw_file_fmt_ops,
       .owner         = THIS_MODULE
   };

The definition assigns a *name* to the file format driver. The *file_fmt_type*
field is set to the file format type that the driver implements. The *owner*
specifies the driver's owner and is used to lock the kernel module of the
driver if the file format driver is in use. The most important field of a loop
file format driver is the specification of its implementation. Therefore, the
*ops* field proposes all file format operations that the driver implement by
link to a statically allocated operations structure.

.. note:: All fields of the **loop_file_fmt_driver** structure must be \
          initialized and set up accordingly, otherwise the driver does not \
          work properly.

An example of such an operations structure looks like::

   struct loop_file_fmt_ops raw_file_fmt_ops = {
       .init        = NULL,
       .exit        = NULL,
       .read        = raw_file_fmt_read,
       .write       = raw_file_fmt_write,
       .read_aio    = raw_file_fmt_read_aio,
       .write_aio   = raw_file_fmt_write_aio,
       .discard     = raw_file_fmt_discard,
       .flush       = raw_file_fmt_flush,
       .sector_size = raw_file_fmt_sector_size
   };

The operations structure consists of a bunch of functions pointers which are
set in this example to some functions of the binary raw disk file format
implemented in the example driver. If a function is not available in the
driver's implementation the function pointer in the operations structure must
be set to *NULL*.

If all definitions are available and set up correctly the driver can be
registered and later on unregistered by using the following functions exported
by the file format subsystem:

.. kernel-doc:: drivers/block/loop/loop_file_fmt.h
   :functions: loop_file_fmt_register_driver loop_file_fmt_unregister_driver
