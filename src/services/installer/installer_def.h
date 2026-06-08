/*
 * jappeos_core, Core system management daemon for JappeOS.
 * Copyright (C) 2026  The JappeOS team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#define STORAGE_MOUNTPOINT_BOOT "/boot"
#define STORAGE_MOUNTPOINT_ROOT "/"

#define STORAGE_PART_BOOT_MIN_SIZE_MIB             100
#define STORAGE_PART_BOOT_MIN_RECOMMENDED_SIZE_MIB 512
#define STORAGE_PART_ROOT_MIN_SIZE_MIB             40960
#define STORAGE_PART_ROOT_MIN_RECOMMENDED_SIZE_MIB 40960 * 2

#define STORAGE_FILESYSTEM_FAT32   "fat32"
#define STORAGE_FILESYSTEM_EXT4    "ext4"
#define STORAGE_FILESYSTEM_BTRFS   "btrfs"
#define STORAGE_FILESYSTEM_XFS     "xfs"
#define STORAGE_FILESYSTEM_UNKNOWN "unknown"

#define INSTALLER_STATE_IDLE      "idle"
#define INSTALLER_STATE_RUNNING   "running"
#define INSTALLER_STATE_SUCCEEDED "succeeded"
#define INSTALLER_STATE_FAILED    "failed"
#define INSTALLER_STATE_CANCELLED "cancelled"