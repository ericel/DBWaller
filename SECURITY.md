# Security Policy

DBWaller is currently an **experimental research project**. It is not yet considered production-hardened.

## Reporting a Vulnerability

If you believe you have found a security vulnerability, please **do not open a public GitHub issue**.

Instead:
- Open a GitHub Security Advisory (preferred), or
- Contact the maintainer privately (via GitHub profile contact info)

Please include:
- A clear description of the issue
- Steps to reproduce
- Impact assessment (what could an attacker do?)
- Any suggested fix or mitigation

## Scope Notes

DBWaller is an application-level **data-plane gateway** and cache engine.
It is **not** a network firewall and does not claim to protect against:
- Network-layer attacks (DDoS, packet-level exploits)
- Compromised host/kernel environments
- Secrets leaked from user application code

Security-sensitive defaults may change as the project evolves.
