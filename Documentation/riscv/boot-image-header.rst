=================================
Boot image header in RISC-V Linux
=================================

:Author: Atish Patra <atish.patra@wdc.com>
:Date:   20 May 2019

This document only describes the boot image header details for RISC-V Linux.

TODO:
  Write a complete booting guide.

The following 64-byte header is present in decompressed Linux kernel image::

	u32 code0;		  /* Executable code */
	u32 code1;		  /* Executable code */
	u64 text_offset;	  /* Image load offset, little endian */
	u64 image_size;		  /* Effective Image size, little endian */
	u64 flags;		  /* kernel flags, little endian */
	u32 version;		  /* Version of this header */
	u32 res1 = 0;		  /* Reserved */
	u64 res2 = 0;		  /* Reserved */
	u64 magic = 0x5643534952; /* Magic number, little endian, "RISCV" */
	u32 res3;		  /* Reserved for additional RISC-V specific header */
	u32 res4;		  /* Reserved for PE COFF offset */

This header format is compliant with PE/COFF header and largely inspired from
ARM64 header. Thus, both ARM64 & RISC-V header can be combined into one common
header in future.

Notes
=====

- This header can also be reused to support EFI stub for RISC-V in future. EFI
  specification needs PE/COFF image header in the beginning of the kernel image
  in order to load it as an EFI application. In order to support EFI stub,
  code0 should be replaced with "MZ" magic string and res5(at offset 0x3c) should
  point to the rest of the PE/COFF header.

- version field indicate header version number

	==========  =============
	Bits 0:15   Minor version
	Bits 16:31  Major version
	==========  =============

  This preserves compatibility across newer and older version of the header.
  The current version is defined as 0.1.

- res3 is reserved for offset to any other additional fields. This makes the
  header extendible in future. One example would be to accommodate ISA
  extension for RISC-V in future. For current version, it is set to be zero.

- In current header, the flag field has only one field.

	=====  ====================================
	Bit 0  Kernel endianness. 1 if BE, 0 if LE.
	=====  ====================================

- Image size is mandatory for boot loader to load kernel image. Booting will
  fail otherwise.
