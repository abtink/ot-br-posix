/*
 *    Copyright (c) 2020, The OpenThread Authors.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions are met:
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *    3. Neither the name of the copyright holder nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *    POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * This file includes config file if defined.
 */
#ifndef OTBR_CONFIG_H_
#define OTBR_CONFIG_H_

#if defined(OTBR_CONFIG_FILE)
#include OTBR_CONFIG_FILE
#elif defined(OTBR_USER_CONFIG_HEADER_ENABLE) && OTBR_USER_CONFIG_HEADER_ENABLE
// This configuration header file should be provided by the user when
// OTBR_USER_CONFIG_HEADER_ENABLE is defined to 1.
#include "ot-br-posix-user-config.h"
#endif

/**
 * @def OTBR_ENABLE_SRP_SERVER
 *
 * Enable SRP server if Advertising Proxy is enabled.
 */
#define OTBR_ENABLE_SRP_SERVER (OTBR_ENABLE_SRP_ADVERTISING_PROXY || OTBR_ENABLE_OT_SRP_ADV_PROXY)

/**
 * @def OTBR_ENABLE_SRP_SERVER_AUTO_ENABLE_MODE
 *
 * By default, enable auto-enable mode for SRP server if SRP server and Border Routing are enabled.
 */
#ifndef OTBR_ENABLE_SRP_SERVER_AUTO_ENABLE_MODE
#if !OTBR_ENABLE_SRP_SERVER_ON_INIT
#define OTBR_ENABLE_SRP_SERVER_AUTO_ENABLE_MODE (OTBR_ENABLE_SRP_SERVER && OTBR_ENABLE_BORDER_ROUTING)
#endif
#endif

/**
 * @def OTBR_CONFIG_CLI_MAX_LINE_LENGTH
 *
 * Defines the maximum length of a line in the CLI.
 */
#ifndef OTBR_CONFIG_CLI_MAX_LINE_LENGTH
#define OTBR_CONFIG_CLI_MAX_LINE_LENGTH 640
#endif

#endif // OTBR_CONFIG_H_
