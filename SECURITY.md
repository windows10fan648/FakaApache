# Security review and deployment checklist

FakaApache is educational software and has not received an independent penetration test. Before public deployment, commission an external review covering HTTP parsing, path canonicalization, TLS configuration, authentication, rate limiting, proxy forwarding, resource exhaustion, Windows and Linux builds, and the Nginx boundary.

The repository provides non-destructive regression checks:

```text
python tests/security_smoke.py --url http://127.0.0.1:8080
python tests/deployment_validation.py --url http://127.0.0.1:8080
python tests/soak_test.py --url http://127.0.0.1:8080/healthz --duration 1800 --workers 32
```

The smoke tests are not a penetration test. Do not run active security testing against a system without authorization.

Review each release for dependency changes, compiler warnings, CodeQL findings, exposed ports, file permissions, TLS certificate validity, Nginx configuration, backup restoreability, and rollback success.

The review must also verify that proxy forwarding cannot reach untrusted destinations, authentication is enabled where required, credentials are not committed, configuration files are readable only by the service account, malformed and oversized requests are rejected, and Nginx does not bypass the intended static/API route split.
