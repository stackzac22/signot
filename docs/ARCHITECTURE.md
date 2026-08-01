# Fleet architecture

## Two networks, one purpose each

Every box in the fleet is reachable over two separate networks, and they're
kept deliberately distinct:

1. **A mesh VPN (tailnet)** — every device gets a stable virtual IP no
   matter what physical network it's actually on. This is the management
   plane: SSH, dashboards, "is it alive" checks. It survives a box roaming
   between WiFi networks.
2. **jack-link** — a private local WiFi network that **x1 itself
   broadcasts** from a USB AP dongle. All the scanning nodes join this
   instead of the home WiFi. Two reasons this exists rather than just using
   the home network directly:
   - The home network's gateway enforces **client isolation** — devices on
     it can reach the internet but not each other. That's fatal for a fleet
     whose whole point is nodes talking to a shared broker.
   - It keeps recon traffic on a network scoped to the project instead of
     mixed in with every other device in the house.

   jack-link hands out a private `/24` and NATs internet out through x1's
   own uplink, so nodes on it get both connectivity to each other *and*
   internet access, in one hop.

Nodes generally reach each other over jack-link when both are on it, and
fall back to the tailnet when they're not (e.g. a phone rig that's mobile
and off the local subnet).

## The stack

```
 scanning nodes (OPi, jacKed's dongle, opportunistic 6T)
        │  Kismet capture
        ▼
 Kismet → MQTT bridge
        │
        ▼
 MQTT broker  ◄──────────────►  Home Assistant (dashboard/automations)
        │
        ▼
 physical status indicator (an ESP32 LED display)
```

- **Kismet** does the actual 802.11 capture on whichever radio is in
  monitor mode for a given node.
- A small bridge script polls Kismet's REST API and republishes
  alerts/status onto **MQTT**, which is the fleet's shared message bus —
  every node publishes to it, Home Assistant subscribes to it.
- **Home Assistant** runs as a container on x1 and is the single dashboard
  and automation layer for the whole fleet (see "one Home Assistant, not
  two" below).
- A small ESP32-based physical indicator reflects scan state (e.g. a
  distinct LED color/animation while any node is actively scanning) —
  decoupled from any one node, so it reflects whichever box happens to be
  scanning at the time.

## Failover: primary/backup, not dual-primary

The OPi is the primary scanner. jacKed runs a **failover watchdog** that
pings the OPi on an interval and, after several consecutive misses, is
meant to start its own Kismet capture and stop again once the OPi is
confirmed back — specifically so the two never scan the same airspace at
once. This is deliberate: the design is "one active scanner at a time with
an automatic standby," not two scanners running in parallel.

**Caveat:** the watchdog logic assumes jacKed has its own capture-capable
radio to switch on. Today it doesn't — see the radio-assignment gap in
LESSONS.md and the BrosTrend plan in OPEN-QUESTIONS.md. Until that's
resolved, this failover is software-ready but hardware-incomplete.

## Self-heal watchdogs

Unattended fleet hardware fails in boring, repeatable ways — a WiFi driver
gives up retrying, a USB dongle's firmware wedges, a radio silently drops to
the wrong mode. Rather than debug each occurrence by hand, every box that
can run one has a small watchdog:

- **Link watchdogs** on the scanning nodes check the default route
  periodically and step through unblock → reconnect → service restart →
  driver reload → (last resort) full reboot, escalating only as far as
  needed. They specifically check *why* the link is down before escalating
  — if other networks are visible but not this fleet's, the radio is fine
  and the AP is just temporarily down, so they hold rather than reboot a
  healthy box for someone else's outage.
- **The AP watchdog on x1** is the most important one, because x1's AP
  radio is a known-fragile chipset (see LESSONS) and if it goes down, every
  scanning node loses its network at once. It checks not just "is the
  interface up" but "is it actually still in AP mode broadcasting the right
  network and holding the right address" — a naive check that only looked
  at link state would have missed real failures where the radio silently
  fell back to client mode.
- Watchdogs specifically avoid duplicating each other — two independent
  watchdogs racing to recover the same radio at the same time has, in
  practice, caused *worse* outages than no watchdog at all (see LESSONS).

## One Home Assistant, not two

At one point two separate Home Assistant containers ended up running on two
different boxes simultaneously, both trying to claim the same ESPHome
devices — a classic split-brain that causes constant reconnect churn on
anything both instances can see. The fix was purely organizational: pick one
box as authoritative (x1, because it's the always-on hub with the most
direct network reach to every other node) and make sure nothing else's HA
container has a restart policy that lets it come back from the dead.

## MQTT as the shared bus

The broker has moved hosts more than once as the fleet's hardware roles
shifted. The recurring lesson isn't about MQTT specifically — it's that any
service treated as fleet-wide infrastructure needs **exactly one** place
that's authoritative for "where is it right now," or every consumer config
drifts independently and you end up debugging "why isn't this alerting"
issues that are actually just a stale hostname/IP in a forgotten config
file. Worth building a health-check into the deploy process rather than
discovering staleness node-by-node.
