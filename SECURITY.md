# Security Policy

## Supported Versions

| Version | Supported |
|---------|-----------|
| Latest `main` | ✅ |
| Older tags | ❌ (no backports) |

## Reporting a Vulnerability

Please **do not** open a public GitHub Issue for security vulnerabilities.

Instead, report security issues by emailing the maintainer directly (see GitHub profile).

We aim to acknowledge reports within 72 hours and provide a fix or mitigation within 14 days.

## Scope

This repository contains build tooling, Kubernetes manifests, and documentation — no server-side code. The primary security surfaces are:

- **Talos machine config** (`*-talosconfig`, `worker.yaml`) — excluded from git via `.gitignore`. Contains cluster certificates and should be treated as a secret.
- **GHCR images** — the extension images are public. Pin image digests in production to avoid supply-chain substitution.
- **CDI device injection** — the device plugin runs with host access to `/var/run/cdi` and `/sys`. Do not grant `rockchip.com/npu` to untrusted workloads.
- **`procMount: Unmasked`** — exposes the full host `/proc` tree to the container. Only use this on trusted workloads.
