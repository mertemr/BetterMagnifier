# Security Policy

## Reporting a vulnerability

Please report security issues privately through
[GitHub Security Advisories](../../security/advisories/new) for this
repository rather than opening a public issue. That gives us a private
channel to discuss and fix the problem before any details are public.

Include as much of the following as you can:

- What the issue is and why it's a security concern (not just a bug)
- Steps to reproduce, or a proof of concept
- The affected version / commit
- Impact you'd expect in a realistic scenario

We'll acknowledge new reports as soon as we can and follow up with a fix
timeline once the issue is understood.

## Scope

BetterMagnifier is a Windows desktop application that runs with administrator
privileges (see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for why) and
installs low-level keyboard and mouse hooks. Reports involving privilege
escalation, hook-based input interception affecting other applications, or
unsafe handling of the elevated process are all in scope.

Reports about the inherent behavior of `RequireAdministrator` (a UAC prompt on
every launch) or the documented limitations in `docs/ARCHITECTURE.md` are not
security issues — they're already tracked as known tradeoffs.

## Supported versions

This project is pre-1.0 and does not yet maintain parallel release branches.
Fixes land on `main`; please test against the latest release before reporting.
