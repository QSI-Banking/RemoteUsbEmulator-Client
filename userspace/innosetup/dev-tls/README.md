# Development TLS tunnel (DO NOT USE IN PRODUCTION)

These files let the installers set up a mutual-TLS tunnel with [stunnel](https://www.stunnel.org)
between usbipd-win (server) and usbip-win2 (client) without any manual certificate work.

| File | Installed by | Purpose |
| --- | --- | --- |
| `ca.crt` | both | Development CA; each side only trusts peers it signed |
| `server.crt`, `server.key` | server MSI | Server identity, CN/SAN `usbip-dev-server` |
| `client.crt`, `client.key` | client setup | One client identity shared by every client |
| `Setup-UsbipTunnel.ps1` | both | Installs stunnel if missing, writes `stunnel.conf`, starts the `stunnel` service |

```
client PC                                              server PC
usbip.exe -> driver -> 127.0.0.1:3240 -> stunnel ==TLS 3241==> stunnel -> 127.0.0.1:3240 -> usbipd
```

- One server accepts any number of clients; they all present `client.crt`.
- The client checks the server certificate by name (`checkHost = usbip-dev-server`), so any server IP works.
- The private keys are in this repository on purpose. Anyone with them can connect to a development
  server. Before production, replace them with a real CA and per-client certificates, and stop shipping keys.
- The CA private key is not in the repository, so new certificates cannot be issued from these files.
- Logs: `%ProgramData%\usbip-dev-tls\setup.log` and `%ProgramData%\usbip-dev-tls\stunnel.log`.
- Certificates expire in November 2036.
