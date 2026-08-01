# Open questions — live planning doc

This is the actual working doc for the next round of hardware decisions.
Unlike the other docs (which record what already happened), this one is
meant to be edited as answers come in.

## 1. What radio should the OPi get next?

**Status: decided.** There is exactly one of each of the three USB radios
fleet-wide (Panda AXE3000, TP-Link T2U Nano, BrosTrend 650) — they've been
physically hand-carried between boxes rather than each box owning one
("quarterbacking"). **The Panda AXE3000 stays permanently assigned to the
OPi** and stops being moved anywhere else. Reasoning below.

The OPi's current capture radio (the Panda / MediaTek MT7921U, WiFi 6E)
already works, with one caveat: it once triggered a full kernel deadlock on
this specific board under sustained monitor-mode capture (see LESSONS.md),
and while it's since run clean multiple times, that risk isn't fully
cleared. That's a "watch it," not a "replace it" — it's still the
best-supported chipset of the three available (mainline driver, proven
elsewhere in the fleet too), so swapping it for something with worse Linux
support to dodge an unreproduced issue would trade a small unconfirmed risk
for a bigger confirmed one.

**Evaluation criteria, in priority order:**

1. **Monitor-mode support with a mainline (in-kernel) Linux driver.**
   Out-of-tree/DKMS drivers are a maintenance liability on an ARM board
   that gets reflashed/rebuilt — mainline support survives kernel upgrades
   without manual driver rebuilding.
2. **Injection support**, if the radio is ever meant to do more than
   passive wardriving (deauth/handshake-capture work). Several radios
   already in this fleet (ath10k) can capture but not inject — worth
   deciding up front whether that matters for this specific board.
3. **Chipset track record in *this* fleet.** MT7921U is proven on two
   boards. RTL8821CU is proven unstable specifically in AP mode, but that
   chipset has been retired from x1 — the T2U Nano (RTL8811AU) now there is
   a fresh swap-in (2026-08-01) being tested as its replacement, so its own
   AP-mode track record doesn't exist yet either way (see RADIOS.md).
4. **USB power draw**, since the OPi is a small board with limited USB
   power budget — a chipset known to need a powered hub is a real
   deployment cost, not just a spec-sheet footnote.
5. **DFS/channel restrictions.** The current MT7921U dongle has none (full
   2.4/5/6 GHz); some chipsets get pinned off DFS channels to dodge
   driver-level wedge issues, which shrinks scan coverage.

**Comparison table** (all three units identified and confirmed live):

| Candidate | Chipset | Driver | Monitor | Injection | DFS-safe | Verdict |
|---|---|---|---|---|---|---|
| Panda PAU0F AXE3000 | MT7921U | `mt7921u` (mainline) | Yes | — | Yes (no restriction) | **Assigned: stays on the OPi permanently.** Best-supported chipset of the three; no more shuffling it to jacKed. |
| TP-Link Archer T2U Nano | RTL8811AU | out-of-tree (aircrack-ng/morrownr fork, not mainline) | Yes, with patched driver | Yes, with patched driver | Needs DFS channels avoided | **Assigned: stays on x1** as the AP radio — freshly swapped in 2026-08-01 to replace the retired, AP-unstable RTL8821CU, and currently being tested/measured in that role. Don't move it mid-evaluation; see the "TP-Link T2U Nano → stays on x1" bullet in question 2 below for why. |
| BrosTrend 650 | RTL8821CU (confirmed) | `rtw88_8821cu` (mainline) | Yes, working live | Untested | Untested | **Assigned: on jacKed now**, closing the failover gap. Same chipset already proven unstable in AP mode elsewhere in this fleet, and now also throwing a repeating (non-fatal) driver WARNING during monitor-mode channel switching — see LESSONS.md. Working today; not a chipset to fully trust unattended long-term. |

## 2. Where should the other two radios go?

**Status: decided**, given the "one of each, shuffled" reality above. The
end state is: **stop shuffling, give each critical role a permanent
radio.**

- **TP-Link T2U Nano → stays on x1.** Moving it "to place it somewhere
  else" would mean physically pulling the radio broadcasting jack-link,
  which per LESSONS.md is the single dependency that takes the *entire
  fleet* offline at once when it's unavailable — reassigning it isn't a
  neutral placement decision, it's a deliberate fleet-wide outage. Leave it
  where it's already working.
- **BrosTrend 650 → jacKed.** Confirmed live (`lsusb`/`lsmod`/`dmesg` on
  jacKed): it's an RTL8821CU (driver `rtw88_8821cu`), and its MAC address
  matches the exact dongle from x1's old AP-crash history — this isn't a
  different unit, it's the same physical 8821CU, relocated rather than
  retired. It closes the failover gap as planned (jacKed now has its own
  capture radio instead of borrowing the OPi's) and is actively running
  Kismet capture successfully as of this check. **But it's not a clean
  bill of health** — see the new LESSONS.md entry: this exact chipset threw
  a kernel WARNING 30 times in ~2 hours during monitor-mode channel
  switching. System stayed healthy and Kismet kept working through it, so
  it's provisionally usable, but this is the same chipset family already
  proven unstable in AP mode elsewhere in this fleet — worth treating as
  "working for now, watch it" rather than "solved," and a stronger
  candidate to replace with a fourth, better-behaved radio if this recurs
  or gets worse under longer runs.
- **the-one (OnePlus 6T)** still has the single-radio link-or-monitor
  tradeoff, and stays that way for now — with all three dongles assigned to
  fixed core-infra roles above, there's nothing left to give it. This is an
  acceptable gap, not an oversight: the-one is the opportunistic/mobile
  node, not always-on core infra, so losing its own network link for the
  duration of a capture session is a much smaller cost than a broken
  failover story for the home fleet. If a fourth radio gets bought, this is
  where it goes.
- **x1's home-base scanning** (if wanted) has no dedicated radio under this
  plan either — see question 3 for why that's a deliberate tradeoff, not a
  gap.

## 3. Can x1's built-in WiFi card do home-base monitor-mode scanning or AP duty?

**Status: answered, by checking rather than guessing.**

Ran `iw dev` + `iw phy <n> info` on x1 directly. Two physical radios are
present:

- **phy0 (onboard):** currently the `managed` client connection x1 itself
  uses for its own uplink. `Supported interface modes` includes `monitor`,
  but `valid interface combinations` never pairs `monitor` with `managed`
  or `AP` — only `managed`/`AP`/P2P modes appear together. So the onboard
  card genuinely *can* do monitor mode, but **not at the same time as
  holding its own network connection.** Bringing it up in monitor mode
  means x1 temporarily drops its own uplink for the duration — unless x1
  has a wired (Ethernet) path that could take over that job instead, which
  would remove the tradeoff entirely and is worth checking separately.
- **phy1 (USB dongle):** this is the AP radio — see the correction in
  RADIOS.md, it's not the chipset previously on record.

This confirms the pattern predicted above: concurrent AP+monitor on one
physical radio is not available here, matching what the-one's radio also
showed. x1's AP role stays on its dedicated dongle; if home-base capture is
wanted without sacrificing x1's own connectivity, that needs its own radio
too — which feeds back into question 2, or the onboard card can be used for
scanning on a schedule that accepts the brief uplink drop.

## 4. What does a sane fleet end-state look like?

Synthesizing the lessons so far into a rough target, to sanity-check any
new radio purchase against:

- **Never make a single-radio device's uplink and capture mutually
  exclusive by accident.** Every box that's hit this problem (the-one, and
  originally the OPi) has the same fix: a second radio, with the stock
  radio pinned to the link and the second one dedicated to capture. This
  should be the default assumption for any new always-on capture node, not
  something discovered after the fact.
- **Keep AP duty on a chipset that's actually proven stable in AP mode**,
  separately from whatever's proven stable for capture — this fleet has
  already learned the hard way that those are different track records for
  the same chipset family, not the same question.
- **Prefer chipsets with mainline Linux support already proven somewhere
  in this fleet** over untested hardware, when the choice is otherwise
  close — the debugging cost of a new chipset's rough edges (see LESSONS)
  is real and has repeatedly eaten more time than the hardware itself
  cost.
- **Assume USB enumeration quirks are the norm, not the exception**, on
  small-board USB controllers — budget for "try a different port class" or
  "route through a hub" as a normal part of bringing up any new dongle,
  not a sign something is broken.
