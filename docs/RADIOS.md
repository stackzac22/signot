# Radio inventory

*"Monitor mode" = a radio can passively capture all 802.11 traffic on a
channel, not just packets addressed to it. That's what wardriving/scanning
needs. Most consumer WiFi chipsets and drivers on Linux don't expose it —
it depends on the chipset **and** how complete the driver is, not just the
hardware spec sheet.*

Every radio actually in service, what it's good for, and what's gone wrong
with it in practice.

| Device | Radio | Chipset / driver | Role | Status |
|---|---|---|---|---|
| OPi (raspyjackboy) | onboard WiFi | Unisoc `sprdwl_ng` (`unisoc_wifi`) | jack-link uplink | Working, but physically fragile — see antenna note below |
| OPi (raspyjackboy) | USB — Panda Wireless PAU0F (AXE3000), WiFi 6E | MediaTek MT7921U (`mt7921u`, USB ID `0e8d:7961`) | Monitor-mode capture (Kismet), full 2.4/5/6 GHz, no DFS restriction | Working, with an unresolved wedge risk under sustained load (see LESSONS). **Now permanently assigned here** — no longer shuffled to jacKed (see OPEN-QUESTIONS.md). |
| jacKed (Pi4B) | onboard WiFi | Broadcom `brcmfmac` | jack-link uplink **only** | Working — but confirmed via `iw phy` to have **no monitor mode at all**. Hard hardware limit, not a config issue. |
| jacKed (Pi4B) | USB — BrosTrend 650 | Realtek `RTL8821CU` (`rtw88_8821cu`) — **confirmed, and same physical unit as x1's old AP dongle below, relocated not retired** | Monitor-mode capture / failover scanner | **Live and working** — closes the failover-automation gap (see LESSONS.md). But: this exact unit is throwing a repeating non-fatal driver `WARNING` during monitor-mode channel switching (30x in ~2h, see LESSONS.md), on a chipset already proven unstable in AP mode elsewhere. Provisional, not fully trusted yet. |
| x1 | onboard WiFi | vendor unconfirmed | Station uplink to the home network | Working as a client radio. **Monitor-mode capability untested** — open question, see OPEN-QUESTIONS.md. |
| x1 | USB — TP-Link Archer T2U Nano (AC600) | Realtek `RTL8811AU` | Runs the jack-link AP (the fleet's private local subnet) | **Freshly installed 2026-08-01** — a new purchase, swapped in today to test/gather signal numbers as a candidate replacement for the retired RTL8821CU behind the AP-crash history below. AP-mode stability not yet established; evaluation in progress. |
| the-one (OnePlus 6T) | onboard WiFi | Qualcomm `ath10k_snoc` (WCN3990) | Either jack-link uplink **or** monitor capture — not both at once | Working, genuinely monitor-capable (unusual for a phone SoC radio), **capture only, no packet injection**. One radio total, so flipping to monitor drops the uplink. |

## The scarcity behind this table

There are only **three** capture/AP-class USB radios in the entire fleet —
the Panda AXE3000, the T2U Nano, and a BrosTrend 650 — and historically
they've been physically hand-carried between boxes rather than each box
owning a dedicated one ("quarterbacking" placement). That's fine for a
radio whose job is occasional/manual, but it quietly breaks anything that's
supposed to be *automatic* — see the failover gap in LESSONS.md. The
recommendation in OPEN-QUESTIONS.md is to stop shuffling and give each
critical role its own permanently-assigned radio.

## The recurring pattern — on the scanning nodes

On every **scanning** box (OPi, jacKed), the same rule keeps proving itself
out: **the stock/onboard radio owns the network link, and any USB capture
dongle is scan-only and never becomes the route.** The reasoning is
mechanical, not aesthetic — dongles get unplugged and moved between boxes
regularly, and if the dongle were ever the uplink, unplugging it would drop
that box off the network entirely. Under this rule, pulling a dongle costs
only monitor mode; the link itself never flinches.

**x1 is the deliberate exception, not a violation of the rule.** Its USB
T2U Nano dongle *is* the route — it's the radio broadcasting jack-link for
every other node. That's fine precisely because x1's onboard card is doing
its own separate job (client uplink to the home network) rather than being
idle, so there's no scenario where pulling that dongle "just costs monitor
mode" — it costs the whole fleet's shared network, which is exactly the
single-point-of-failure risk documented in LESSONS.md.

A box with only one radio (the-one, and originally the OPi before it got a
dongle) cannot both hold the network link and monitor-scan simultaneously —
that requires two independent radios, full stop. See
[OPEN-QUESTIONS.md](OPEN-QUESTIONS.md) for how this constrains the next
hardware purchases.

## Chipset track record so far

- **MediaTek MT7921U (`mt7921u`)** — the fleet's proven capture chipset.
  WiFi 6E, no DFS channel restriction, works on both the OPi and the Pi4B.
  One serious incident (a kernel deadlock on the OPi under sustained
  capture — see LESSONS) that has not reproduced since, but isn't fully
  cleared either.
- **Realtek RTL8821CU (`rtw88_8821cu`)** — proven *unstable* in AP mode,
  with a well-documented firmware-crash/reload cycle. Retired from x1's AP
  role 2026-08-01 (replaced by a TP-Link T2U Nano, RTL8811AU, bought
  specifically to test as a candidate — its own AP-mode stability is
  unproven so far). This exact 8821CU unit didn't leave the fleet, though —
  it's now jacKed's capture radio (see the jacKed row above), where it's
  working but throwing a repeating non-fatal driver WARNING during channel
  switching. Two different failure modes on the same chipset (AP-mode
  firmware crash vs. monitor-mode channel-switch WARNING) — worth treating
  this chipset family as generally not fully trustworthy unattended, in
  either role, rather than writing off just the AP-mode issue as fixed by
  switching roles.
- **Qualcomm ath10k (`ath10k_snoc`)** — capture-capable, injection-incapable.
  Fine for passive wardriving, useless for anything that needs to transmit
  attack frames.
- **Broadcom `brcmfmac`** (Pi4B onboard) — link-only, no monitor mode,
  confirmed via `iw phy`'s supported-interface-modes list. Don't waste time
  trying to coax capture out of it.
