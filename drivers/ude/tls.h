/*
 * Mutual TLS for the connection to usbipd-win.
 */

#pragma once

#include <libdrv/codeseg.h>
#include <libdrv/wdm_cpp.h>
#include <libdrv/wdf_cpp.h>

#include <UdeCxTypes.h>

/*
 * Mutual TLS between the driver and usbipd-win, terminated in the kernel with Schannel.
 *
 * Settings come from the admin-only key HKLM\SOFTWARE\USBip\Connection (see tls.cpp),
 * so an unprivileged caller of the VHCI device cannot choose where the driver connects.
 *
 * Schannel calls must be made at PASSIVE_LEVEL, but the driver's data path runs at DISPATCH_LEVEL.
 * Each attached device therefore has two system threads:
 * - receive thread: reads whole TLS records, decrypts them and feeds the plaintext into the same
 *   parser that WSK receive events use (ring buffer mode);
 * - send thread: takes device_ctx.pending_sends in order, encrypts and sends them, then runs the
 *   regular send completion.
 */

namespace usbip
{
struct device_ctx;
struct device_ctx_ext;
} // namespace usbip


namespace usbip::tls
{

struct session;

/*
 * Reads the server host and TCP port from HKLM\SOFTWARE\USBip\Connection.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS get_server_location(
        _Out_writes_z_(host_size) char *host, _In_ size_t host_size,
        _Out_writes_z_(service_size) char *service, _In_ size_t service_size);

/*
 * TLS handshake on the connected socket ext.sock. On success ext.tls is set.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS connect(_Inout_ device_ctx_ext &ext);

/*
 * Synchronous plaintext I/O for OP_REQ_AUTH / OP_REQ_IMPORT, before the device is plugged in.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS send(_Inout_ session &s, _In_reads_bytes_(len) const void *data, _In_ ULONG len);

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS recv(_Inout_ session &s, _Out_writes_bytes_(len) void *data, _In_ ULONG len);

/*
 * Replace events::start_receive_data / start_receive_data_irp for a TLS connection.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS start_receive_data(_In_ UDECXUSBDEVICE device);

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED wdm::object_reference stop_receive_data(_In_ UDECXUSBDEVICE device, _Inout_ bool &socket_closed);

/*
 * Releases the Schannel handles of a session whose device was never created.
 * Can be called several times.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void close(_Inout_ device_ctx_ext &ext);

/*
 * Called by send_pending instead of WskSend: wakes the send thread,
 * or fails the queued sends if the session is stopped.
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void queue_send(_Inout_ device_ctx &dev, _Inout_ session &s);

/*
 * Frees the memory of the session, Schannel handles must already be released.
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void free(_Inout_ session* &s);

} // namespace usbip::tls
