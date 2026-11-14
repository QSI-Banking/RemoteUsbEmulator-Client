# Remote USB client (USBip)

Attaches a USB device shared by a [RemoteUsbEmulator-Server](https://github.com/QSI-Banking/RemoteUsbEmulator-Server)
(usbipd) to this Windows PC, such as a license dongle. The connection is mutual TLS, made by the driver itself, and
each attach needs the user's name and a one-time code from their authenticator app.

## Requirements

- Windows 10 x64 version 1903 or later.
- A **client certificate** with its private key in Local Computer > Personal, issued by the CA the server trusts.
  The server administrator must allow it (`usbipd client add`).
- The CA of the **server certificate** in Local Computer > Trusted Root, and the server's address, port and
  certificate name.
- **Test signing, for now.** The driver is signed with a test certificate, so Windows loads it only with Secure Boot
  off and test signing on. Development and testing only; production signing is planned.
  ```powershell
  bcdedit /set testsigning on   # elevated, then reboot ("Test Mode" shows on the desktop)
  ```

<details>
<summary>Test certificates (development only)</summary>

The server README has a script that creates a test CA, a server certificate and a client certificate. On this PC,
import the CA `.cer` into Local Computer > Trusted Root and the client `.pfx` into Local Computer > Personal:

```powershell
Import-Certificate -FilePath .\usbip-dev-ca.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-PfxCertificate -FilePath .\usbip-dev-client.pfx -CertStoreLocation Cert:\LocalMachine\My -Password (Read-Host -AsSecureString)
```

</details>

## Install

Run the installer (`USBip-<version>-x64-release.exe`) as an administrator. It does not use PowerShell. Its pages:

| Page | Field | Notes |
|---|---|---|
| Server | Address | Host name or IP address of the server |
| | Port | Default 3241 |
| | Server certificate name | The DNS name in the server certificate; the connection fails if it does not match |
| | Server certificate pin | Optional SHA-256 of the server certificate; when set, only that certificate is accepted |
| Client certificate | Certificate | Chosen from Local Computer > Personal certificates with a private key |

Silent install: `USBip-<version>-x64-release.exe /VERYSILENT /SERVER=<address> /PORT=3241 /SERVERNAME=<name> /SERVERPIN=<sha256> /CLIENTCERT=<SHA-1 thumbprint>`

The settings are kept in the driver's Parameters registry key, which only administrators can change.
To change them, run the installer again.

## Initial setup

1. Check that the server can be reached: `Test-NetConnection <server> -Port 3241`.
2. The server administrator adds your user and shows a QR code once; scan it with your authenticator app.

## Ongoing use

`usbip.exe` is on the PATH:

| Task | Command |
|---|---|
| Attach your device | `usbip attach --user <name>` (asks for the code), or `--otp <code>` |
| See attached devices | `usbip port` |
| Detach | `usbip detach --port <port>`, or `usbip detach --all` |

A session ends at the server's time limit, when you detach, or when an administrator ends it. The client never
reconnects by itself, also not after a dropped connection or a reboot: attach again with a new code.

## Security

- The driver makes the TLS connection itself and checks the server certificate's name (and pin, if set) before
  sending anything. It presents this PC's client certificate.
- The one-time code is sent only inside that connection and is not stored.
- No automatic reconnect, no persistent devices: every attach needs a user and a new code.
- Only administrators can change the settings. No passwords or secrets are stored on the client.

## Troubleshooting

| Message | Meaning | What to do |
|---|---|---|
| Authentication failed | Unknown user, wrong code, or a code that was already used | Wait for the next code; check the phone's clock |
| No device is bound to user | Your user has no device on this server | Ask the server administrator |
| Cannot be attached until an administrator checks it | The server restarted, or the device was unplugged or moved | Ask the server administrator |
| Server unavailable | The server cannot reach its database | Try later; tell the server administrator |
| Connection refused or TLS failure | Wrong address or port, server certificate name or pin does not match, or this PC's certificate is not allowed | Check the installer settings; ask the administrator to check `usbipd client list` |
| `VCRUNTIME140D.dll` was not found | A Debug build was installed | Install a Release build |

## Building from source

Visual Studio with the WDK. Build Release for any PC without Visual Studio (Debug builds need the debug C++ runtime):

```powershell
& $msbuild usbip_win2.slnx /restore /m /p:Configuration=Release /p:Platform=x64
```

The installer is `x64\Release\USBip-<version>-x64-release.exe`. Test plan: TESTING.md in the server repository.

## Credits and license

Based on [usbip-win2](https://github.com/vadimgrn/usbip-win2) by Vadym Hrynchyshyn and contributors.
Licensed under the GNU General Public License v3.0; see [LICENSE.txt](LICENSE.txt).
