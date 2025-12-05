/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ARM64_KVM_NVHE_SERIAL_H__
#define __ARM64_KVM_NVHE_SERIAL_H__

void hyp_puts(const char *s);
void hyp_putx64(u64 x);
void hyp_putc(char c);
int __pkvm_register_serial_driver(void (*driver_cb)(char));

/*
 * Debug output macros - stub implementations
 *
 * These are placeholder macros that accept printf-style arguments.
 * The actual implementation with hyp_printf() is added in a later commit.
 */
#define hyp_dbg(fmt, ...)	do { } while (0)
#define hyp_info(fmt, ...)	do { } while (0)
#define hyp_err(fmt, ...)	do { } while (0)
#define hyp_warn(fmt, ...)	do { } while (0)

#endif
