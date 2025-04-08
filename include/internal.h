// SPDX-License-Identifier: MIT

#ifndef INTERNAL_H
#define INTERNAL_H

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

#define FILESYSTEM_DISPLAYNAME "yuki"

# define __INT64_C(c)	c ## L
# define __UINT64_C(c)	c ## UL

# define UINT8_MAX		(255)
# define UINT16_MAX		(65535)
# define UINT32_MAX		(4294967295U)
# define UINT64_MAX		(__UINT64_C(18446744073709551615))

#endif /* INTERNAL_H */