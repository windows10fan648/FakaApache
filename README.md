# FakaApache

**FakaApache** is a small, educational HTTP/1.x static-file server written in C++17. It is intended for learning how TCP sockets, HTTP request parsing, response headers, static-file delivery, and basic connection limits fit together. It is **not** a replacement for Apache, Nginx, Caddy, or another production web server.

## Downloads and releases

Prebuilt Windows packages are published on the [GitHub Releases page](https://github.com/windows10fan648/FakaApache/releases). The recommended download is the portable Windows ZIP, which contains the executable, its default configuration, the `www` directory, and this README. Verify every downloaded archive against its accompanying SHA-256 checksum before use. [RELEASES.md](RELEASES.md) records the available assets and Windows verification command.

## Build and run

A C++17 compiler and POSIX-compatible operating system are required. Build the server with the following command.

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -pthread webserver.cpp -o fakaapache
```

Run the server from the repository root so that the default configuration can find the `www` directory.

```bash
./fakaapache --config siteconfig.fakaapache
```

The checked-in configuration binds only to `127.0.0.1:8080`. After starting the server, open <http://127.0.0.1:8080/> in a browser or run `curl -i http://127.0.0.1:8080/`.

## Windows and MinGW-w64

FakaApache supports **64-bit MinGW-w64** on Windows. Install a MinGW-w64 toolchain using its POSIX threading model, then run the following command from the repository root in a MinGW shell.

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -pthread webserver.cpp -o fakaapache.exe -static -static-libgcc -static-libstdc++ -lws2_32
```

Alternatively, use `mingw32-make` with the supplied `Makefile`; it detects Windows and produces `fakaapache.exe` with the required Winsock library. From another POSIX machine, use `make mingw` to cross-compile a Windows executable with `x86_64-w64-mingw32-g++-posix`.

A MinGW-ready [Code::Blocks project](FakaApache.cbp) is included as well. Open it in Code::Blocks, select a MinGW-w64 compiler toolchain, then build either the **Debug** or **Release** target. This project configuration follows the same Code::Blocks structure used by the related Mini-Terminal project while adding the `ws2_32` Winsock dependency required by FakaApache.

The Windows build initializes Winsock 2.2 before listening, uses `closesocket()` for socket cleanup, links against `ws2_32`, and retains all request-path, header-size, timeout, response-write, and concurrency protections of the POSIX build. The supplied build configuration statically links the MinGW runtime and pthread support, so the resulting executable needs only standard Windows system DLLs. The executable continues to read `siteconfig.fakaapache` from its working directory; run it from the repository root or provide an explicit `--config` path.

## Configuration

The configuration uses `key = value` entries. Lines beginning with `#` are comments. The server accepts the settings below.

| Setting | Default | Purpose |
| --- | --- | --- |
| `server_name` | `localhost` | A label shown in startup logs. It does not determine the listening interface. |
| `bind_address` | `127.0.0.1` | The IPv4 address on which to listen. Keep the default for local development; set `0.0.0.0` only when network access is intentional. |
| `port` | `8080` | The TCP port. It must be an integer from `1` through `65535`. |
| `root_directory` | `www` | The directory containing files that may be served. |
| `index_file` | `index.html` | The file served for a request ending in `/`. |

## Security and operational boundaries

The server URL-decodes and canonicalizes requested paths before serving them, rejects requests that escape the configured document root, and guards against symlink escapes. It accepts only `GET` requests, limits request headers to 16 KiB, applies a five-second receive timeout, protects sends from `SIGPIPE`, and limits active client handlers to 64.

These safeguards make the example safer for local experimentation, but they do not provide TLS, HTTP keep-alive, range requests, compression, access control, virtual hosting, rate limiting, structured logging, or a production-grade concurrency architecture. Do not expose it directly to the public internet.

## Tests

On a POSIX host, run `make test` after building to execute the integration checks. The suite verifies successful static-file delivery, unsupported-method handling, missing-file handling, percent-encoded traversal rejection, malformed URL rejection, oversized-header rejection, and invalid-port validation. The MinGW cross-build is verified separately with `make mingw`; executing the resulting Windows binary requires a Windows environment or compatible runtime.
