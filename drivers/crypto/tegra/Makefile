// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

tegra-se-objs := tegra-se-key.o tegra-se-main.o

tegra-se-y += tegra-se-aes.o
tegra-se-y += tegra-se-hash.o

# Only build in-tree driver if NOT using NVIDIA OOT drivers
ifndef CONFIG_TEGRA_HOST1X_NVIDIA_OOT
obj-$(CONFIG_CRYPTO_DEV_TEGRA) += tegra-se.o
endif
