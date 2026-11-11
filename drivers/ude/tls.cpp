/*
 * Mutual TLS for the connection to usbipd-win, see tls.h.
 *
 * Settings, the driver's Parameters key HKLM\SYSTEM\CurrentControlSet\Services\usbip2_ude\Parameters
 * (writable by administrators only, written by the client installer):
 *   Host                        REG_SZ     server name or IP address, required
 *   Port                        REG_DWORD  TLS port of usbipd-win, default 3241
 *   ServerName                  REG_SZ     name that must be in the server certificate (SAN/CN), required
 *   ClientCertificateThumbprint REG_SZ     SHA-1 thumbprint of the client certificate in LocalMachine\My, required
 *   ServerCertificateSha256     REG_SZ     optional pin, SHA-256 of the server certificate (DER)
 *
 * The server certificate must chain to a root in LocalMachine\Root, Schannel checks it and ServerName.
 */

#include <ntifs.h> // kernel-mode SSPI, must precede wdm.h

#include "tls.h"
#include "trace.h"
#include "tls.tmh"

#include "context.h"
#include "device.h"
#include "device_ioctl.h"
#include "network.h"
#include "persistent.h"
#include "ring_buffer.h"
#include "wsk_context.h"
#include "wsk_receive_events.h"

#include <libdrv/lists.h>
#include <libdrv/strconv.h>

#include <ntstrsafe.h>

namespace
{

using namespace usbip;
using namespace libdrv;

constexpr ULONG pooltag = 'SLTU';

/*
 * SECURITY_STATUS values, winerror.h is not available in kernel mode.
 */
constexpr SECURITY_STATUS sec_e_ok = 0;
constexpr SECURITY_STATUS sec_i_continue_needed = 0x00090312L;
constexpr SECURITY_STATUS sec_i_context_expired = 0x00090317L;
constexpr SECURITY_STATUS sec_i_incomplete_credentials = 0x00090320L;
constexpr SECURITY_STATUS sec_i_renegotiate = 0x00090321L;
constexpr SECURITY_STATUS sec_e_incomplete_message = static_cast<SECURITY_STATUS>(0x80090318L);

/*
 * schannel.h is user-mode only, these mirror its definitions.
 */
constexpr ULONG sch_credentials_version = 5;
constexpr ULONG sch_cred_format_cert_hash_store = 2;
constexpr ULONG sch_machine_cert_hash = 1;
constexpr ULONG sch_cred_no_default_creds = 0x00000010;
constexpr ULONG sch_cred_auto_cred_validation = 0x00000020;
constexpr ULONG sch_use_strong_crypto = 0x00400000;

constexpr ULONG sp_prot_ssl2_client = 0x00000008;
constexpr ULONG sp_prot_ssl3_client = 0x00000020;
constexpr ULONG sp_prot_tls1_0_client = 0x00000080;
constexpr ULONG sp_prot_tls1_1_client = 0x00000200;

struct schannel_cert_hash_store // SCHANNEL_CERT_HASH_STORE
{
        ULONG dwLength;
        ULONG dwFlags;
        ULONG_PTR hProv;
        UCHAR ShaHash[20];
        WCHAR pwszStoreName[128];
};

struct tls_parameters // TLS_PARAMETERS
{
        ULONG cAlpnIds;
        void *rgstrAlpnIds;
        ULONG grbitDisabledProtocols;
        ULONG cDisabledCrypto;
        void *pDisabledCrypto;
        ULONG dwFlags;
};

struct sch_credentials // SCH_CREDENTIALS
{
        ULONG dwVersion;
        ULONG dwCredFormat;
        ULONG cCreds;
        void *paCred;
        void *hRootStore;
        ULONG cMappers;
        void *aphMappers;
        ULONG dwSessionLifespan;
        ULONG dwFlags;
        ULONG cTlsParameters;
        tls_parameters *pTlsParameters;
};

/*
 * TLS record: type(1), version(2), length(2), body.
 * TLSCiphertext.length is at most 2^14 + 2048 (TLS 1.2), 2^14 + 256 (TLS 1.3).
 */
constexpr ULONG record_header_size = 5;
constexpr ULONG max_record_body = 16384 + 2048;
constexpr ULONG max_record_size = record_header_size + max_record_body;
constexpr ULONG in_capacity = 2*max_record_size; // handshake leftovers + one record

constexpr ULONG thumbprint_size = 20; // SHA-1
constexpr ULONG pin_size = 32; // SHA-256

constexpr auto default_port = 3241UL;

// for messages only, the key is opened with DriverRegKeyParameters
const UNICODE_STRING settings_key = RTL_CONSTANT_STRING(L"HKLM\\SYSTEM\\CurrentControlSet\\Services\\usbip2_ude\\Parameters");

class event_lock
{
public:
        _IRQL_requires_(PASSIVE_LEVEL)
        explicit event_lock(_Inout_ KEVENT &e) : m_event(e)
        {
                KeWaitForSingleObject(&m_event, Executive, KernelMode, false, nullptr);
        }

        ~event_lock() { KeSetEvent(&m_event, IO_NO_INCREMENT, false); }

        event_lock(const event_lock&) = delete;
        event_lock& operator=(const event_lock&) = delete;
private:
        KEVENT &m_event;
};

struct sec_pkg_context_bindings // SecPkgContext_Bindings, not in ntifs.h
{
        unsigned long BindingsLength;
        SEC_CHANNEL_BINDINGS *Bindings;
};

/*
 * ksecdd.lib exports EncryptMessage/DecryptMessage as SealMessage/UnsealMessage,
 * the function table gives them by their documented names.
 */
PSecurityFunctionTableW g_sspi;

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto init_sspi()
{
        PAGED_CODE();

        if (!g_sspi) {
                auto t = InitSecurityInterfaceW();
                if (!(t && t->EncryptMessage && t->DecryptMessage)) {
                        Trace(TRACE_LEVEL_ERROR, "InitSecurityInterfaceW failed");
                        return STATUS_NOT_SUPPORTED;
                }
                InterlockedExchangePointer(reinterpret_cast<PVOID*>(&g_sspi), t);
        }

        return STATUS_SUCCESS;
}

struct settings
{
        UNICODE_STRING server_name;
        WCHAR server_name_buf[256];
        UCHAR client_thumbprint[thumbprint_size];
        UCHAR server_pin[pin_size];
        bool has_pin;
};

} // namespace


struct usbip::tls::session
{
        device_ctx_ext *ext; // ext->sock

        CredHandle cred;
        CtxtHandle ctx;
        bool has_cred;
        bool has_ctx;

        UNICODE_STRING target; // ServerName, Buffer is target_buf
        WCHAR target_buf[256];

        SecPkgContext_StreamSizes sizes;

        KEVENT crypto_lock; // serializes Schannel calls on ctx
        KEVENT write_lock; // keeps the TLS records in order on the wire

        UCHAR *in; // ciphertext, can hold more than one record after the handshake
        ULONG in_len;

        UCHAR *plain; // decrypted data not yet consumed by tls::recv
        ULONG plain_pos;
        ULONG plain_len;

        UCHAR *out; // cbHeader + cbMaximumMessage + cbTrailer
        ULONG out_used; // plaintext bytes of the record being built

        _KTHREAD *recv_thread;
        _KTHREAD *send_thread;
        KEVENT send_wakeup;

        LONG stopped;
        LONG refs; // the owner (device_ctx_ext) + running threads; the last one releases Schannel handles
        LONG owner_released;
};


namespace
{

using tls::session;

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
inline auto is_stopped(_In_ session &s)
{
        return InterlockedCompareExchange(&s.stopped, 0, 0) != 0;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void free_pool(_Inout_ UCHAR* &p, _In_ ULONG secure_len = 0)
{
        if (p) {
                if (secure_len) {
                        RtlSecureZeroMemory(p, secure_len);
                }
                ExFreePoolWithTag(p, pooltag);
                p = nullptr;
        }
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void free_buffers(_Inout_ session &s)
{
        free_pool(s.in, in_capacity);
        free_pool(s.plain, max_record_body);

        auto out_size = s.sizes.cbHeader + s.sizes.cbMaximumMessage + s.sizes.cbTrailer;
        free_pool(s.out, out_size);
}

/*
 * Called by the owner and by each thread on exit. The last caller releases Schannel handles.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void release_handles(_Inout_ session &s)
{
        PAGED_CODE();

        if (InterlockedDecrement(&s.refs)) {
                return;
        }

        if (s.has_ctx) {
                DeleteSecurityContext(&s.ctx);
                s.has_ctx = false;
        }

        if (s.has_cred) {
                FreeCredentialsHandle(&s.cred);
                s.has_cred = false;
        }

        free_buffers(s);
        TraceDbg("session %04x, Schannel handles released", ptr04x(&s));
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void release_owner(_Inout_ session &s)
{
        PAGED_CODE();

        if (!InterlockedExchange(&s.owner_released, true)) {
                release_handles(s);
        }
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto alloc_session(_In_ device_ctx_ext &ext)
{
        PAGED_CODE();

        auto s = static_cast<session*>(ExAllocatePoolZero(NonPagedPoolNx, sizeof(session), pooltag));
        if (!s) {
                return s;
        }

        s->ext = &ext;
        s->refs = 1; // owner

        SecInvalidateHandle(&s->cred);
        SecInvalidateHandle(&s->ctx);

        KeInitializeEvent(&s->crypto_lock, SynchronizationEvent, true);
        KeInitializeEvent(&s->write_lock, SynchronizationEvent, true);
        KeInitializeEvent(&s->send_wakeup, SynchronizationEvent, false);

        s->in = static_cast<UCHAR*>(ExAllocatePoolZero(NonPagedPoolNx, in_capacity, pooltag));
        s->plain = static_cast<UCHAR*>(ExAllocatePoolZero(NonPagedPoolNx, max_record_body, pooltag));

        if (!(s->in && s->plain)) {
                free_buffers(*s);
                ExFreePoolWithTag(s, pooltag);
                s = nullptr;
        }

        return s;
}

/*
 * Accepts upper/lower case hex digits separated by spaces or colons, as certutil and the
 * certificate dialog print them (the dialog also adds invisible U+200E).
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto parse_hex(_In_ const UNICODE_STRING &str, _Out_writes_bytes_(size) UCHAR *out, _In_ ULONG size)
{
        PAGED_CODE();

        ULONG n = 0;
        int hi = -1;

        for (USHORT i = 0; i < str.Length/sizeof(*str.Buffer); ++i) {

                auto c = str.Buffer[i];
                if (c == L' ' || c == L'\t' || c == L':' || c == L'\0' || c == 0x200E || c == 0x200F || c == 0xFEFF) {
                        continue;
                }

                int v = c >= L'0' && c <= L'9' ? c - L'0' :
                        c >= L'a' && c <= L'f' ? c - L'a' + 10 :
                        c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;

                if (v < 0) {
                        return false;
                } else if (hi < 0) {
                        hi = v;
                } else if (n == size) {
                        return false;
                } else {
                        out[n++] = static_cast<UCHAR>(hi << 4 | v);
                        hi = -1;
                }
        }

        return hi < 0 && n == size;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto open_settings(_Inout_ registry &key)
{
        PAGED_CODE();

        auto st = usbip::open(key, DriverRegKeyParameters);
        if (NT_ERROR(st)) {
                Trace(TRACE_LEVEL_ERROR, "Cannot open %!USTR!, %!STATUS!. Run the client installer again.", &settings_key, st);
        }
        return st;
}

/*
 * @param value Buffer and MaximumLength must be set
 * @return STATUS_OBJECT_NAME_NOT_FOUND if the value does not exist
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS query_string(_In_ WDFKEY key, _In_ PCWSTR value_name, _Inout_ UNICODE_STRING &value)
{
        PAGED_CODE();

        UNICODE_STRING name;
        RtlInitUnicodeString(&name, value_name);

        value.Length = 0;
        USHORT len{};

        auto st = WdfRegistryQueryUnicodeString(key, &name, &len, &value);
        if (!NT_SUCCESS(st)) { // STATUS_BUFFER_OVERFLOW is a warning
                value.Length = 0;
                return NT_ERROR(st) ? st : STATUS_BUFFER_TOO_SMALL;
        }

        // drop terminating nulls and surrounding blanks
        auto &s = value;
        while (s.Length && (s.Buffer[s.Length/sizeof(WCHAR) - 1] == L'\0' || s.Buffer[s.Length/sizeof(WCHAR) - 1] == L' ')) {
                s.Length -= sizeof(WCHAR);
        }
        while (s.Length && *s.Buffer == L' ') {
                RtlMoveMemory(s.Buffer, s.Buffer + 1, s.Length -= sizeof(WCHAR));
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS read_settings(_Out_ settings &cfg)
{
        PAGED_CODE();
        RtlZeroMemory(&cfg, sizeof(cfg));

        registry key;
        if (auto st = open_settings(key); NT_ERROR(st)) {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        auto st = STATUS_SUCCESS;

        cfg.server_name.Buffer = cfg.server_name_buf;
        cfg.server_name.MaximumLength = sizeof(cfg.server_name_buf) - sizeof(WCHAR);

        if (NT_ERROR(query_string(key.get(), L"ServerName", cfg.server_name)) || !cfg.server_name.Length) {
                Trace(TRACE_LEVEL_ERROR, "%!USTR!\\ServerName is required", &settings_key);
                st = STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        WCHAR buf[128];
        UNICODE_STRING str{ .MaximumLength = sizeof(buf), .Buffer = buf };

        if (NT_SUCCESS(st)) {
                if (NT_ERROR(query_string(key.get(), L"ClientCertificateThumbprint", str)) ||
                    !parse_hex(str, cfg.client_thumbprint, sizeof(cfg.client_thumbprint))) {
                        Trace(TRACE_LEVEL_ERROR, "%!USTR!\\ClientCertificateThumbprint must be 40 hex digits", &settings_key);
                        st = STATUS_DEVICE_CONFIGURATION_ERROR;
                }
        }

        if (NT_SUCCESS(st)) {
                if (NT_ERROR(query_string(key.get(), L"ServerCertificateSha256", str)) || !str.Length) {
                        cfg.has_pin = false; // optional
                } else if (parse_hex(str, cfg.server_pin, sizeof(cfg.server_pin))) {
                        cfg.has_pin = true;
                } else {
                        Trace(TRACE_LEVEL_ERROR, "%!USTR!\\ServerCertificateSha256 must be 64 hex digits", &settings_key);
                        st = STATUS_DEVICE_CONFIGURATION_ERROR;
                }
        }

        return st;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS acquire_credentials(_Inout_ session &s, _In_ const settings &cfg)
{
        PAGED_CODE();

        schannel_cert_hash_store hash { 
                .dwLength = sizeof(hash), 
                .dwFlags = sch_machine_cert_hash, // LocalMachine
        };
        RtlCopyMemory(hash.ShaHash, cfg.client_thumbprint, sizeof(hash.ShaHash));
        NT_VERIFY(NT_SUCCESS(RtlStringCchCopyW(hash.pwszStoreName, ARRAYSIZE(hash.pwszStoreName), L"MY")));

        tls_parameters params { // TLS 1.2 and 1.3 only
                .grbitDisabledProtocols = sp_prot_ssl2_client | sp_prot_ssl3_client | 
                                          sp_prot_tls1_0_client | sp_prot_tls1_1_client
        };

        sch_credentials cred {
                .dwVersion = sch_credentials_version,
                .dwCredFormat = sch_cred_format_cert_hash_store,
                .cCreds = 1,
                .paCred = &hash,
                .dwFlags = sch_cred_no_default_creds | sch_cred_auto_cred_validation | sch_use_strong_crypto,
                .cTlsParameters = 1,
                .pTlsParameters = &params,
        };

        WCHAR package_name[] = L"Microsoft Unified Security Protocol Provider"; // UNISP_NAME_W
        UNICODE_STRING package;
        RtlInitUnicodeString(&package, package_name);

        auto ss = AcquireCredentialsHandleW(nullptr, &package, SECPKG_CRED_OUTBOUND, nullptr, &cred, 
                                            nullptr, nullptr, &s.cred, nullptr);

        if (ss != sec_e_ok) {
                Trace(TRACE_LEVEL_ERROR, "AcquireCredentialsHandle 0x%08lX. Is the client certificate (ClientCertificateThumbprint) in LocalMachine\\My, with a private key?", ss);
                return STATUS_NO_SUCH_LOGON_SESSION;
        }

        s.has_cred = true;
        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto send_raw(_In_ session &s, _In_ memory pool, _In_reads_bytes_(len) void *data, _In_ ULONG len)
{
        PAGED_CODE();

        auto st = usbip::send(s.ext->sock, pool, data, len);
        if (NT_ERROR(st)) {
                Trace(TRACE_LEVEL_ERROR, "send %lu bytes %!STATUS!", len, st);
        }
        return st;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
inline ULONG record_size(_In_reads_bytes_(record_header_size) const UCHAR *p)
{
        auto type = p[0];
        if (!(type >= 20 && type <= 23) || p[1] != 3) { // change_cipher_spec .. application_data, TLS 1.x
                return 0;
        }

        ULONG len = p[3] << 8 | p[4];
        return len && len <= max_record_body ? record_header_size + len : 0;
}

/*
 * Makes s.in contain the complete record that starts at s.in + offset.
 * Reads exactly what is missing, so s.in never holds bytes past that record.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS read_record(_Inout_ session &s, _In_ ULONG offset, _Out_ ULONG &size)
{
        PAGED_CODE();
        size = 0;

        if (offset + max_record_size > in_capacity) {
                Trace(TRACE_LEVEL_ERROR, "TLS receive buffer overflow, offset %lu", offset);
                return STATUS_BUFFER_OVERFLOW;
        }

        if (auto end = offset + record_header_size; s.in_len < end) {
                auto st = usbip::recv(s.ext->sock, memory::nonpaged, s.in + s.in_len, end - s.in_len);
                if (NT_ERROR(st)) {
                        TraceDbg("recv %!STATUS!", st);
                        return st;
                }
                s.in_len = end;
        }

        size = record_size(s.in + offset);
        if (!size) {
                auto p = s.in + offset;
                Trace(TRACE_LEVEL_ERROR, "Not a TLS record: %02x %02x %02x %02x %02x. Is Port the TLS port of usbipd?",
                                          p[0], p[1], p[2], p[3], p[4]);
                return STATUS_INVALID_NETWORK_RESPONSE;
        }

        if (auto end = offset + size; s.in_len < end) {
                auto st = usbip::recv(s.ext->sock, memory::nonpaged, s.in + s.in_len, end - s.in_len);
                if (NT_ERROR(st)) {
                        TraceDbg("recv %!STATUS!", st);
                        return st;
                }
                s.in_len = end;
        }

        return STATUS_SUCCESS;
}

/*
 * @return offset of the first byte that does not belong to a complete record
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
auto complete_records(_In_ const session &s)
{
        ULONG off = 0;

        while (off + record_header_size <= s.in_len) {
                auto sz = record_size(s.in + off);
                if (!sz || off + sz > s.in_len) {
                        break;
                }
                off += sz;
        }

        return off;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
inline void consume(_Inout_ session &s, _In_ ULONG n)
{
        NT_ASSERT(n <= s.in_len);
        RtlMoveMemory(s.in, s.in + n, s.in_len - n);
        s.in_len -= n;
}

/*
 * Keeps only SECBUFFER_EXTRA (the tail of the input) in s.in.
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void keep_extra(_Inout_ session &s, _In_ const SecBuffer &extra)
{
        if (extra.BufferType == SECBUFFER_EXTRA && extra.cbBuffer && extra.cbBuffer <= s.in_len) {
                consume(s, s.in_len - extra.cbBuffer);
        } else {
                s.in_len = 0;
        }
}

/*
 * Sends the output token of InitializeSecurityContext and frees it.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS send_token(_In_ session &s, _Inout_ SecBuffer &token)
{
        PAGED_CODE();

        auto st = STATUS_SUCCESS;

        if (token.pvBuffer) {
                if (token.cbBuffer) {
                        st = send_raw(s, memory::paged, token.pvBuffer, token.cbBuffer);
                }
                FreeContextBuffer(token.pvBuffer);
                token.pvBuffer = nullptr;
        }

        return st;
}

constexpr ULONG isc_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                            ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_USE_SUPPLIED_CREDS | 
                            ISC_REQ_EXTENDED_ERROR;

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS handshake(_Inout_ session &s)
{
        PAGED_CODE();
        NT_ASSERT(!s.in_len);

        for (int round = 0; round < 64; ++round) {

                bool first = !round;

                SecBuffer in_bufs[] { 
                        { s.in_len, SECBUFFER_TOKEN, s.in }, 
                        { 0, SECBUFFER_EMPTY, nullptr } 
                };
                SecBufferDesc in_desc{ SECBUFFER_VERSION, ARRAYSIZE(in_bufs), in_bufs };

                SecBuffer out_buf{ 0, SECBUFFER_TOKEN, nullptr };
                SecBufferDesc out_desc{ SECBUFFER_VERSION, 1, &out_buf };

                ULONG attr{};
                SECURITY_STATUS ss;
                {
                        event_lock lck(s.crypto_lock);
                        ss = InitializeSecurityContextW(&s.cred, first ? nullptr : &s.ctx, &s.target, isc_flags, 0, 0,
                                                        first ? nullptr : &in_desc, 0, &s.ctx, 
                                                        &out_desc, &attr, nullptr);
                }

                if (first) {
                        s.has_ctx = ss >= 0;
                }

                auto st = send_token(s, out_buf); // can be an alert if ss is an error
                if (ss >= 0 && NT_ERROR(st)) {
                        return st;
                }

                if (ss == sec_e_ok) {
                        if (first) {
                                s.in_len = 0;
                        } else {
                                keep_extra(s, in_bufs[1]); // the first post-handshake records
                        }
                        return STATUS_SUCCESS;
                } else if (ss == sec_i_incomplete_credentials) {
                        Trace(TRACE_LEVEL_ERROR, "The server asked for a client certificate, but Schannel cannot use ClientCertificateThumbprint. It must be in LocalMachine\\My, have a private key and the Client Authentication usage.");
                        return STATUS_NO_SUCH_LOGON_SESSION;
                } else if (ss == sec_i_continue_needed) {
                        if (!first) {
                                keep_extra(s, in_bufs[1]);
                        }
                } else if (ss != sec_e_incomplete_message) {
                        Trace(TRACE_LEVEL_ERROR, "TLS handshake with '%!USTR!' failed, InitializeSecurityContext 0x%08lX. Check that the server certificate chains to LocalMachine\\Root and contains ServerName, and that the server trusts the client certificate.", &s.target, ss);
                        return STATUS_TRUST_FAILURE;
                }

                if (ss == sec_e_incomplete_message || complete_records(s) < s.in_len || !s.in_len) {
                        ULONG size;
                        st = read_record(s, complete_records(s), size);
                        if (NT_ERROR(st)) {
                                Trace(TRACE_LEVEL_ERROR, "TLS handshake: receive %!STATUS!. Did the server reject the client certificate? See the usbipd log.", st);
                                return st;
                        }
                }
        }

        Trace(TRACE_LEVEL_ERROR, "TLS handshake does not complete");
        return STATUS_TRUST_FAILURE;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS check_pin(_In_ session &s, _In_ const settings &cfg)
{
        PAGED_CODE();

        if (!cfg.has_pin) {
                return STATUS_SUCCESS;
        }

        sec_pkg_context_bindings b{};
        auto ss = QueryContextAttributesW(&s.ctx, SECPKG_ATTR_ENDPOINT_BINDINGS, &b);
        if (ss != sec_e_ok) {
                Trace(TRACE_LEVEL_ERROR, "QueryContextAttributes(SECPKG_ATTR_ENDPOINT_BINDINGS) 0x%08lX", ss);
                return STATUS_TRUST_FAILURE;
        }

        // RFC 5929 tls-server-end-point: hash of the server certificate, SHA-256 for SHA-256 or weaker signatures
        static const char prefix[] = "tls-server-end-point:";
        constexpr ULONG prefix_len = sizeof(prefix) - 1;

        bool match{};

        if (auto cb = b.Bindings; cb && b.BindingsLength >= sizeof(*cb) && 
            cb->dwApplicationDataOffset <= b.BindingsLength &&
            cb->cbApplicationDataLength <= b.BindingsLength - cb->dwApplicationDataOffset) {

                auto data = reinterpret_cast<const UCHAR*>(cb) + cb->dwApplicationDataOffset;

                match = cb->cbApplicationDataLength == prefix_len + pin_size &&
                        RtlEqualMemory(data, prefix, prefix_len) &&
                        RtlEqualMemory(data + prefix_len, cfg.server_pin, pin_size);
        }

        if (b.Bindings) {
                FreeContextBuffer(b.Bindings);
        }

        if (!match) {
                Trace(TRACE_LEVEL_ERROR, "The server certificate does not match ServerCertificateSha256");
                return STATUS_TRUST_FAILURE;
        }

        TraceDbg("server certificate matches ServerCertificateSha256");
        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS after_handshake(_Inout_ session &s, _In_ const settings &cfg)
{
        PAGED_CODE();

        auto st = check_pin(s, cfg);
        if (NT_ERROR(st)) {
                return st;
        }

        auto &sz = s.sizes;
        auto ss = QueryContextAttributesW(&s.ctx, SECPKG_ATTR_STREAM_SIZES, &sz);

        if (ss != sec_e_ok) {
                Trace(TRACE_LEVEL_ERROR, "QueryContextAttributes(SECPKG_ATTR_STREAM_SIZES) 0x%08lX", ss);
                RtlZeroMemory(&sz, sizeof(sz));
                return STATUS_TRUST_FAILURE;
        }

        if (!sz.cbMaximumMessage || sz.cbMaximumMessage > max_record_body || 
            sz.cbHeader > 1024 || sz.cbTrailer > 1024) {
                Trace(TRACE_LEVEL_ERROR, "Unexpected stream sizes: header %lu, max message %lu, trailer %lu",
                                          sz.cbHeader, sz.cbMaximumMessage, sz.cbTrailer);
                RtlZeroMemory(&sz, sizeof(sz));
                return STATUS_TRUST_FAILURE;
        }

        auto out_size = sz.cbHeader + sz.cbMaximumMessage + sz.cbTrailer;
        s.out = static_cast<UCHAR*>(ExAllocatePoolZero(NonPagedPoolNx, out_size, pooltag));

        if (!s.out) {
                RtlZeroMemory(&sz, sizeof(sz));
                return STATUS_INSUFFICIENT_RESOURCES;
        }

        Trace(TRACE_LEVEL_INFORMATION, "TLS session with '%!USTR!' established", &s.target);
        return STATUS_SUCCESS;
}

/*
 * Encrypts the plaintext accumulated in s.out and sends it. The caller holds s.write_lock.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS flush(_Inout_ session &s)
{
        PAGED_CODE();

        if (!s.out_used) {
                return STATUS_SUCCESS;
        }

        auto hdr = s.sizes.cbHeader;

        SecBuffer b[] {
                { hdr, SECBUFFER_STREAM_HEADER, s.out },
                { s.out_used, SECBUFFER_DATA, s.out + hdr },
                { s.sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, s.out + hdr + s.out_used },
                { 0, SECBUFFER_EMPTY, nullptr },
        };
        SecBufferDesc desc{ SECBUFFER_VERSION, ARRAYSIZE(b), b };

        SECURITY_STATUS ss;
        {
                event_lock lck(s.crypto_lock);
                ss = g_sspi->EncryptMessage(&s.ctx, 0, &desc, 0);
        }

        s.out_used = 0;

        if (ss != sec_e_ok) {
                Trace(TRACE_LEVEL_ERROR, "EncryptMessage 0x%08lX", ss);
                return STATUS_ENCRYPTION_FAILED;
        }

        auto len = b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer; // contiguous
        return send_raw(s, memory::nonpaged, s.out, len);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS append(_Inout_ session &s, _In_reads_bytes_(len) const void *data, _In_ ULONG len)
{
        PAGED_CODE();

        auto max = s.sizes.cbMaximumMessage;
        auto p = static_cast<const UCHAR*>(data);

        while (len) {
                auto n = min(len, max - s.out_used);
                RtlCopyMemory(s.out + s.sizes.cbHeader + s.out_used, p, n);

                s.out_used += n;
                p += n;
                len -= n;

                if (s.out_used == max) {
                        if (auto st = flush(s); NT_ERROR(st)) {
                                return st;
                        }
                }
        }

        return STATUS_SUCCESS;
}

/*
 * Copies the data described by an MDL chain, see prepare_wsk_buf in device_ioctl.cpp.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS append(_Inout_ session &s, _In_ const WSK_BUF &buf)
{
        PAGED_CODE();

        auto offset = buf.Offset;
        auto remaining = buf.Length;

        for (auto mdl = buf.Mdl; mdl && remaining; mdl = mdl->Next, offset = 0) {

                ULONG cnt = MmGetMdlByteCount(mdl);
                if (offset >= cnt) {
                        offset -= cnt;
                        continue;
                }

                const ULONG priority = NormalPagePriority | MdlMappingNoExecute;

                auto va = static_cast<const UCHAR*>(MmGetSystemAddressForMdlSafe(mdl, priority));
                if (!va) {
                        Trace(TRACE_LEVEL_ERROR, "MmGetSystemAddressForMdlSafe error");
                        return STATUS_INSUFFICIENT_RESOURCES;
                }

                ULONG n = static_cast<ULONG>(min(cnt - offset, remaining));

                if (auto st = append(s, va + offset, n); NT_ERROR(st)) {
                        return st;
                }

                remaining -= n;
        }

        if (remaining) {
                Trace(TRACE_LEVEL_ERROR, "MDL chain is %Iu bytes shorter than WSK_BUF.Length", remaining);
                return STATUS_INVALID_PARAMETER;
        }

        return STATUS_SUCCESS;
}

/*
 * TLS 1.3 post-handshake messages (NewSessionTicket, KeyUpdate): Schannel returns them
 * in SECBUFFER_EXTRA and they must be passed to InitializeSecurityContext.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS post_handshake(_Inout_ session &s, _In_opt_ const SecBuffer *extra)
{
        PAGED_CODE();

        SecBuffer in_bufs[] {
                { extra ? extra->cbBuffer : 0, SECBUFFER_TOKEN, extra ? extra->pvBuffer : nullptr },
                { 0, SECBUFFER_EMPTY, nullptr }
        };
        SecBufferDesc in_desc{ SECBUFFER_VERSION, ARRAYSIZE(in_bufs), in_bufs };

        SecBuffer out_buf{ 0, SECBUFFER_TOKEN, nullptr };
        SecBufferDesc out_desc{ SECBUFFER_VERSION, 1, &out_buf };

        ULONG attr{};
        SECURITY_STATUS ss;
        {
                event_lock lck(s.crypto_lock);
                ss = InitializeSecurityContextW(&s.cred, &s.ctx, &s.target, isc_flags, 0, 0, &in_desc, 0, &s.ctx, 
                                                &out_desc, &attr, nullptr);
        }

        NTSTATUS st;
        {
                event_lock lck(s.write_lock);
                st = send_token(s, out_buf);
        }

        if (ss != sec_e_ok) {
                Trace(TRACE_LEVEL_ERROR, "Post-handshake message, InitializeSecurityContext 0x%08lX. TLS renegotiation is not supported.", ss);
                return STATUS_TRUST_FAILURE;
        }

        return st;
}

/*
 * Decrypts the record s.in[0, size) in place.
 * @param data, len plaintext inside s.in, len can be zero; valid until consume(s, size)
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS decrypt(_Inout_ session &s, _In_ ULONG size, _Out_ UCHAR* &data, _Out_ ULONG &len)
{
        PAGED_CODE();

        data = nullptr;
        len = 0;

        SecBuffer b[] {
                { size, SECBUFFER_DATA, s.in },
                { 0, SECBUFFER_EMPTY, nullptr },
                { 0, SECBUFFER_EMPTY, nullptr },
                { 0, SECBUFFER_EMPTY, nullptr },
        };
        SecBufferDesc desc{ SECBUFFER_VERSION, ARRAYSIZE(b), b };

        SECURITY_STATUS ss;
        {
                event_lock lck(s.crypto_lock);
                ss = g_sspi->DecryptMessage(&s.ctx, &desc, 0, nullptr);
        }

        const SecBuffer *extra{};

        for (auto &i: b) {
                if (i.BufferType == SECBUFFER_DATA) {
                        data = static_cast<UCHAR*>(i.pvBuffer);
                        len = i.cbBuffer;
                } else if (i.BufferType == SECBUFFER_EXTRA && i.cbBuffer) {
                        extra = &i;
                }
        }

        if (ss == sec_e_ok) {
                if (extra) [[unlikely]] {
                        Trace(TRACE_LEVEL_ERROR, "DecryptMessage left %lu bytes of a single record", extra->cbBuffer);
                        return STATUS_INVALID_NETWORK_RESPONSE;
                }
                return STATUS_SUCCESS;
        } else if (ss == sec_i_renegotiate) {
                len = 0;
                return post_handshake(s, extra);
        } else if (ss == sec_i_context_expired) {
                Trace(TRACE_LEVEL_INFORMATION, "The server closed the TLS session");
                return STATUS_CONNECTION_DISCONNECTED;
        }

        Trace(TRACE_LEVEL_ERROR, "DecryptMessage 0x%08lX", ss);
        return STATUS_DECRYPTION_FAILED;
}

/*
 * Completes the queued sends with an error, see send_complete in device_ioctl.cpp.
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void fail_pending_sends(_Inout_ device_ctx &dev, _In_ NTSTATUS status)
{
        for (auto entry = reverse(InterlockedFlushSList(&dev.pending_sends)); entry; ) {

                auto &ctx = *CONTAINING_RECORD(entry, wsk_context, entry);

                entry = entry->Next;
                ctx.entry.Next = nullptr;

                ctx.wsk_buf = WSK_BUF{};
                device::complete_send(ctx, status);
        }
}

/*
 * Packs the queued sends into as few TLS records as possible, completes them after the last record is sent.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS send_pending_records(_Inout_ device_ctx &dev, _Inout_ session &s)
{
        PAGED_CODE();
        auto st = STATUS_SUCCESS;

        while (auto entry = reverse(InterlockedFlushSList(&dev.pending_sends))) {

                SLIST_ENTRY *done{};
                auto tail = &done;
                {
                        event_lock lck(s.write_lock);

                        while (entry) {
                                auto &ctx = *CONTAINING_RECORD(entry, wsk_context, entry);

                                auto next = entry->Next;
                                entry->Next = nullptr;

                                auto buf = ctx.wsk_buf;
                                ctx.wsk_buf = WSK_BUF{};

                                if (NT_SUCCESS(st)) {
                                        st = append(s, buf);
                                }

                                *tail = entry;
                                tail = &entry->Next;

                                entry = next;
                        }

                        if (NT_SUCCESS(st)) {
                                st = flush(s);
                        }
                }

                for (entry = done; entry; ) {
                        auto &ctx = *CONTAINING_RECORD(entry, wsk_context, entry);

                        entry = entry->Next;
                        ctx.entry.Next = nullptr;

                        device::complete_send(ctx, st);
                }

                if (NT_ERROR(st)) {
                        break;
                }
        }

        return st;
}

_Function_class_(KSTART_ROUTINE)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void send_thread_function(_In_ void *context)
{
        PAGED_CODE();

        auto device = static_cast<UDECXUSBDEVICE>(context);
        auto &dev = *get_device_ctx(device);

        auto mem = dev.ctx_ext;
        WdfObjectReference(mem); // the session lives in device_ctx_ext, see destroy_device_ctx_ext
        auto &s = *get_device_ctx_ext(mem).tls;

        TraceDbg("dev %04x", ptr04x(device));
        auto st = STATUS_SUCCESS;

        while (NT_SUCCESS(st)) {
                KeWaitForSingleObject(&s.send_wakeup, Executive, KernelMode, false, nullptr);
                if (is_stopped(s)) {
                        break;
                }
                st = send_pending_records(dev, s);
        }

        InterlockedExchange(&s.stopped, true); // tls::queue_send fails new sends from now on
        fail_pending_sends(dev, STATUS_CONNECTION_DISCONNECTED);

        if (NT_ERROR(st) && !get_flag(dev.unplugged)) {
                TraceDbg("dev %04x, detaching, %!STATUS!", ptr04x(device), st);
                device::async_detach_and_delete(device, true); // do not access dev after that
        }

        release_handles(s);
        TraceDbg("dev %04x, exited", ptr04x(device));
        WdfObjectDereference(mem);
}

_Function_class_(KSTART_ROUTINE)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void recv_thread_function(_In_ void *context)
{
        PAGED_CODE();

        auto device = static_cast<UDECXUSBDEVICE>(context);
        auto &dev = *get_device_ctx(device);

        auto mem = dev.ctx_ext;
        WdfObjectReference(mem);
        auto &s = *get_device_ctx_ext(mem).tls;

        TraceDbg("dev %04x", ptr04x(device));
        NTSTATUS st;

        while (true) {
                if (is_stopped(s) || get_flag(dev.unplugged)) {
                        st = STATUS_CANCELLED;
                        break;
                }

                ULONG size;
                st = read_record(s, 0, size);
                if (NT_ERROR(st)) {
                        break;
                }

                UCHAR *data;
                ULONG len;
                st = decrypt(s, size, data, len);
                if (NT_ERROR(st)) {
                        break;
                }

                if (len && !events::received(device, dev, reinterpret_cast<const char*>(data), len)) {
                        st = STATUS_INVALID_NETWORK_RESPONSE;
                        break;
                }

                consume(s, size);
        }

        if (!get_flag(dev.unplugged)) {
                TraceDbg("dev %04x, detaching, %!STATUS!", ptr04x(device), st);
                device::async_detach_and_delete(device, true); // do not access dev after that
        }

        release_handles(s);
        TraceDbg("dev %04x, exited", ptr04x(device));
        WdfObjectDereference(mem);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS start_thread(
        _In_ UDECXUSBDEVICE device, _Inout_ session &s, _Inout_ _KTHREAD* &result, _In_ PKSTART_ROUTINE func)
{
        PAGED_CODE();
        const auto access = THREAD_ALL_ACCESS;

        InterlockedIncrement(&s.refs); // released by the thread

        HANDLE handle{};
        auto st = PsCreateSystemThread(&handle, access, nullptr, nullptr, nullptr, func, device);
        if (NT_ERROR(st)) {
                InterlockedDecrement(&s.refs); // the owner still holds a reference
                Trace(TRACE_LEVEL_ERROR, "PsCreateSystemThread %!STATUS!", st);
                return st;
        }

        PVOID thread{};
        NT_VERIFY(NT_SUCCESS(ObReferenceObjectByHandle(handle, access, *PsThreadType, KernelMode, &thread, nullptr)));

        NT_VERIFY(!InterlockedExchangePointer(reinterpret_cast<PVOID*>(&result), thread));
        NT_VERIFY(NT_SUCCESS(ZwClose(handle)));

        return STATUS_SUCCESS;
}

} // namespace


_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::tls::get_server_location(
        _Out_writes_z_(host_size) char *host, _In_ size_t host_size,
        _Out_writes_z_(service_size) char *service, _In_ size_t service_size)
{
        PAGED_CODE();

        if (host_size) {
                *host = '\0';
        }
        if (service_size) {
                *service = '\0';
        }

        registry key;
        if (auto st = open_settings(key); NT_ERROR(st)) {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        WCHAR buf[256];
        UNICODE_STRING name{ .MaximumLength = sizeof(buf), .Buffer = buf };

        auto st = query_string(key.get(), L"Host", name);
        if (NT_ERROR(st) || !name.Length) {
                Trace(TRACE_LEVEL_ERROR, "%!USTR!\\Host is required", &settings_key);
                st = STATUS_DEVICE_CONFIGURATION_ERROR;
        } else if (st = unicode_to_utf8(host, static_cast<USHORT>(host_size), name); NT_ERROR(st)) {
                Trace(TRACE_LEVEL_ERROR, "%!USTR!\\Host '%!USTR!' %!STATUS!", &settings_key, &name, st);
        }

        ULONG port = default_port;

        if (NT_SUCCESS(st)) {
                UNICODE_STRING port_name = RTL_CONSTANT_STRING(L"Port");

                if (auto err = WdfRegistryQueryULong(key.get(), &port_name, &port); err == STATUS_OBJECT_NAME_NOT_FOUND) {
                        port = default_port;
                } else if (NT_ERROR(err)) {
                        Trace(TRACE_LEVEL_ERROR, "%!USTR!\\Port must be a REG_DWORD, %!STATUS!", &settings_key, err);
                        st = STATUS_DEVICE_CONFIGURATION_ERROR;
                }

                if (NT_SUCCESS(st) && !(port && port <= MAXUINT16)) {
                        Trace(TRACE_LEVEL_ERROR, "%!USTR!\\Port %lu is out of range", &settings_key, port);
                        st = STATUS_DEVICE_CONFIGURATION_ERROR;
                }
        }

        if (NT_SUCCESS(st)) {
                st = RtlStringCbPrintfA(service, service_size, "%lu", port);
        }


        if (NT_SUCCESS(st)) {
                Trace(TRACE_LEVEL_INFORMATION, "server %s:%s", host, service);
        }
        return st;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::tls::connect(_Inout_ device_ctx_ext &ext)
{
        PAGED_CODE();
        NT_ASSERT(!ext.tls);

        auto st = init_sspi();
        if (NT_ERROR(st)) {
                return st;
        }

        settings cfg;
        st = read_settings(cfg);
        if (NT_ERROR(st)) {
                return st;
        }

        auto s = alloc_session(ext);
        if (!s) {
                return STATUS_INSUFFICIENT_RESOURCES;
        }

        s->target.Buffer = s->target_buf;
        s->target.MaximumLength = sizeof(s->target_buf);
        RtlCopyUnicodeString(&s->target, &cfg.server_name);

        st = acquire_credentials(*s, cfg);
        if (NT_SUCCESS(st)) {
                st = handshake(*s);
        }
        if (NT_SUCCESS(st)) {
                st = after_handshake(*s, cfg);
        }

        RtlSecureZeroMemory(&cfg, sizeof(cfg));

        if (NT_ERROR(st)) {
                release_owner(*s);
                free(s);
        } else {
                ext.tls = s;
        }

        return st;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::tls::send(_Inout_ session &s, _In_reads_bytes_(len) const void *data, _In_ ULONG len)
{
        PAGED_CODE();

        event_lock lck(s.write_lock);

        auto st = append(s, data, len);
        if (NT_SUCCESS(st)) {
                st = flush(s);
        }

        return st;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::tls::recv(_Inout_ session &s, _Out_writes_bytes_(len) void *data, _In_ ULONG len)
{
        PAGED_CODE();
        auto dst = static_cast<UCHAR*>(data);

        while (len) {
                if (s.plain_pos == s.plain_len) {
                        s.plain_pos = s.plain_len = 0;

                        ULONG size;
                        auto st = read_record(s, 0, size);
                        if (NT_ERROR(st)) {
                                return st;
                        }

                        UCHAR *p;
                        ULONG n;
                        st = decrypt(s, size, p, n);
                        if (NT_ERROR(st)) {
                                return st;
                        } else if (n > max_record_body) {
                                return STATUS_INVALID_NETWORK_RESPONSE;
                        }

                        RtlCopyMemory(s.plain, p, n);
                        s.plain_len = n;

                        consume(s, size);
                        continue;
                }

                auto n = min(len, s.plain_len - s.plain_pos);
                RtlCopyMemory(dst, s.plain + s.plain_pos, n);

                s.plain_pos += n;
                dst += n;
                len -= n;
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::tls::start_receive_data(_In_ UDECXUSBDEVICE device)
{
        PAGED_CODE();

        auto &dev = *get_device_ctx(device);
        auto &s = *dev.ext().tls;

        auto st = realloc(dev.recv_buf, 1); // the parser of WSK receive events is reused
        if (NT_ERROR(st)) {
                return st;
        }

        if (auto n = s.plain_len - s.plain_pos) { // received together with OP_REP_IMPORT
                auto data = reinterpret_cast<const char*>(s.plain + s.plain_pos);
                s.plain_pos = s.plain_len = 0;

                if (!events::received(device, dev, data, n)) {
                        return STATUS_INVALID_NETWORK_RESPONSE;
                }
        }

        st = start_thread(device, s, s.recv_thread, recv_thread_function);
        if (NT_SUCCESS(st)) {
                st = start_thread(device, s, s.send_thread, send_thread_function);
        }

        TraceDbg("dev %04x, %!STATUS!", ptr04x(device), st);
        return st;
}

/*
 * @return object_reference defers dereference of the thread that executes this function, if it is ours
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED wdm::object_reference usbip::tls::stop_receive_data(_In_ UDECXUSBDEVICE device, _Inout_ bool &socket_closed)
{
        PAGED_CODE();

        auto &dev = *get_device_ctx(device);
        auto s = dev.ext().tls;

        if (s) {
                InterlockedExchange(&s->stopped, true);
        }

        socket_closed = close_socket(dev.sock()); // unblocks recv/send of the threads

        wdm::object_reference deferred;
        if (!s) {
                return deferred;
        }

        KeSetEvent(&s->send_wakeup, IO_NO_INCREMENT, false);

        _KTHREAD **threads[] { &s->recv_thread, &s->send_thread };

        for (auto t: threads) {
                wdm::object_reference thread(InterlockedExchangePointer(reinterpret_cast<PVOID*>(t), nullptr), false);

                if (!thread) {
                        continue;
                } else if (thread.get() == KeGetCurrentThread()) {
                        thread.set_defer_delete();
                        deferred = static_cast<wdm::object_reference&&>(thread);
                        continue;
                }

                auto st = KeWaitForSingleObject(thread.get(), Executive, KernelMode, false, nullptr);
                if (st != STATUS_SUCCESS) {
                        Trace(TRACE_LEVEL_ERROR, "dev %04x, KeWaitForSingleObject %!STATUS!", ptr04x(device), st);
                }
        }

        fail_pending_sends(dev, STATUS_CONNECTION_DISCONNECTED);

        if (auto &buf = dev.recv_buf) { // the receive thread, if it calls this function, does not use it anymore
                TraceDbg("ring buffer capacity %Iu", buf->capacity);
                free(buf);
        }

        release_owner(*s);

        TraceDbg("dev %04x", ptr04x(device));
        return deferred;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void usbip::tls::close(_Inout_ device_ctx_ext &ext)
{
        PAGED_CODE();

        if (auto s = ext.tls) {
                InterlockedExchange(&s->stopped, true);
                release_owner(*s);
        }
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void usbip::tls::queue_send(_Inout_ device_ctx &dev, _Inout_ session &s)
{
        if (is_stopped(s)) {
                fail_pending_sends(dev, STATUS_CONNECTION_DISCONNECTED);
        } else {
                KeSetEvent(&s.send_wakeup, IO_NO_INCREMENT, false);
        }
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void usbip::tls::free(_Inout_ session* &s)
{
        if (!s) {
                return;
        }

        if (auto refs = s->refs) {
                Trace(TRACE_LEVEL_ERROR, "session %04x, Schannel handles are not released, refs %ld", ptr04x(s), refs);
                free_buffers(*s);
        }

        ExFreePoolWithTag(s, pooltag);
        s = nullptr;
}
