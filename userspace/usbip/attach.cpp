/*
 * Copyright (c) 2021-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "usbip.h"
#include "ctrl_c_guard.h"
#include "strings.h"
#include "log.h"

#include <libusbip/vhci.h>
#include <resources/messages.h>

#include <iostream>
#include <print>

namespace
{

using namespace usbip;

/*
 * Fixed endpoint of the local TLS tunnel. The driver enforces the same values (include/usbip/consts.h),
 * so these are informational only.
 */
constexpr auto &tunnel_host = "127.0.0.1";
constexpr auto &tunnel_port = "3240";
constexpr auto &auth_busid_placeholder = "auth"; // the server returns the real busid in OP_REP_AUTH

/*
 * Read the one-time password from the console without echo.
 */
auto read_otp()
{
        std::string otp;

        auto in = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode{};
        auto console = GetConsoleMode(in, &mode);

        if (console) {
                SetConsoleMode(in, mode & ~ENABLE_ECHO_INPUT);
        }

        std::print("One-time password: ");
        std::getline(std::cin, otp);

        if (console) {
                SetConsoleMode(in, mode);
                std::println();
        }

        return otp;
}

} // namespace


bool usbip::cmd_attach(const attach_args &args)
{
        auto dev = vhci::open();
        if (!dev) {
                log::error(get_last_error_msg());
                return false;
        }

        ctrl_c_guard guard(dev.get());

        vhci::attach_args cmd_args {
                .config = {
                        .location = {
                                .hostname = tunnel_host,
                                .service = tunnel_port,
                                .busid = auth_busid_placeholder,
                        },
                        .recv_mode = args.recv_mode,
                        .user = args.user,
                },
                .once = true, // Never reconnect automatically, a new attach needs a new one-time password
                .otp = args.otp.empty() ? read_otp() : args.otp,
        };

        auto port = vhci::attach(dev.get(), cmd_args);
        SecureZeroMemory(cmd_args.otp.data(), cmd_args.otp.size());

        if (!port) {
                auto err = GetLastError();
                if (err == USBIP_ERROR_ST_NODEV) {
                        log::error("no device is bound to user '{}'", args.user);
                } else {
                        log::error(get_last_error_msg(err));
                }
                return false;
        }

        if (args.terse) {
                std::println("{}", port);
                return true;
        }

        std::string busid;
        if (auto devices = vhci::get_imported_devices(dev.get())) {
                for (auto &d: *devices) {
                        if (d.port == port) {
                                busid = d.config.location.busid;
                                break;
                        }
                }
        }

        std::println("successfully attached busid '{}' for user '{}' to port {}", busid, args.user, port);
        return true;
}
