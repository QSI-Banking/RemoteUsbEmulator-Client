/*
 * Copyright (c) 2021-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#include <libusbip/vhci.h>
#include <libusbip/remote.h>

#include <set>
#include <string>

namespace usbip
{

class UsbIds;
const UsbIds& get_ids();

std::string get_last_error_msg(DWORD msg_id = GetLastError());

struct global_args
{
        std::string tcp_port = get_tcp_port();
};
inline global_args global_args;

/*
 * Attach takes only a user and a one-time password. The busid comes from the server, and the attach is never retried automatically.
 */
struct attach_args
{
        std::string user;
        std::string otp; // prompted for if empty
        bool terse{};
        receive_mode recv_mode = receive_mode::zero_copy;
};
bool cmd_attach(const attach_args &args);

struct detach_args
{
        int port = vhci::port_all;
};
bool cmd_detach(const detach_args &args);

struct port_args
{
        std::set<int> ports;
        bool persistent{};
};
bool cmd_port(const port_args &args);

} // namespace usbip
