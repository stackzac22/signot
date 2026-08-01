# Lessons learned

Incident write-ups in roughly chronological order. Each one is a real bug
with a real root cause — kept because the reasoning that found the cause is
more valuable than the fix itself.

## The OPi's "flaky network" was a physical antenna fault, not SD corruption

**Symptom:** the OPi kept bouncing online/offline on the mesh VPN — briefly
reachable, then gone, repeatedly. The board's status LEDs showed it healthy
the whole time (power good, kernel heartbeat alive), but it was completely
unreachable at the network layer — not even visible in a local ARP scan.

**First guess (wrong):** repeated freezes under load earlier had looked like
SD card corruption, and that verdict got carried forward onto this symptom
too.

**Actual cause:** the OPi's onboard WiFi antenna connector had physically
failed. The board and kernel were fine; the radio simply had no usable
signal to associate with, which explains both the online/offline flapping
(intermittent physical contact) and the total silence (no signal, no
association, nothing to see in a scan).

**Fix:** none available remotely — it's a hardware fault. But it turned out
to be *compensable*: physically relocating the board to sit right next to
its access point let it hold a strong, direct (non-relayed) link despite
the broken antenna. The lesson generalizes — a "flapping" device with a
healthy-looking LED is worth checking for a physical RF fault before
assuming it's a software or storage problem.

## A USB capture dongle can deadlock a whole board via the kernel's netlink lock

**Symptom:** the OPi locked up completely while running a WiFi monitor-mode
capture — load climbed to ~24, a chunk of the system's processes stuck in
uninterruptible sleep, and even `systemctl` calls hung.

**Root cause:** an `iw`/nl80211 operation on the USB WiFi chipset's driver
hung while holding the kernel's RTNL lock — the lock that basically
everything touching network state (including `systemd` itself) needs to
proceed. One stuck driver call cascaded into the whole system looking dead,
even though CPU usage was near-idle the entire time (it was lock
contention, not compute).

**Why this matters:** this class of failure is **not fixable from
userspace** once it happens — the affected processes ignore signals and
systemd itself is part of the pileup. The only recovery is a power cycle.
A software watchdog can't help either, since the watchdog process itself
would get stuck behind the same lock.

**Status:** the same board later ran the identical dongle in monitor mode
cleanly for extended periods with no recurrence, so it may have been a
kernel/firmware version issue that's since resolved, or it may simply not
have been triggered again yet. Treated as a live risk to watch (system load
under *sustained* capture, not just a quick test) rather than a solved
problem.

## x1's AP dongle firmware-crashes, and it takes the whole fleet down at once

**Symptom:** every scanning node would appear to go offline simultaneously —
looking exactly like a fleet-wide outage, or like each individual board had
crashed.

**Root cause:** x1's AP radio (the dongle broadcasting the fleet's private
WiFi network) would firmware-crash while in AP mode, dropping the interface
entirely. Because every other node's network path runs through this one
AP, they all lost connectivity at the same instant — which is exactly what
made it *look* like several unrelated devices had failed together, rather
than one shared dependency.

**Diagnostic takeaway:** when multiple independent-seeming nodes vanish from
the network at the same moment, check the thing they all depend on before
investigating them individually.

**Fix that actually worked:** a full kernel module reload of the WiFi
driver stack (unload, then reload) reliably recovers it. A lighter-weight
USB unbind/rebind of just the device is **not sufficient** — it can bring
firmware back up once, but re-entering AP mode immediately re-crashes it.
Only the full module reload clears the wedged driver state completely.

**Follow-up:** an automated watchdog now detects the crash signature and
runs the same reload sequence, so this now self-heals in under a minute
without intervention — but the underlying instability is a chipset/firmware
limitation, not something the reload sequence "fixes" permanently. A more
AP-stable chipset for this role remains an open improvement (see
OPEN-QUESTIONS.md).

**A subtlety that wasted debugging time:** the driver logs a lot of benign
background noise (transient firmware messages) that looks alarming but
doesn't correlate with an actual outage. Distinguishing the real crash
signal from cosmetic log noise, by checking actual interface mode/state
rather than reacting to every log line, cut down on unnecessary reloads
that were themselves briefly disruptive.

**Correction:** this incident's chipset was on record as an RTL8821CU-class
adapter, with detailed dmesg/driver evidence from that debugging session —
that chipset ID was accurate at the time. The radio currently on x1 is a
different unit: a TP-Link Archer T2U Nano (RTL8811AU), bought and swapped
in on 2026-08-01 specifically to test and gather signal/performance numbers
as a possible replacement. **The "unstable in AP mode" history belongs to
the retired RTL8821CU, not this T2U Nano** — the T2U Nano's own AP-mode
stability is brand new and unproven, evaluation still in progress.

## Two watchdogs racing each other caused a worse outage than either bug alone

**Symptom:** while investigating the AP crash above, a *second* recovery
watchdog got installed without checking whether one already existed for the
same radio. The two ended up racing — one would start a driver reload while
the other's reload was still in progress — producing a new failure
("device could not be readied for configuration") that neither one alone
would have caused.

**Fix:** retired the duplicate, merged its (legitimately better) health
checks into the original, and lengthened the settle time after a reload so
a check can't fire mid-recovery.

**Lesson:** before installing any new automated recovery mechanism on a
piece of shared infrastructure, check what's already running against it.
Self-heal systems that don't know about each other are a failure mode of
their own.

## One malformed frame from an unrelated device could crash a core network
service

**Symptom:** two seemingly unrelated alerts ("the AP is down" and "the
gateway itself lost its network") kept firing together, at the same
moments, on a recurring schedule.

**Root cause:** x1's WiFi supplicant process would segfault when it
received a specific malformed WiFi Easy Connect (DPP) frame from a nearby
device that was not part of the fleet at all — most likely a phone
repeatedly retrying a broken pairing flow. The crash took down *both* of
x1's radios at once (its own uplink and the AP it hosts for everyone else),
which is why the two alerts always fired as a pair — one root cause, not
two problems.

**Fix (mitigating blast radius, not the underlying bug):** the supplicant
service had no automatic restart configured at all, so a single malformed
frame meant a multi-minute outage before anyone noticed. Adding an
immediate auto-restart policy turned a multi-minute fleet-wide outage into
a few-second blip with no visible alert.

**Lesson:** a "fix" doesn't always mean patching the root cause (the buggy
service isn't something local to patch) — sometimes the right fix is making
the *failure* cheap enough that the underlying bug stops mattering
operationally. This also became a small side investigation into physically
locating the source device by signal strength, since the crashing frame's
sender could be triangulated from repeated captures — an example of the
fleet's own capture capability being turned toward a real, self-contained
RF investigation.

## USB 3.0 vs USB 2.0 enumeration failures on older chipsets

**Symptom:** a known-good capture dongle failed to enumerate at all on one
board's USB 3.0 port — no driver load, invisible to any wifi tooling — while
working perfectly on another board.

**Root cause:** an older-generation WiFi chipset design that doesn't
negotiate cleanly with some USB 3.0 (xHCI) controllers. Moving the same
dongle to a USB 2.0 port on the same board fixed it completely, no other
changes needed.

**Lesson:** "not accepting address" / protocol-level USB enumeration
failures on an otherwise-known-good device are worth trying a different USB
generation port before suspecting the cable, the power budget, or the
dongle itself.

## A cable that "sort of" worked was a bandwidth-class mismatch, not a power problem

**Symptom:** the same dongle failed to enumerate at all through one
particular USB OTG cable, while other USB devices (like a keyboard
receiver) worked fine through the identical cable.

**Diagnosis:** the devices that worked were all **full-speed** (12 Mbps)
USB devices; the dongle that failed is a **high-speed** (480 Mbps) device.
The cable could carry full-speed signaling but not high-speed — a
lower-bandwidth device tolerates a cable's imperfect shielding/impedance in
a way a higher-bandwidth device can't. Routing the same dongle through a
powered USB hub (which re-drives the signal) fixed it immediately.

**What it wasn't:** power. USB enumerates at a very low current draw by
spec, well before a device can request more — so a failure this early in
the handshake can never be a power/current shortfall, no matter how
tempting that explanation is. Ruled out kernel-level USB quirks and
timeout tuning too; neither addresses a signal-integrity problem that
occurs before the kernel even knows what device it's talking to.

**Lesson:** "the cable kind of works, just not for this device" is a real
and diagnosable category, not a contradiction — check whether the working
devices are lower-bandwidth than the failing one before concluding a cable
is fine.

## An "automatic" failover that depends on hand-carrying a USB dongle isn't automatic

**Symptom (design review, not a live outage):** the OPi/jacKed failover
watchdog is described — and works — as: jacKed detects the OPi is down and
starts its own capture. But there are only three capture-class USB radios
in the whole fleet, and they get physically moved between boxes by hand
rather than each box owning one permanently. If the OPi's dongle *is* the
one jacKed is supposed to fail over with, jacKed has nothing to capture
with until a person walks over and moves it — which defeats the point of
an unattended watchdog.

**Root cause:** the failover logic and the physical hardware allocation
were designed/discussed separately, so the software assumed a radio
availability the hardware layout didn't actually guarantee.

**Fix:** stop treating the three USB radios as interchangeable spares to be
quarterbacked on demand. Assign each fleet-critical role (OPi capture,
jacKed capture, x1 AP) its own permanently-owned radio, even if that means
one candidate device (the-one/6T) has to keep living with the
link-or-monitor tradeoff, since it's the opportunistic/mobile node, not
always-on core infra.

**Lesson:** a redundancy plan is only as automatic as its most manual
dependency. If recovering from a failure requires a human to relocate
hardware, it's a manual runbook wearing an automatic watchdog's clothes —
worth explicitly checking whether every resource a failover path needs is
actually already in place, not just that the software logic is correct.

## A stale regulatory domain silently emptied one radio's scan results

**Symptom:** one radio would report seeing WiFi networks (via the higher-level
tooling) while a second radio on the same box, doing a scan of its own,
found nothing at all — and connecting to a known-good network through the
second radio consistently failed as if the network weren't there.

**Root cause:** that radio's regulatory domain had reverted to the generic
"world" default, which marks nearly every band passive-scan-only — meaning
the radio could listen but not transmit the probe requests an active scan
needs, so its scans came back empty. The *other* radio on the same box had
its regulatory data baked into firmware and was unaffected, which is
exactly what made this look radio-specific rather than a box-wide setting.

**Fix:** explicitly set the regulatory domain and reload the driver to
clear the stale firmware state; made persistent so a reboot can't silently
revert it again.

**Diagnostic lesson:** a merged "networks visible" list from a
higher-level tool can mask which physical radio actually saw what. When one
of several radios on the same box is suspect, query that radio directly
rather than trusting an aggregated view.
