# Constella project instructions

Read `CLAUDE.md` for the project handoff, architecture, and operating constraints.

## Production boundary — confirmed by Craig, 2026-09-30

- Only Craig operates production. Never SSH into production, execute production
  commands, deploy, or change production infrastructure on his behalf.
- Prepare and test changes locally. Give Craig the exact commands, expected
  verification results, and rollback instructions; Craig runs them himself.
- A request to prepare a deployment or configure production is a request for
  local preparation and operator instructions, not permission to execute them.
- Never `git push`.

## Intended hosting

- Use an existing, underutilized Lightsail instance for the Explorer, Postgres,
  and one node. The instance has not yet been identified.
- Craig plans a second node on another Lightsail instance and one in the homelab.
- Cloud mining versus validation/relay-only operation remains undecided.
- Account for the existing workloads when preparing resource limits, ports,
  storage, and deployment commands for the shared instance.
