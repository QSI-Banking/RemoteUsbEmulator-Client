/*
 * Copyright (c) 2023-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

namespace usbip
{

inline constexpr auto& tcp_port = "3240";
inline constexpr auto& driver_filename = L"usbip2_ude"; // used by filter driver
inline constexpr auto& persistent_devices_value_name = L"PersistentDevices";

enum op_status_t // op_common.status
{
        ST_OK,
        ST_NA, // !SDEV_ST_AVAILABLE
        ST_DEV_BUSY, // SDEV_ST_USED
        ST_DEV_ERR, // SDEV_ST_ERROR
        ST_NODEV, // requested device not found by busid; for OP_REP_AUTH: no device is mapped to the user
        ST_ERROR, // ST_DEV_ERR?
        ST_AUTH_FAILED, // OP_REQ_AUTH rejected (unknown user or wrong one-time password)
};

/*
 * SERIAL_BUFSZ
 *      buffer length for USB_DEVICE_DESCRIPTOR.iSerialNumber
 *      string in characters, including the terminating '\0'.
 */
enum { 
        USBIP_VERSION = 0x111, // protocol
        DEV_PATH_MAX = 256, 
        BUS_ID_SIZE = 32,
        SERIAL_BUFSZ = 16,
        USER_NAME_BUFSZ = 64, // Including the terminating '\0'
        OTP_BUFSZ = 16, // Including the terminating '\0'
};

/*
 * Placeholder busid sent to the driver; the real busid comes from the server in OP_REP_AUTH.
 */
inline constexpr auto& auth_busid_placeholder = "auth";

} // namespace usbip
