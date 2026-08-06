# Running the fleet's local LLM on a repurposed phone

## The pattern in one sentence

A retired flagship phone with a real Linux userland is an 8-core, 8GB
always-on inference box — so the fleet's AI muscle (a 7B language model that
several tools depend on) runs *there*, and every small board that needs an
answer makes a network call instead of trying to host a model it can't fit.

This is the companion to [ZAP-OFFLOAD.md](ZAP-OFFLOAD.md): same "small box
drives a heavy thing on a capable box" shape, same capable box (**the-one**,
a repurposed OnePlus 6T), but the heavy thing here is *inference* rather than
a scanner. It also carries an honest field note about a GPU idea that **did
not work out**, because the parts that fail are worth writing down too.

## Why this came up

The fleet's scan/agent controller ("Ragnar") wants a language model behind
it — to summarize recon, reason about findings, and drive a conversational
voice assistant. A useful-quality model for security reasoning is ~7B
parameters, which at Q4 is a ~4.7GB weights file and wants that much RAM
resident plus headroom. None of the small boards can hold it:

- **the Pi 4B** (the scan controller): ~3.7GB — can't fit the 7B at all.
- **the laptop hub**: 8GB on paper, but most of it is already spoken for by
  the home-automation stack + network hub role.
- **the Orange Pi / Zero-class boxes**: far too small.

So the model lives on the one box in the fleet with genuine RAM headroom that
isn't already committed to something else — **a phone.**

## The host: an 8GB phone that thinks it's a server

**the-one** is a retired OnePlus 6T running a full ARM64 Linux userland
(Kali/NetHunter): 8 cores, ~7.7GB RAM, Adreno 630 GPU, and — the part that
matters — always-ish on and on the mesh VPN. It runs a standard local
inference server (Ollama) hosting two models:

- a **7B** model — the "real" model, used by the pentest/agent Ragnar for
  security reasoning.
- a **3B** model — a fast, low-latency model for the household voice
  assistant, where response *speed* matters more than depth.

The same realization as the ZAP write-up applies, just aimed at inference:
**a "phone" with 8GB and a real Linux userland is a better model host than a
4GB Pi**, even though the Pi is the "real computer." Repurposed flagship
phones are an underrated source of capable, low-power, always-on compute — a
detail phone you already own beats buying a mini-PC for a lot of home-lab
jobs.

## Part 1 — one model resident at a time (the RAM interlock)

8GB fits the 7B *or* the 3B comfortably, but not both loaded at once alongside
everything else the phone does. Two guardrails keep it honest:

- **Pin the loaded-model count to one** and give it a short keep-alive.
  Loading a second model **evicts** the first, so a voice request (3B) and an
  agent request (7B) don't try to co-reside and OOM the box. The cost: the
  first request after a model switch pays a reload (~tens of seconds for the
  7B on this hardware). Fine for one-caller-at-a-time; see the caveat below.
- **A hard kill exists** — stopping the inference service frees all of it —
  because the phone shares RAM with other duties (it's also the heavy-scanner
  host from the sister doc) and sometimes another job needs the room.

## Part 2 — pointing the small boxes at it

Every small box that wants an answer is a *client*: it makes an HTTP call to
the phone's inference endpoint over the mesh VPN. The controller code talks a
standard OpenAI-compatible client to `http://<host>:<port>/v1`, names the
model, and gets a completion back. No model, no weights, no GPU on the small
box — just a socket.

Two failure modes here cost real time and are the actual lesson of this doc,
because **both fail silently:**

1. **Address the host by IP, not by name.** The controller was configured
   with the *bare hostname* of the model host. That name doesn't resolve from
   the calling box (no shared DNS/hosts entry across these boxes), so every
   inference call failed name resolution. Use the host's **mesh-VPN IP** —
   stable across the phone roaming between its home WiFi and cellular, which a
   local-subnet IP is not. And put that IP in *both* the runtime config
   **and** the code's hardcoded fallback: leaving a bare hostname in the
   fallback is a landmine that silently re-breaks inference the moment the
   config is lost or the service is reinstalled.
2. **A swallowed exception hides a dead model for weeks.** The AI call sits
   inside a `try/except` that logs and moves on, so when every call was
   failing DNS, *nothing surfaced it* — the dashboard looked perfectly
   healthy while the AI had been dead the whole time. Wrap the call defensive
   if you must, but make the failure **visible** (a health probe that
   actually exercises the real code path, not just a port check). "The port
   is open" is not "the model answers."

The fix for both is verified the only way that counts: run the real code path
(OpenAI client → `/v1` → the 7B) end-to-end and read back an actual
completion — not just confirm the config value or that the port accepts a
connection.

## Part 3 — making the local-vs-cloud switch conflict-free upstream

The controller is a vendored copy of an upstream project whose AI defaults to a
**cloud** model, and whose updater does `git reset --hard origin/main` — which
discards any local edit in the tree. Pointing it at the local model instead has
to survive that reset and must not diverge from upstream for anyone who *doesn't*
want local. Same durable pattern as the sister [ZAP offload](ZAP-OFFLOAD.md):

- **One env var is the whole gate.** If `OLLAMA_HOST` (or your equivalent) is
  set, the AI client's base URL is overridden to the local inference endpoint
  (`.../v1`) with a throwaway key; if it's **unset, the code path is byte-for-byte
  upstream** — cloud model, cloud key, nothing changed. A single `if` at the one
  place the client is constructed. That means the change is *upstreamable*: it
  adds a capability and changes no default.
- **Toggle it from outside the code.** Because the gate is an env var, switching
  the whole controller between local and cloud is renaming a systemd drop-in
  (present = local, absent = cloud) and restarting — no edit to the tree, no
  redeploy. The target is the model host's **stable mesh-VPN IP**, so the switch
  keeps working across the phone roaming networks.
- **The edit lives in a patch *outside* the repo, re-applied on every start.**
  A tiny `ExecStartPre` hook walks a patch directory under `/usr/local/...` and,
  for each patch, re-applies it only if it isn't already applied
  (`git apply --reverse --check`), only if it lands clean (`git apply --check`
  first), and **never fatally** (a patch upstream has outgrown is logged loudly
  and skipped — it can't block startup). So `git reset --hard` wipes the tree,
  the next start silently restores the patch, and the day upstream makes the AI
  endpoint configurable for real, the patch stops applying and you delete it.

Net result: the local-model wiring has **zero standing conflict with upstream** —
it's an opt-in, reset-proof patch, not a fork. (It also injects a one-line
"runtime facts" system prompt so the model answers honestly about whether it's
the local or the cloud model instead of confabulating its own stack — a small
fix for a real "wait, which model am I even talking to" confusion.)

## Part 4 — the GPU offload that didn't pan out (honest field note)

The obvious next win: the phone's inference is **CPU-only**, pegging all 8
cores while the Adreno 630 GPU sits idle. GPU offload should be a big speedup.
It isn't — and this is worth recording because "we tried the clever thing and
it lost" is a real result.

Measured on-device, same weights file the server uses, apples-to-apples
(`llama-bench`, prefill/gen tok/s):

| Config | prefill tok/s | gen tok/s | outcome |
|--------|---------------|-----------|---------|
| **CPU** (8 threads) | 2.74 | 1.82 | works; matches the real-world pipeline |
| **GPU** (Vulkan/Turnip, all layers) | — | — | **model won't load** |

The GPU path doesn't just underperform — it **fails to load the model at
all.** The open-source Vulkan driver for this GPU (Turnip on the Adreno 630)
reports `fp16:0` and **no 16-bit storage support**, and the inference
backend's Vulkan path requires 16-bit storage. Load aborts. Worse, merely
*having* the Vulkan backend present broke the CPU run too, until the GPU
device was explicitly hidden from it.

**Verdict: GPU offload is not viable on this GPU.** Not a config problem — a
hardware/driver feature gap. This is the known "hit-or-miss on old mobile
GPUs" risk landing squarely on *miss*. The takeaway isn't "phones can't do
GPU inference," it's **benchmark the specific chip before you build around it;
don't assume an idle GPU is free performance.** Inference stays on CPU. GPU/NPU
offload is parked for a future dedicated host whose accelerator actually
supports fp16.

## The ceiling (be honest about it)

This works well for **one caller at a time**, which is the real usage. It has
two structural limits worth stating plainly, because they're where this design
stops scaling:

1. **One model resident** means concurrent callers — a voice request and an
   agent request arriving together — **evict each other and thrash.** The
   interlock that protects the RAM is the same thing that serializes the work.
2. **One 8GB box** is both a single point of failure and the ceiling. All the
   AI muscle lives on one phone; if it roams off the network or is repurposed,
   the fleet's AI goes with it.

The real fix for both is a **dedicated inference host** with enough RAM to
keep more than one model resident (and, ideally, an accelerator that supports
fp16 so Part 3 comes out the other way). Until that box exists, the phone is
the pragmatic answer — and knowing exactly *why* it won't scale is what tells
you when you've outgrown it.

## Replicating it

You don't need this hardware — you need the shape:

- **Clients** you want to keep on small/cheap always-on boxes.
- **A model** those boxes can't host.
- **A capable box already on your management network** — a repurposed
  flagship phone with a Linux userland, a spare mini-PC, an old laptop.

Then: run a standard inference server on the capable box, address it by a
**stable management-network IP** (not a bare hostname, not a roaming local
IP), point every small box's client at that endpoint, pin the resident-model
count to fit the RAM, and make the client's failure path **loud** — a health
check that runs the real inference call, so a dead model can't hide behind an
open port. Benchmark any GPU/accelerator on the *actual* chip before you
count on it.

## Status

Working. The 7B model runs on the repurposed phone; the Pi-class controller
and the voice assistant both reach it over the mesh VPN and get real
completions (verified end-to-end: the controller's own client, over the mesh
VPN, gets a live `200` from the model host). The local-vs-cloud switch is the
opt-in, reset-proof patch from Part 3 — so the controller can run on the local
model or fall back to a cloud one with a single toggle and no divergence from
upstream; which one is live at any moment is a deployment choice, not a code
change. The resident-model interlock keeps the phone inside its RAM budget.
GPU offload was measured and rejected (driver feature gap) — CPU is the shipped
path. Open items — chiefly the eventual dedicated inference host — live in
[OPEN-QUESTIONS.md](OPEN-QUESTIONS.md).

## Scope note

Host-specific addresses, ports, and model names here are genericized —
placeholders, not live connection details. See the repo README's scope note.
Related: **[ARCHITECTURE.md](ARCHITECTURE.md)** for the mesh/AP split this
rides on, **[ZAP-OFFLOAD.md](ZAP-OFFLOAD.md)** for the sister offload pattern
on the same host, and **[LESSONS.md](LESSONS.md)** for the silent-failure
incidents referenced above.
