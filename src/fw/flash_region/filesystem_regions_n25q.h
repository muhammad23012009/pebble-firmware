/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

//! @file filesystem_regions_n25q.h
//!
//! Flash regions used for the filesystem used by the serial flash chip that we're using in
//! Tintin/Bianca. Only included by core/flash_region/filesystem_regions.c

#define FILE_SYSTEM_REGIONS(MACRO_OPERATOR)                                               \
  MACRO_OPERATOR(FLASH_REGION_FILESYSTEM_BEGIN, FLASH_REGION_FILESYSTEM_END)              \
  MACRO_OPERATOR(FLASH_REGION_FILESYSTEM_2_BEGIN, FLASH_REGION_FILESYSTEM_2_END)          \
  MACRO_OPERATOR(FLASH_REGION_FILESYSTEM_3_BEGIN, FLASH_REGION_FILESYSTEM_3_END)          \
  MACRO_OPERATOR(FLASH_REGION_FILESYSTEM_4_BEGIN, FLASH_REGION_FILESYSTEM_4_END)          \
  MACRO_OPERATOR(FLASH_REGION_EXTRA_FILESYSTEM_BEGIN, FLASH_REGION_EXTRA_FILESYSTEM_END)  \
  MACRO_OPERATOR(FLASH_REGION_FILESYSTEM_5_BEGIN, FLASH_REGION_FILESYSTEM_5_END)

