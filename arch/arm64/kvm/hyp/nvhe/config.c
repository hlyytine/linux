// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Fuad Tabba <tabba@google.com>
 */

/*
 * We need the code in `arch/arm64/kvm/config.c` to be compiled for the nVHE
 * hypervisor. It's not feasible to have all that as inline code. Therefore,
 * ensure that it's build for nVHE as well.
 */

#include "../../config.c"
