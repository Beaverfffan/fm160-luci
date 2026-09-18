# Provenance and licensing

This repository contains two kinds of code. They are licensed differently and
the distinction is deliberate — please read this before reusing anything.

## 1. `at-daemon/` — vendored from QModem (MPL-2.0 + non-commercial)

`at-daemon/` is the `ubus_at_daemon` application taken **unmodified** from
[FUjr/QModem](https://github.com/FUjr/QModem), used here as the AT transport
layer.

| | |
|---|---|
| Upstream | `https://github.com/FUjr/QModem` |
| Path | `application/ubus_at_daemon` |
| Commit | `86102c2a6f62` (2026-09-11) |
| License | MPL-2.0, plus an additional non-commercial restriction |

The whole point of vendoring it unmodified is that the AT transport is the one
piece of this stack that has to be trusted absolutely, so its behaviour should
be auditable against upstream by hash rather than by reading a diff. All ten
files under `at-daemon/src/` **and both files under `at-daemon/files/`** are
byte-identical to the upstream commit above; their git blob hashes are listed in
`at-daemon/NOTICE.md` and can be re-checked at any time.

Upstream's own `LICENSE` is carried verbatim as `at-daemon/LICENSE`. It is
MPL-2.0 with an added clause: **commercial use of the software or any
derivative work is strictly prohibited.** That restriction is upstream's, not
ours, and it travels with the code.

Note that the licence does not spread to the rest of this repository: `fm160d`
never links against `ubus-at-daemon`, it talks to it over ubus across a process
boundary.

## 2. Everything else — original work (MPL-2.0)

`fm160d/`, `luci-app-fm160/` and `docs/` — plus the packaging that wraps the
vendored component (`at-daemon/Makefile`, `at-daemon/version.mk`,
`at-daemon/README.md`, `at-daemon/NOTICE.md`) — were written for this project and
are released under the **Mozilla Public License 2.0**.

## Not in this repository

Three things are deliberately absent:

- **`_probe/`** — the raw AT transcripts captured on real hardware. They carry
  the test module's IMEI and serial number. The conclusions drawn from them are
  in `docs/HARDWARE-PROBE.md`.
- **Fibocom's documentation** — the `AT Commands User Manual`, the GNSS
  Application Guide and the ECM/NCM/RNDIS/MBIM dial-up integration guide are
  Fibocom's copyrighted material. `docs/AT-FACTS.md` records only the facts this
  project depends on, with page references back to the manuals; the manuals
  themselves are not redistributed.
- **QModem's Lua/LuCI application code** — only the `ubus_at_daemon` transport
  is vendored. The management UI in this repository is written from scratch and
  shares no code with QModem's, because the design goals differ: QModem is a
  multi-vendor framework, this is single-module by choice.
