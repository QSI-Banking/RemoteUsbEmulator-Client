/*
 * Copyright (c) 2021-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "usbip.h"
#include "resource.h"

#include <libusbip/vhci.h>
#include <libusbip/output.h>
#include <libusbip/win_handle.h>
#include <libusbip/win_socket.h>
#include <libusbip/format_message.h>

#include <libusbip/src/usb_ids.h>
#include <libusbip/src/strconv.h>
#include <libusbip/src/file_ver.h>

#include <resources/messages.h>

#include "log.h"
#include "strings.h"

#include <CLI11/CLI11.hpp>
#include <memory>
#include <print>

namespace
{

using namespace usbip;

const auto MAX_HUB_PORTS = 255; // @see drivers/usbip_ude/vhci.cpp, set_usb_ports_cnt

constexpr auto &str_zero_copy = "zero-copy";
constexpr auto &str_low_latency = "low-latency";

auto get_ids_data()
{
        win::Resource r(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDR_USB_IDS), RT_RCDATA);
        assert(r);
        return r.str();
}

auto get_version()
{
        win::FileVersion fv;
        auto ver = fv.GetFileVersion();
        return wchar_to_utf8_or(ver);
}

template <typename Cmd, typename Args>
auto pack(Cmd &&cmd, std::shared_ptr<Args> args)
{
        return [cmd = std::forward<Cmd>(cmd), args = std::move(args)] {
                if (!cmd(*args)) {
                        throw CLI::RuntimeError(EXIT_FAILURE);
                }
        };
}

void add_cmd_attach(CLI::App &app)
{
        auto r = std::make_shared<attach_args>();

        /*
         * The server picks the device mapped to the user and is never retried automatically.
         */
        auto cmd = app.add_subcommand("attach", "Attach the USB device mapped to a user");
        auto rem = cmd;

        rem->add_option("-u,--user", r->user, "User name")
                ->required();

        rem->add_option("--otp", r->otp, "One-time password (prompted for if omitted)");

        rem->add_flag("-t,--terse", r->terse, "Show port number as a result");

        auto receive = [&mode = r->recv_mode, low_lat = str_low_latency] (const auto &choice)
        {
                mode = choice == low_lat ? receive_mode::low_latency : receive_mode::zero_copy;
        };

        rem->add_option_function<std::string>("--receive-mode", receive, "How to receive data from the network")
                ->check(CLI::IsMember({str_zero_copy, str_low_latency}))
                ->default_str(str_zero_copy);

        cmd->callback(pack(cmd_attach, r));
}

void add_cmd_detach(CLI::App &app)
{
        auto r = std::make_shared<detach_args>();

        auto cmd = app.add_subcommand("detach", "Detach a remote USB device")
                ->callback(pack(cmd_detach, r))
                ->require_option(1);

        auto opt_port = cmd->add_option("-p,--port", r->port, "Hub port number the device is plugged in")
                ->check(CLI::Range(1, MAX_HUB_PORTS));

        cmd->add_option("-a,--all", "Detach all devices")
                ->each([&port = r->port] (const auto&) noexcept { port = vhci::port_all_closeonly; }) // will not be called if argument is not passed
                ->check(CLI::IsMember({"closeonly"}))
                ->expected(0, 1)
                ->excludes(opt_port);
}

void add_cmd_port(CLI::App &app)
{
        auto r = std::make_shared<port_args>();

        auto cmd = app.add_subcommand("port", "Show imported USB devices")
                ->callback(pack(cmd_port, r));

        // No persistent devices: every attach needs a user and a new one-time password.

        cmd->add_option("number", r->ports, "Hub port number")
                ->check(CLI::Range(1, MAX_HUB_PORTS))
                ->expected(1, MAX_HUB_PORTS);
}

auto &msgtable_dll = L"resources"; // resource-only DLL that contains RT_MESSAGETABLE

auto& get_resource_module() noexcept
{
        static HModule mod(LoadLibraryEx(msgtable_dll, nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR));
        return mod;
}

void init(CLI::App &app)
{
        app.set_version_flag("-V,--version", get_version());

        app.add_flag("-d,--debug",
                [] (auto) { log::set_debug(true); }, "Debug output");

        add_cmd_attach(app);
        add_cmd_detach(app);
        add_cmd_port(app);

        app.require_subcommand(1);
}

auto run(int argc, wchar_t *argv[])
{
        log::init();
        libusbip::set_debug_output(log::debug_msg);

        if (!get_resource_module()) {
                auto err = GetLastError();
                log::critical("can't load '{}.dll', error {:#x} {}", "resources", err, format_message(err));
                return EXIT_FAILURE;
        }

        InitWinSock2 ws2;
        if (!ws2) {
                log::critical("can't initialize Windows Sockets 2, {}", get_last_error_msg());
                return EXIT_FAILURE;
        }

        CLI::App app("USB/IP client");
        init(app);

        try {
                app.parse(argc, argv);
        } catch (CLI::ParseError &e) {
                return app.exit(e);
        }

        return EXIT_SUCCESS;
}

} // namespace


std::string usbip::get_last_error_msg(DWORD msg_id)
{
        auto &mod = get_resource_module();
        return format_message(mod.get(), msg_id);
}

const UsbIds& usbip::get_ids()
{
        static UsbIds ids(get_ids_data());
        assert(ids);
        return ids;
}

int wmain(int argc, wchar_t *argv[])
{
        try {
                return run(argc, argv);
        } catch (const std::exception &e) {
                std::println(stderr, "exception: {}", e.what());
                return EXIT_FAILURE;
        }
}
