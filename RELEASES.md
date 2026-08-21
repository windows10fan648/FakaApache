# FakaApache Releases

This page records the public FakaApache releases. Downloadable artifacts are available from the repository’s [GitHub Releases page](https://github.com/windows10fan648/FakaApache/releases).

## v1.0.0 — MinGW-w64 Windows release

Version **v1.0.0** is the first documented FakaApache release. It includes a prebuilt, 64-bit Windows console executable compiled with MinGW-w64, along with a complete portable bundle containing the default configuration, web root, and documentation.

| Asset | Purpose |
| --- | --- |
| `FakaApache-v1.0.0-windows-x64.zip` | The recommended portable Windows download. Extract it, keep the included files together, and run the executable from the extracted directory. |
| `FakaApache-v1.0.0-windows-x64.zip.sha256` | SHA-256 checksum for the portable Windows bundle. |
| `FakaApache-v1.0.0-windows-x64.exe` | Standalone Windows executable for users who already have a compatible configuration and web-root directory. |
| `FakaApache-v1.0.0-windows-x64.exe.sha256` | SHA-256 checksum for the standalone executable. |

Run the portable build from Command Prompt or PowerShell using:

```text
FakaApache-v1.0.0-windows-x64.exe --config siteconfig.fakaapache
```

The checked-in default configuration listens only at `127.0.0.1:8080`. Browse to `http://127.0.0.1:8080/` once the server starts. For source builds, configuration settings, and safety limits, read [README.md](README.md).

## Verifying a download

On Windows PowerShell, calculate a SHA-256 hash with the following command. Compare it with the corresponding `.sha256` asset published in the same release.

```powershell
Get-FileHash .\FakaApache-v1.0.0-windows-x64.zip -Algorithm SHA256
```

> FakaApache is an educational static-file server and should not be exposed directly to the public internet.
