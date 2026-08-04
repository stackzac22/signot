# Offloading a heavy scanner to a second box

## The pattern in one sentence

The box that runs the scan *controller* doesn't have to be the box that runs
the *heavy scanner* — if you point the controller at a hardened scanner
daemon on a more capable node, a small board can drive a tool it could never
host itself.

This is the write-up of one concrete instance: getting **OWASP ZAP** (a
Java web-app scanner that wants server-class RAM) usable from the fleet's
scan controller, which runs on a 4GB Raspberry Pi that can't host ZAP.
Everything here generalizes to any "controller on a small box, heavy tool on
a capable box" split.

## Why this came up

The scan controller in the fleet (RaspyJack's "Ragnar" server-tier stack)
runs on **jacKed** — a Pi 4B, ~3.7GB usable RAM. Its Advanced Vuln tab
offers several scanners. Most are lightweight CLI tools. **ZAP is the
exception:** it's a Java daemon that holds ~1GB+ resident on its own and
spikes well past that during an active scan. The controller knows this — it
gates ZAP behind a RAM floor (~7.5GB) so a small board doesn't try to launch
it, OOM itself, and take the whole tab down. On jacKed, that gate correctly
keeps ZAP greyed out. So the button was there but permanently dark.

Two ways out: buy jacKed more RAM (can't — it's a Pi), or **run ZAP
somewhere else and point the controller at it.** The fleet already had a
"somewhere else."

## The second box: a repurposed phone

**the-one** is a retired OnePlus 6T running a full Linux userland
(Kali/NetHunter). It's an 8-core, ~7.7GB-RAM ARM64 machine that happens to
be shaped like a phone. It already earns its keep in the fleet running a
small local LLM, so it's on the tailnet and always-ish on. That RAM headroom
is exactly what ZAP wants.

The key realization: **a "phone" with 8GB and a real Linux userland is a
better ZAP host than a 4GB Pi**, even though the Pi is the "real computer" of
the two. Repurposed flagship phones are an underrated source of capable
always-on compute.

## Part 1 — a hardened ZAP daemon on the second box

ZAP ships a daemon mode with a REST/JSON API. Running it *well* as an
unattended service — coexisting with other work on a shared box, reachable
only by the right callers — took working around four separate things that
each broke it. All four are encoded in a systemd unit so a restart can't
regress them:

1. **Add-on auto-update hangs first launch forever.** On first start ZAP
   found dozens of newer add-ons and blocked on updating them over the box's
   flaky uplink, never binding its port. Start it with update-checking
   disabled and it binds in seconds.
2. **Cap the JVM heap explicitly.** ZAP's launcher otherwise grabs ~1/4 of
   system RAM, which on a shared box means it fights whatever else lives
   there (here, the local LLM) for memory. Pin a modest cap (e.g. ~1GB, set
   via the launcher's heap-properties file — which is read relative to
   `$HOME`, so the service must set `HOME` explicitly or the cap silently
   doesn't apply).
3. **Two independent access controls, both required.** ZAP has a *bind
   address* (what interface it listens on) **and** a *caller allowlist*
   (which source addresses the API will answer, defaulting to localhost
   only). Setting one without the other looks identical from outside — you
   get a refusal either way — so it's easy to spend an hour on the wrong one.
   For remote use you set both: bind somewhere reachable, and allowlist the
   management-network callers (plus loopback).
4. **Never background it over SSH.** The box's uplink drops sessions often
   enough that a `nohup`'d start silently vanishes mid-launch. Use a real
   service manager so the init system owns the process, not your SSH session.

### The self-call gotcha (worth its own note)

Once ZAP is bound to the box's management-VPN address, you'd expect the box
itself to be able to reach it there. **It can't** — a node calling its *own*
mesh-VPN IP has the packet routed over loopback instead of the VPN
interface, and the VPN's own firewall rule silently drops VPN-range traffic
that didn't arrive on the VPN interface. So `curl <my-own-vpn-ip>:<port>`
from the box hangs, while *other* nodes reach the same address fine, and the
box reaches it fine via `127.0.0.1`. This is not fixable from the caller
side.

The fix is to also listen on loopback. Since the daemon's bind option takes a
single address, bind `0.0.0.0` and then **re-close the door with a host
firewall chain** applied *before* the daemon comes up: allow loopback, allow
the management-VPN interface, drop the port on everything else. Net result:
the box can hit its own ZAP over `127.0.0.1`, fleet peers hit it over the
VPN, and the local/home WiFi it's physically joined to can't see the port at
all. (Idempotent chain, one interface-match per rule — some firewall
backends silently reject a rule with two interface matches and leave you
wide open, so verify the chain actually populated.)

## Part 2 — wiring the controller to use it

The controller hardwires `http://127.0.0.1:<port>` for ZAP and launches ZAP
as a local subprocess. To make it use a remote ZAP instead, three small,
opt-in changes — all gated on a single environment variable so the default
behavior is untouched for anyone not opting in:

1. **Read a remote endpoint from the environment.** If `ZAP_REMOTE_URL` (and
   an optional `ZAP_REMOTE_KEY`) is set, override the base URL and API key
   the controller uses for every ZAP call. There's a single choke point where
   the controller builds its API requests, so overriding two attributes
   reroutes *all* of them.
2. **Open the RAM gate when — and only when — a remote endpoint is set.**
   The gate exists to stop a small box launching a *local* ZAP. If ZAP is
   remote, the local RAM cost is zero, so the gate no longer applies. Guard
   this on the same env var so small boxes without a remote endpoint stay
   protected.
3. **Refuse to spawn a local ZAP in remote mode.** This is the safety
   interlock. The controller's "start ZAP" path first checks whether ZAP is
   *already* responding at its configured URL and, if so, just uses it and
   never spawns. Point the URL at the remote box and that early-return does
   the right thing for free. But if the remote box is *unreachable* (a phone
   roams off WiFi), the old code would fall through and launch a local ZAP —
   OOMing the small board, the exact thing we were avoiding. So in remote
   mode, make that fall-through **fail the scan cleanly** instead of ever
   spawning locally.

With those in place, the controller's ZAP button lights up on the small box,
and every ZAP request transparently runs on the capable box.

## Part 3 — making it survive upstream updates

The controller is a vendored copy of an upstream project, and its updater
does `git reset --hard origin/main` — which discards any local edit in the
tree. So in-tree patches don't survive an update. The durable pattern:

- Keep the changes as a **patch file and an env drop-in that both live
  *outside* the repo** (under `/usr/local/...` and in a systemd unit
  drop-in). The reset can't reach them.
- Re-apply the patch on **every service start** via an `ExecStartPre` hook,
  guarded so it's idempotent (skip if already applied), safe (only apply if
  it lands cleanly), and non-fatal (a patch that no longer applies because
  upstream moved the code is logged loudly but never blocks startup). If a
  future update makes the tool remote-aware for real, this patch simply
  stops applying and you delete it.

This is the same mechanism the fleet already uses to re-assert other local
controller customizations after each update — the offload is just one more
patch on the pile.

## Replicating it

You don't need *this* hardware — you need the shape:

- **A controller** you want to keep on a small/cheap always-on box.
- **A heavy tool** that box can't host.
- **A second box** with the resources, already reachable on your management
  network (a spare mini-PC, an old laptop, a repurposed flagship phone).

Then: run the hardened tool-daemon on the capable box, expose it only to your
management network + its own loopback, and teach the controller an opt-in
"remote endpoint" env var that (a) reroutes its calls, (b) drops the local
resource gate, and (c) refuses to fall back to a local launch. Keep the
wiring outside any auto-reset blast radius.

## Failure mode / caveat

If the second box is offline when you kick off a scan, the controller reports
the remote tool as unreachable and the scan errors — **by design**. The whole
point was to never fall back to a local launch that would kill the small box.
A roaming phone as the ZAP host means ZAP availability tracks that phone's
link; a wired always-on second box makes it rock-solid. Pick the second box
accordingly.

## Status

Working. The small controller box drives ZAP running on the second box; the
button is enabled in the controller UI; verified surviving a full reboot of
the controller box (the reset-proof re-apply does its job on boot). Open
items live in OPEN-QUESTIONS.md.

## Scope note

Host-specific addresses, ports, and API keys here are genericized —
placeholders, not live connection details. See the repo README's scope note.
Related: **[ARCHITECTURE.md](ARCHITECTURE.md)** for the mesh/AP split this
rides on, and **[LESSONS.md](LESSONS.md)** for the self-call and
auto-reset-war incidents referenced above.
