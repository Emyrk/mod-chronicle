# mod-chronicle — Remaining Future Work

This file tracks the **actual remaining gaps** after the ingestion hardening
work completed in April 2026.

## Attribution and identity

- **Recursive owner-chain attribution**
  - Follow owner chains (pet/guardian/totem/charmed entity) instead of using
    only the immediate owner GUID.
- **Stable creature spawn identity**
  - Evaluate using `GetSpawnId()` where Chronicle benefits from stable spawn
    identity across respawns or instance re-entry.
- **Scope filtering**
  - Optionally suppress irrelevant units or noise sources when that improves
    Chronicle signal quality without hiding meaningful combat state.

## Test coverage

- **Formatter regression tests**
  - Add focused tests for GUID formatting, spell prefixes, damage suffixes,
    encounter events, and Chronicle extension field ordering.
- **Parser-alignment regression fixtures**
  - Keep sample log lines for newer Chronicle extensions, especially
    `CHRONICLE_UNIT_INFO` with appended `affiliation` and `isBoss` fields.

## Upload architecture follow-up

- **Replace detached thread fan-out with a worker queue/service**
  - Current upload and ping tasks are tracked for shutdown safety, but still
    use detached threads internally.
- **Optional retry policy tuning**
  - Startup orphan retry is present; only add explicit retry/dead-letter policy
    if operational evidence shows it is needed.

## Account-linking design

- Design account-linking before implementation:
  - emitted identifier shape
  - privacy implications
  - proof-of-ownership flow
  - Chronicle-side linking UX

## Upstreaming

- Split upstreaming into:
  1. core observer-hook contribution
  2. module cleanup and alignment once hook interfaces stabilize

See `UPSTREAM_HOOK_PLAN.md` for the current contribution package.
