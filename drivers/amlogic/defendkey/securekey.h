 /*
  * drivers/amlogic/defendkey/defendkey.c
  *
  * Copyright (C) 2015 Amlogic, Inc. All rights reserved.
  *
  * This program is free software; you can redistribute it and/or modify
  * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation; either version 2 of the License, or
  * (at your option) any later version.
  *
  * This program is distributed in the hope that it will be useful, but WITHOUT
  * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
  * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
  * more details.
  */

#ifdef CONFIG_ARM64
#define AML_D_P_UPGRADE_CHECK   (0x80)
#define AML_DATA_PROCESS		(0x820000FF)

int aml_is_secure_set(void);
unsigned long aml_sec_boot_check(unsigned long nType,
	unsigned long pBuffer,
	unsigned long nLength,
	unsigned long nOption);
#endif
