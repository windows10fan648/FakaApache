# FakaApache

**FakaApache** is a small, educational HTTP/1.x static-file server written in C++17. It is intended for learning how TCP sockets, HTTP request parsing, response headers, static-file delivery, and basic connection limits fit together. It is **not** a replacement for Apache, Nginx, Caddy, or another production web server.

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
