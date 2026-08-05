# Signot

A self-taught home-lab build log: a small fleet of repurposed hardware doing
distributed WiFi recon (wardriving / passive scanning), tied together over a
private mesh, with a Home Assistant dashboard on top. This repo is the
**architecture and lessons-learned record**, not the toolkit itself — it's
what I point an LLM at when I want help reasoning about the next hardware
decision, instead of re-explaining months of debugging from scratch.

I'm self-taught (started from zero in December 2025), so treat this as a
learning-in-public project. The goal isn't a product — it's building real
capability by running real hardware into real problems and writing down what
actually happened.

## What's in the fleet

| Nickname | Hardware | Role |
|---|---|---|
| **raspyjackboy** ("OPi") | OrangePi Zero 2W | Primary WiFi scanner |
| **jacKed** | Raspberry Pi 4B | Backup/failover scanner + server-tier compute |
| **x1** | Laptop (always-on) | Fleet hub: private AP, mesh VPN anchor, Home Assistant |
| **the-one** | Repurposed OnePlus 6T (Kali NetHunter, 8-core, ~7.7GB) | Heavy-lift sidekick — local 7B LLM host, remote OWASP ZAP engine, opportunistic wardrive rig |

All four sit on a private mesh VPN (tailnet) for management, plus a
purpose-built local WiFi network (**jack-link**) that x1 broadcasts, so the
scanning nodes can reach each other and a shared message bus even when the
home network's client isolation would otherwise block them.

> **On `the-one` — the sleeper of the fleet.** It's a retired OnePlus 6T, but
> under Kali NetHunter it's a full ARM64 Linux box with *more RAM and cores
> than the "real computers" it serves*. So it's not just a scanner — it's the
> fleet's borrowable compute: a small board (like the 4GB Pi) can offload a
> job it could never host itself — a local 7B LLM, a full OWASP ZAP scan —
> onto it over the mesh, then go back to being small. A repurposed flagship
> phone with a real Linux userland is a genuinely underrated always-on
> machine; [docs/ZAP-OFFLOAD.md](docs/ZAP-OFFLOAD.md) is one worked example,
> and I've almost certainly only scratched the surface of what it can take on.

## Docs

- **[docs/RADIOS.md](docs/RADIOS.md)** — every radio in the fleet: chipset,
  role, current status, and the failure modes each one has actually hit.
- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — how the boxes talk to
  each other: the mesh VPN, the local AP subnet, the message bus, the
  dashboard, and the self-heal watchdogs that keep it all up unattended.
- **[docs/ZAP-OFFLOAD.md](docs/ZAP-OFFLOAD.md)** — a reusable pattern:
  letting a small controller box (the 4GB Pi) drive a heavy scanner (OWASP
  ZAP) that actually runs on a more capable node (the repurposed 6T), with a
  hardened remote daemon and an update-proof way to wire it in.
- **[docs/LESSONS.md](docs/LESSONS.md)** — incident write-ups. Real bugs,
  real root causes, in the order I found them.
- **[docs/OPEN-QUESTIONS.md](docs/OPEN-QUESTIONS.md)** — the live planning
  doc: what radio the OPi should get next, where the other spare radios
  should go, whether x1's built-in WiFi card can pull double duty, and what
  a sane end-state for the fleet looks like.

## Related repos

- [`Raspyjack`](https://github.com/stackzac22/Raspyjack) — my fork of the
  actual RaspyJack toolkit (the code that runs on OPi/jacKed), including the
  OrangePi port. This repo is deliberately kept separate from that one so
  the architecture/planning notes aren't tangled up with a red-team-toolkit
  fork's commit history.
- [`presence-lab`](https://github.com/stackzac22/presence-lab) — a related
  but distinct project: RF human-presence sensing (mmWave / WiFi-CSI / PIR),
  not WiFi recon.
- [`bushnode`](https://github.com/stackzac22/bushnode) — an outdoor sensor
  node in the fleet: dual-radio ESP32 (Heltec V3 + XIAO C5) passive WiFi
  scanning with a LoRa/Meshtastic uplink. Kept as its own firmware repo (its
  own build/flash lifecycle, like `Raspyjack`) and linked here rather than
  vendored into the notes.
- [`fleet-bridge`](https://github.com/stackzac22/fleet-bridge) — the ESP32
  WiFi-to-WiFi NAT bridge the fleet sits on: STA joins an upstream hotspot, AP
  rebroadcasts a fixed SSID, NAPT routes clients out through the uplink. Headless
  (WROOM) and TTGO T-Display variants. Its own firmware repo, linked here.

## A note on scope

Everything here has host-specific IPs, MAC addresses, and credentials
scrubbed or genericized — this describes *how the architecture works*, not
live connection details for the network it runs on.
