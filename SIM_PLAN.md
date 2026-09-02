# SIM_PLAN.md

A Monte Carlo goldfishing simulator for one cEDH Kinnan deck. Design document.
No code exists yet.

**Status:** design agreed, deck received and resolved (§3), nothing blocked.
Ready to start Phase 0 (§15).

---

## 1. What this is, in one paragraph

A C++ library that plays one Commander deck against no opponents, thousands of
times, and returns the distribution of *which turn the deck assembles a declared
win pattern, and which pattern it was*. It is the value function for a later
mulligan solver, so its interface takes a specific opening hand and returns a
value. It does not know what a mulligan is. It has no opponents, no stack, no
interaction, and no general rules engine, and every one of those absences is a
deliberate, stated limit rather than an unfinished feature.

---

## 2. Verified findings about the data source

These were checked against the live database on 2026-09-01, not against the
documentation. Four are contract defects in the ingestion repo (§2.1, §2.3,
§2.7 — and §2.7 is the one that silently corrupts a deck). Four are modelling
consequences that change the design (§2.4, §2.5, §2.6, §2.8).

### 2.1 `mtg_v1.card_face` did not exist — FIXED in Phase 0

`PLAN.md` §3.8 of the ingestion repo lists it in the contract view inventory.
`sql/repeatable/R__010_views_v1.sql` creates four views: `card`,
`card_any_medium`, `card_non_gameplay`, `card_legality`. There is no
`card_face`. The physical table `mtg_internal.card_face` exists from migration
`0002` and holds **6,429 rows**, but `mtg_consumer` has `USAGE = false` on
`mtg_internal`, so a consumer cannot reach it at all.

This is the same bug class as `card_legality`: listed in the inventory, never
created, discovered when a real consumer needed it.

**This is an upstream fix, not a workaround here.** In the ingestion repo:

1. Add `mtg_v1.card_face` to `R__010_views_v1.sql` exposing `oracle_id`,
   `face_index`, `name`, `mana_cost`, `face_mana_value`, `type_line`,
   `oracle_text`, `colors`, `power`, `toughness` — everything but `raw`.
2. Update the exact-set assertion in the migration test. A new view in `mtg_v1`
   is a contract change and *should* fail that test until acknowledged.
3. Regenerate `sql/schema.snapshot.sql`.
4. Re-apply grants after the view (the repo already knows `CREATE OR REPLACE
   VIEW` drops them).
5. **Add an inventory-drift test.** §3.8's view inventory has now been wrong
   twice in the same direction, and the existing exact-set assertion cannot
   catch it: it tests *what `mtg_v1` contains*, so it catches views added to the
   schema but never views promised by the document and never built. The new test
   parses §3.8's table out of `PLAN.md` and asserts it equals the set of views
   `R__010_views_v1.sql` creates — failing in **both** directions. This is the
   third contract gap found by a consumer rather than by CI, and it should be
   the last one that can be found this way.

**Done, 2026-09-01** (`d67a1b2` upstream). The view exposes every `card_face`
column but `raw`, 6,429 rows, joined against `card` to respect `deleted_at` the
way `card_legality` does; `mtg_consumer` reads it through the existing default
privileges. §3.8's table gained a `Status` column (`live` / `planned`) and
`test_plan_inventory_matches_the_views_file` now asserts
`{live rows} == {views R__010 creates}` in both directions — verified to reject
all four failure modes rather than assumed to. 392 tests pass, lint and mypy
clean, snapshot regenerated.

It paid for itself the same hour: the first query against it resolved
*Invasion of Ikoria* (§4.6), which both of us had guessed wrong.

### 2.2 Why that view is load-bearing

Without it, the contract gives essentially nothing for multi-faced cards:

| layout | cards | `mana_cost` empty | `mana_cost` is `"A // B"` |
|---|---|---|---|
| transform | 393 | **393** | 0 |
| modal_dfc | 98 | **98** | 0 |
| adventure | 158 | 0 | 152 |
| split | 135 | 0 | 135 |
| prepare | 52 | 0 | 52 |
| flip | 26 | 5 | 0 |

And **862 of 864** multi-faced cards have empty `oracle_text` in `mtg_v1.card`.

Concretely, for *Bala Ged Recovery // Bala Ged Sanctuary* the contract yields
`mana_value = 3`, `castable_cmcs = {3}`, `has_land_face = t` — and **not** the
`{2}{G}` pip requirement, not which face is the land, not that the land enters
tapped, and not what colour it taps for. A cEDH Kinnan list runs several MDFCs.
Any colour-aware mana model needs `card_face`.

#### The same warning, landing on X spells — 7 of the 99

The ingestion README warns that `mana_value` distorts a curve and that
`castable_cmcs` is the column to use instead. **For X spells `castable_cmcs` is
technically correct and practically meaningless**, and this deck has seven:

| Card | Cost | `castable_cmcs` | What it does at that cost |
|---|---|---|---|
| Chord of Calling | `{X}{G}{G}{G}` | `{3}` | finds a creature with MV ≤ 0 — nothing |
| Finale of Devastation | `{X}{G}{G}` | `{2}` | creature MV ≤ 0 — nothing |
| Nature's Rhythm | `{X}{G}{G}` | `{2}` | creature MV ≤ 0 — nothing |
| Invasion of Ikoria | `{X}{G}{G}` | `{2}` | non-Human MV ≤ 0 — nothing |
| Disrupting Shoal | `{X}{U}{U}` | `{2}` | counters a spell of MV 0 |
| Wan Shi Tong | `{X}{U}{U}` | `{2}` | 0 counters, draws 0 cards |
| **Mockingbird** | `{X}{U}` | `{1}` | **functional** — copies a 1-drop dork |

**Six of seven are castable at a cost where the card does nothing.** The column
is not wrong — X=0 genuinely is a legal cost — but "the earliest slot this card
can be played into" is the wrong question for a spell whose whole purpose is
scaling. *Castable at 2 and useless at 2* is precisely the distortion
`castable_cmcs` exists to prevent, reappearing one level down.

Two consequences, both load-bearing:

1. **The exporter must not treat `castable_cmcs` as the cost.** A cost with an
   `{X}` is exported as a *variable* cost: fixed pips plus a free variable. Any
   model reading `min(castable_cmcs)` here would let the deck cast *Finale of
   Devastation* on turn two and find nothing — a play that is legal, useless,
   and would look like the deck functioning.
2. **Choosing X is a policy decision, not a cost lookup** (§6). "How much X do I
   want, and can I afford it?" belongs to `Policy`, and for a `TUTOR` the answer
   is driven by the mana value of the target the scorer picked. This is the
   first place the mana system (§6.4) and the scorer (§6.2) genuinely have to
   talk to each other, and Phase 2 should build `can_pay` with a variable
   component from the start rather than retrofitting one.

*Chord of Calling* additionally has **convoke**, which pays part of its cost by
tapping creatures — interacting with both the mana system and Kinnan (a tapped
creature is not producing mana). It is the single most complex cost in the deck
and worth authoring last.

**No human noticed this from the schema; it took reading seven oracle texts.**
The contract cannot flag it, because from the database's side nothing is wrong.

### 2.3 Names are stored in combined form

`mtg_v1.card.name` for an MDFC is `Bala Ged Recovery // Bala Ged Sanctuary`.
Decklists write the front face. `mtg_internal.card_name_index` — designed in
`PLAN.md` §3.7 with a `kind` of `full | face` precisely to solve this — **does
not exist**; there is no migration `0004`. Until it does, the exporter builds
its own front-face index by splitting on `" // "`. Cheap, and it belongs in the
exporter anyway since resolution failures must surface at export time (§9.3).

### 2.4 Your performance estimate is high by one to two orders of magnitude

C(99,2) = **4,851** pairs, not "~4,800 pairs" needing billions of games. At
50,000 games per pair that is 243M games, plus 99 × 50,000 = 5M for the
leave-one-out sweep. Call it **250M games**, not billions.

Your hardware is a **base M1: 4 performance cores, 4 efficiency cores, 8 GB
RAM** — less than "Apple Silicon Mac mini" implies. Even so, at a pessimistic
20 µs/game across 4 P-cores the full pairwise sweep is ~21 minutes. At a
realistic 5 µs/game it is five minutes.

**What this changes:** C++ is still correct — this workload is 100–1000× beyond
comfortable Python, and the mulligan solver on top multiplies it again. But you
have roughly 10× more budget than you planned for. Spend it on a design you can
verify, not on bit-packing you cannot debug. Specifically: §10.4's variance
reduction will buy you more than any micro-optimisation, and it costs nothing at
runtime.

### 2.5 Two premium cards are worth exactly zero in this model

From the live oracle text:

- **Rhystic Study** — "Whenever *an opponent* casts a spell…"
- **Mystic Remora** — "Whenever *an opponent* casts a noncreature spell…"

With no opponents these draw **zero cards, ever**. They are not approximated;
they are correctly inert, and they cost mana to deploy. This is the cleanest
illustration of what goldfishing measures and what it does not, and the run
report should say so out loud (§9.5).

**What this implies beyond v1.** Two of the format's strongest cards are not
weak in this model, they are *structurally invisible* — their entire text
addresses a game object the model does not have. That is concrete evidence for a
general claim: **opposition profiles are not a later refinement to this design,
they are load-bearing.** A goldfish measures how fast a deck can go unimpeded,
which is a real and useful quantity, but it is an upper bound and it
systematically misvalues an entire class of cEDH card — taxation, stax, and
opponent-triggered card advantage all read as zero here.

Out of scope for v1 and not designed here. But it means the v1 numbers should be
labelled as *goldfish speed*, never as *deck strength*, and it argues against
ever letting the ablation output be read as a card-quality ranking without that
caveat attached. §9.5's header is where that caveat lives.

### 2.6 Kinnan + Basalt Monolith makes infinite *colorless* mana

Kinnan reads "Whenever you tap a nonland permanent for mana, add one mana of any
type that permanent produced" — an **additive +1 of an already-produced type**,
not a doubler. Basalt Monolith taps for `{C}{C}{C}`, Kinnan adds `{C}` → 4;
`{3}` untaps it → **net +1 colorless per iteration**.

So the engine produces unbounded `{C}` and *no coloured mana*. Kinnan's own
outlet costs `{5}{G}{U}`, which the loop cannot pay for by itself. **Mana flags
in win patterns must therefore be typed** (`INFINITE:C`), not boolean. A boolean
`INFINITE_MANA` flag would silently let the deck cast anything, and would
overstate it on exactly the hands you most want to trust.


### 2.7 `mtg_v1.card` silently drops 254 Reserved List cards

**Contract gap #4, and the worst-behaved of the four.** Five cards in the Kinnan
list do not resolve in `mtg_v1.card` at all:

| Card | `games` | `rep_set_code` |
|---|---|---|
| Copy Artifact | `{mtgo}` | `me4` |
| Lotus Petal | `{mtgo}` | `tpr` |
| Mox Diamond | `{mtgo}` | `tpr` |
| Transmute Artifact | `{mtgo}` | `me4` |
| Tropical Island | `{mtgo}` | `vma` |

All five are unambiguously paper cards. Their *representative printing* happens
to be from an MTGO-only reprint set (Masters Edition, Vintage Masters, Tempest
Remastered), so `games = {mtgo}` and the paper filter on `mtg_v1.card` excludes
them.

**Root cause: `games` is a `rep_`-scoped column without the `rep_` prefix.** The
README is explicit that `rep_*` columns "describe *that printing, not the card*"
— `games` has exactly that property and none of the warning.

**Proof it is a defect, not a policy:** **254 Reserved List cards are excluded
from `mtg_v1.card`**, including *Ancestral Recall*, *Aluren*, and *Tropical
Island*. The Reserved List is a paper-only promise; a Reserved card that is not
a paper card cannot exist.

Why this one is the most dangerous: it does not error. It silently drops cards,
and it is **biased toward exactly the old, powerful staples a cEDH deck plays** —
5 of 99 here. A deck loader trusting `card` would build a 94-card deck and report
success.

**Consumer-side fix (immediate):** the exporter joins **`card_any_medium`**, per
the README's existing advice. All 99 resolve there. This is already the plan
(§8.1) and needs no upstream change to unblock.

**Upstream fix (should still happen):** rename to `rep_games`, or make `card`'s
paper filter consider all printings rather than the representative one. Until
then the README's stated reason for the `card`/`card_any_medium` split — "a
legitimately owned Arena-only card shows up as unresolved" — understates the
problem: `card` also drops paper cards, which the documentation does not warn
about.

### 2.8 Opponent-dependence reaches into the mana base

§2.5 said two premium *card-advantage* cards read as zero. The real list shows
the problem is worse and more structural — it reaches the mana:

| Card | Text | Value at 0 opponents |
|---|---|---|
| Exotic Orchard | "any color that a land **an opponent** controls could produce" | **Produces nothing** |
| Fellwar Stone | same | **Produces nothing** |
| Rejuvenating Springs | "enters tapped **unless you have two or more opponents**" | **Always enters tapped** |
| Gemstone Caverns | "if in your opening hand and **you're not the starting player**" | **Free-play never applies**; taps for `{C}` only |

Two mana sources produce zero mana, and a land that is untapped in every real
game is always tapped here. These are not card-quality misvaluations; they are
**errors in the part of the model that is supposed to be most trustworthy.**

Three distinct failure modes, all from the same hardcoded zero:

1. **Produces nothing** — Exotic Orchard, Fellwar Stone
2. **Enters tapped when it would not** — Rejuvenating Springs
3. **Draws nothing** — Rhystic Study, Mystic Remora, Faerie Mastermind,
   Consecrated Sphinx, Wan Shi Tong, The Cabbage Merchant
4. **A free opening-hand play never triggers** — Gemstone Caverns

**Number 4 is the dangerous one, and it is worse than the other three.** Errors
1–3 are diffuse: they shift mana availability a little, across all games, and
they blur into the distribution's width. This one does not average out.

*Gemstone Caverns* is an **opening-hand card**, and the mulligan solver's entire
job is evaluating opening hands. The error lands directly on `K(H, n)` for every
hand `H` containing it — a systematic, one-directional undervaluation, not noise
that cancels over 50,000 games. **P(Caverns in an opening seven) = 7/99 ≈ 7.1%**,
so roughly one hand in fourteen carries a value that is wrong in a fixed
direction, and the keep/mull chart is built out of exactly those numbers.

**`on_the_play` is itself a bias, and its direction is stated.** In a four-player
pod you are on the play **25%** of the time. Neither setting is neutral:

| Setting | Caverns | Bias |
|---|---|---|
| `on_the_play = true` | free-play never applies | **Undervalues** Caverns hands; matches reality 25% of the time |
| `on_the_play = false` | free-play always applies | **Overvalues** Caverns hands; matches reality 75% of the time |

Neither is right, because the honest answer is a mixture the model cannot
represent without simulating seat order. This deck is fast enough that a
turn-zero land is worth roughly a full turn on the hands that have it, so the
gap between the two settings is not academic.

**Therefore `on_the_play` is required, never defaulted** — the same rule as
`opponents`, for the same reason. A default here would silently pick one of two
known-wrong answers and hide that a choice was made. Running both and reporting
the spread is the honest treatment, and it is cheap: two runs, one number each.

The proper fix is seat-order simulation, which is opposition-profile work and
out of scope (§2.5). Until then this is a **declared, measured** distortion
rather than an invisible one.

**Design response, and it is cheap:** the deck file declares an opponent
*context*, not an opponent *model*:

```toml
[table]
opponents    = 3                        # a 4-player pod
on_the_play  = true                     # REQUIRED, no default (see below)
opponent_colors = ["W","U","B","R","G"]  # what Exotic Orchard / Fellwar Stone see
```

`opponents` is read **only** by cards whose text tests opponent count — it fixes
Rejuvenating Springs exactly, for free. `opponent_colors` is a stated assumption
that makes Exotic Orchard and Fellwar Stone produce mana; in cEDH, opponents
playing duals and fetches means "any colour" is closer to true than "nothing."

This is **not** an opposition model and does not simulate opponents. It is the
same philosophy as §9.4's replacement card: the assumption is declared, printed
in the honesty header, and changeable, so a result that depends on it can be
tested by changing it. Setting `opponent_colors = []` recovers the pessimistic
reading and the difference between the two runs *measures the deck's exposure to
this assumption*.

---

## 3. The deck

**Received and resolved.** 99 mainboard + *Kinnan, Bonder Prodigy* = 100. All 99
resolve against `mtg_v1.card_any_medium`; 97 by exact name, 2 by front-face
split (§2.3). Colour identity is within `{G,U}` for every card. Measured
composition is in §4.5; it replaces the projection that used to be there.

Nothing is blocked. §12's scaffolding and Phase 0 can start now.

---

## 4. The card/effect model

### 4.1 Limits, stated first

This model **cannot express**, and will not try to:

- The stack, priority, holding priority, or responding to anything
- Opponents, their permanents, their life totals, their spells
- Combat, damage, or the attack step
- Targeting restrictions, hexproof, protection, legality of targets
- Replacement effects, layers, or timestamp ordering
- State-based actions beyond "creature with 0 toughness dies" (unused in v1)
- Graveyard recursion loops, storm count, cascade
- Any card whose value depends on an opponent acting

It **can express** exactly the archetypes in §4.2 and nothing else. A card that
is not one of those is `inert` or the build fails (§4.4).

#### The recommended default objective: P(assembled by turn 3)

This model is a *value function* (§1), so something has to choose which number
out of §10's output is the value. **That choice is now made, on evidence, rather
than left to whoever wires up the solver.**

> **The default objective is an early-turn CDF point — `P(assembled by turn 3)`
> for this deck — not turn 12, not the censored fraction, and not a mean.**

The reasoning is §16.4, measured rather than assumed. Fixing the mana payment
bug moved the curve like this:

| | turn 3 | turn 4 | turn 6 | turn 12 |
|---|---|---|---|---|
| before | 18.27% | 26.87% | 39.72% | 67.25% |
| after | 8.71% | 19.59% | 37.01% | 66.65% |

**Turn 3 halved. Turn 12 moved by six-tenths of a point.** A bug that handed the
deck unlimited free mana was nearly invisible at the tail, because by turn 12
this deck has enough real mana anyway and free mana only changes *when*. The
tail is not a weaker signal — **it is a different question**, and mostly asks
"does this deck ever get there", which almost every keepable hand answers yes to.

Three consequences for the solver, and they are the point of stating this here
rather than in §10:

1. **A keep/mull chart is a chart of whatever objective it was given.** Scored on
   turn 12, most seven-card hands look alike and the chart says keep almost
   everything — not because that is right, but because the objective cannot
   distinguish them. Scored on turn 3 the same hands separate, because turn 3 is
   what an opening hand actually determines.
2. **Sensitivity to the model's own errors follows the same curve.** An objective
   at turn 3 is the one that moves when the model is wrong, which cuts both
   ways: it is the honest place to read a difference, and the place a modelling
   bug does the most damage. Both are arguments for reading it, not for
   avoiding it.
3. **The turn is a per-deck parameter, not a constant.** Three is right for a
   list whose turn-3 assembly rate is ~9% and whose curve is steepest at turns
   3–5. A slower deck's discriminating turn is later. Pick it by looking at
   where the CDF is steepest, and **say which turn the number is**, since
   "P(assembled)" without a turn is not a quantity.

A mean is excluded outright and for a separate reason: with a third of games
censored, the mean over the games that finished is not the mean of anything
(§10.3), and it biases optimistically.

#### THE BLIND SPOT this objective has, and it is not a trade-off

An early-turn objective **cannot see a mid-game engine piece.** This is stated
separately from the reasoning above because it is a different kind of warning: a
trade-off is something a reader weighs, and this is something the number is
structurally unable to represent.

Measured (§16.5), the same card at three objectives:

| Card | turn 3 | turn 6 | turn 12 |
|---|---|---|---|
| **Basalt Monolith** | **−0.61%** | +1.09% | **+5.58%** |
| Enduring Vitality | +7.28% | +24.98% | +30.50% |
| Force of Will (inert, a control) | −0.63% | −1.23% | −0.31% |

*Basalt Monolith* is **half of the deck's primary engine** — the `kinnan_basalt`
line that produces unbounded colourless mana, and the reason *Thrasios* is in the
99 at all. At turn 3 it scores **indistinguishably from a card declared to do
nothing**, and slightly worse. That is not an error. It is colourless, it does
not untap, and casting it on turn three buys nothing on turn three. By turn 12 it
is worth +5.6.

> **A keep/mull chart built on `P(assembled by turn 3)` ranks cards by how fast
> they get you there, and a card whose entire contribution arrives on turn six
> registers as a blank. The chart is not wrong. It is answering a question in
> which that card does not appear.**

Three consequences that a reader of such a chart has to be told, not left to
infer:

1. **Do not read a low score as "this card is bad."** Read it as "this card does
   not act by turn N." Those coincide for a genuinely weak card and diverge
   completely for a slow engine piece, and the chart cannot distinguish them.
2. **Never derive a decklist change from a single-objective sweep.** Cutting the
   cards at the bottom of a turn-3 table would cut *Basalt Monolith*, which is
   half the engine. Run the sweep at two objectives before touching a list; the
   §16.5 tables print the turn in the header for exactly this reason.
3. **The blind spot is defensible for a MULLIGAN chart specifically**, which is
   what this tool is for (§1). An opening-hand decision is a bet on the early
   game, and a card that acts on turn six is genuinely worth less *to that
   decision* than to the deck. The warning is against carrying the same number
   to a different question.

The honest summary is that this objective is right for the tool's stated purpose
and wrong for the question people will reach for it to answer next.

### 4.2 The effect kinds — a closed, enumerated set

**This list is closed.** It is derived from reading all 99 oracle texts, not
accreted while authoring. Every count below is the number of cards in *this*
deck that use the kind, so the set is fitted to a corpus rather than to
imagination.

| # | Kind | One-line definition | Cards |
|---|---|---|---|
| 1 | `MANA_SOURCE` | A permanent that taps for a fixed, known quantity of mana. | **30** |
| 2 | `DYNAMIC_MANA_SOURCE` | Taps for mana whose amount or colours are a function of game state. | 6 |
| 3 | `RITUAL` | One-shot mana that does not come from tapping a battlefield permanent. | 2 |
| 4 | `SELF_UNTAP` | Pay a cost to untap this permanent. | 3 |
| 5 | `UNTAP` | Untap a set of other permanents, on resolution or on a trigger. | 3 |
| 6 | `DRAW` | Put N cards from library into hand, N possibly state-dependent. | 6 |
| 7 | `SELECT` | Look at N cards, choose K by a policy decision, dispose of the rest. | 2 |
| 8 | `TUTOR` | Search the library by a predicate, move the result to a named zone. | 9 |
| 9 | `FETCH` | Sacrifice for a land from the library; shuffles, and costs the land drop. | 7 |
| 10 | `CLONE` | Enter as, or become, a copy of a permanent already on the battlefield. | 8 |
| 11 | `STATIC_MANA_MODIFIER` | While on the battlefield, changes how the mana system itself behaves. | 2 |
| 12 | `CARD_COST` | An entry cost paid in cards from hand rather than in mana. | 2 |
| — | `INERT` | Declared to do nothing in this model, with a required `reason`. | **28** |

Twelve kinds, and as of Phase 0 **no card is undecided** — *Invasion of Ikoria* resolved to `TUTOR` once §2.1 landed (§4.6).

**Dropped from the first draft, because this deck has zero of them:**
`COST_REDUCTION`, `EXTRA_LAND_DROP`, and `MANA_MULTIPLIER` as a standalone kind
(folded into `STATIC_MANA_MODIFIER`). A kind with no user is speculation, and
speculation is what makes a set open.

**Added, and it was a real miss in the first draft:** `CLONE` at 8 cards is the
second-largest kind and was entirely absent from the original archetype list.
This is a clone deck — *Clever Impersonator*, *Copy Artifact*, *Copy
Enchantment*, *Mirrormade*, *Flesh Duplicate*, *Mockingbird*, *Flash
Photography*, *Mirage Mirror* — and in a Kinnan shell a clone's usual job is to
copy a mana source. Designing the effect model without reading the list would
have shipped a set missing its second-biggest member.

> **This is why the kinds are deck-derived, not format-derived.** A taxonomy
> built from what Magic *contains* would have produced a plausible, balanced,
> general-looking set — and `CLONE` would have been a minor entry near the
> bottom, if present at all, because clones are a small slice of the format. In
> *this* deck they are 8 cards and copy the mana engine.
>
> The general set would not have been wrong about Magic. It would have been
> wrong about the only deck the tool models, in a way no amount of reasoning
> about Magic would have surfaced — only reading 99 oracle texts did. **A
> taxonomy fitted to a corpus can be checked against that corpus; one fitted to
> a domain can only be checked against opinion.** Every count in the table above
> is falsifiable by re-running a query.

#### The rule for a new kind versus a flag

> **A new kind is justified only when the game loop needs a verb it does not
> already have. Anything that changes *when*, *how much*, or *under what
> condition* an existing verb fires is a flag.**

Three questions, in order. The first "yes" decides it:

1. Must the loop do something it currently cannot? → **kind**
2. Does it modify an existing verb's timing, quantity, or precondition? → **flag**
3. Is it expressible as an existing kind plus at most two flags? → **flag**

*Worked:* `SELF_UNTAP` is a kind — untapping a permanent mid-turn for a cost is a
verb the loop otherwise lacks. `untaps_normally = false` on Basalt Monolith is a
flag — the untap step already exists, this changes whether it applies.

**Governance, per your requirement:** adding a kind after Phase 7 begins is an
explicit decision that **re-audits every already-authored card** against the new
kind, recorded in this section with the reason. It is never a quiet append. The
failure being prevented is real: if kinds accrete, card 60 gets a kind card 12
should have used, nobody backfills, and **the ablation numbers then depend on
authoring order** — a bias with no symptom.

#### The two diagnostics you asked for

> **A representation can remove a policy question instead of answering it.**
>
> Two instances, and they are the same move:
>
> | Question | Naive form | What the representation did |
> |---|---|---|
> | "Does Kinnan multiply this?" | a branch in the policy | `is_land` on the Source, read by the mana system |
> | "Should I pay 2 life for an untapped Breeding Pool?" | a scorer term weighing life against tempo | a `life_floor`, so an unaffordable source is filtered out at collection |
>
> Neither question is answered anywhere. Both stopped being questions. **A source
> you cannot afford is not a source**, so `can_pay` never learns about life and
> the scorer never weighs it — life cost ~60 lines and no policy surface at all.
>
> The general form: before adding a term to a decision function, check whether
> the decision can be made not to arise. A filter upstream is cheaper than a
> term downstream, it cannot be mis-weighted against other terms (the magnitude
> mismatch in §6.2), and it needs no tuning. When it does not apply, the
> decision was real and belongs in the policy — but it is worth asking first.

**`MANA_SOURCE` has 30 users — is it hiding a distinction?** Partly, and here is
the honest account. It carries five flags: `produces`, `taps`, `is_land`,
`is_creature`, `enters_tapped`. Four are inert to the loop — it taps a thing and
gets mana either way. **`is_land` is not inert**: Kinnan multiplies *nonland*
permanents only, so that one flag is the difference between the deck's engine
working and not. It stays a flag because the loop does not branch on it (the
mana system reads it), but it is the single field in the model most worth a
dedicated test.

Where the group genuinely strains is `enters_tapped`, which is not a boolean but
a per-card predicate: *Botanical Sanctum* (≤2 other lands), *Breeding Pool* (pay
2 life), *Rejuvenating Springs* (≥2 opponents, §2.8). Three cards, three
conditions.

That observation generalises, and the threshold is declared **now** rather than
judged later:

> **RULE K1 — a flag carrying per-card predicate logic is a kind wearing a
> flag's clothes.**
>
> A flag is a *value*: a boolean, a number, a member of a fixed enum. The moment
> a flag's value is a *condition evaluated against game state*, it has stopped
> being a parameter and started being behaviour, and behaviour hidden inside a
> parameter is the exact accretion the closed set exists to prevent.
>
> **Threshold, fixed in advance: at the fourth distinct predicate shape, the
> flag is promoted** — either to its own kind, or to a named predicate
> vocabulary with its own closed enumeration. Not "when it feels messy."
>
> Current standings: `enters_tapped` is at **3 of 4** (`land_count`, `life_
> payment`, `opponent_count`). It is the only flag anywhere near the threshold.
> A fourth shape promotes it, and that promotion re-audits every `MANA_SOURCE`
> card exactly as a new kind would (§4.2 governance).

Declaring the threshold in advance is the point. A judgement call made at the
moment of pain is made by someone who wants to keep authoring, and they will
find the fourth predicate reasonable.

**Kinds with only 2 users — wrong, or really flags?** Four qualify, and each is
defended by question 1 above, not by convenience:

- `RITUAL` (Lotus Petal, Elvish Spirit Guide) — **keep.** *Elvish Spirit Guide*
  produces mana **from hand**. The loop cannot currently get mana from a
  non-battlefield zone; that is a new verb.
- `SELECT` (Thrasios, Sylvan Library) — **keep**, and see the note below.
- `STATIC_MANA_MODIFIER` (Kinnan, Enduring Vitality) — **keep.** Both alter the
  mana system while present rather than producing mana. Merging the two former
  one-user kinds was right; merging them into `MANA_SOURCE` would not be.
- `CARD_COST` (Chrome Mox, Mox Diamond) — **keep, and it matters more than its
  count.** Both pay for themselves with a *card from hand*. For a mulligan
  solver this is the difference between a 7-card keep and a 6-card keep, so a
  2-user kind sits directly on the value function's most sensitive input.

`CARD_COST` generalises into the test for any future low-count kind:

> **RULE K2 — a kind's justification is its position, not its population.**
>
> User count is a *diagnostic*, not a criterion. The criterion is **sensitivity**:
> how much does the answer move if this kind is modelled wrong? A kind used by
> two cards that both consume cards from the opening hand outranks a kind used by
> ten that each shift a turn-8 outcome, because the opening hand is what the
> value function is a function *of*.
>
> So a low-count kind is justified when it sits on a sensitive input; and a
> **high**-count kind on an insensitive one deserves scrutiny it would otherwise
> escape. Population only ever raises the question. Position answers it.

The ranking of sensitivity for this project, most to least: **cards in the
opening hand → mana available on turns 1–3 → pattern-completion terms →
everything else.** `CARD_COST` is in the first tier with two users. Anything
landing in the first tier gets a kind almost regardless of count.

#### SCOPE NOTE — the scorer is thinly validated by this deck

**This deck has no cantrips.** No *Brainstorm*, no *Ponder*, no *Preordain*.
`SELECT` has two users (*Thrasios*, *Sylvan Library*); `TUTOR` has eight.

§6.3 argued that tutors and selection are the same problem sharing one scorer.
That still holds, but the emphasis was backwards — the scorer earns its keep on
**tutor targets**, and Phase 5 tests against those first.

The larger point is a limit on what v1 can demonstrate, and it belongs on the
record rather than in a priority ordering:

> **§6.3's state-dependent scorer will be validated against 8 tutor targets and
> 2 selection effects. That is thinner evidence than the design's prominence
> implies.** A scorer that ranks tutor targets well may still misplay a
> library-manipulation deck badly, and this deck cannot detect that. The scorer
> is the component whose correctness is least established by v1, and it should be
> described that way when v1's numbers are presented.

**This is what a second deck would be for.** Not breadth for its own sake — the
purpose would be *adversarial validation of the scorer*. A storm or
library-manipulation list with a dozen cantrips exercises `SELECT` the way this
deck exercises `TUTOR`, and any scorer bug that survives Kinnan would surface
immediately there. Recorded here so that if a second deck is ever picked, it is
picked for that reason and not at random. Out of scope for v1.

`FETCH` remains called out separately because it is not an effect in the clean
sense — it removes a card from the library and shuffles, changing the
composition of every later draw. At 7 of 25 lands, getting it wrong biases the
whole distribution. Note the deck's fetches are mostly off-colour
(*Flooded Strand*, *Polluted Delta*, *Scalding Tarn*, *Windswept Heath*,
*Wooded Foothills*), so each can find only a subset of the four fetchable lands
— *Tropical Island*, *Breeding Pool*, *Forest*, *Island*. That predicate must be
per-card, not "get a land."

### 4.3 Where the archetype model breaks

Honestly, and in rough order of how much it costs you:

1. **Combos are relationships, not card effects.** Kinnan + Basalt Monolith is
   infinite mana; neither card's effect definition contains that fact. This is
   why §5 exists as a separate layer. Your instinct that "most cards reduce to a
   few archetypes" is right about *cards* and wrong about *decks* — the archetype
   layer is the easy part.
2. **Isochron Scepter needs imprint state.** A permanent that remembers a card.
   One special-cased field on the battlefield entry; ugly but bounded.
3. **Freed from the Real / Pemmin's Aura need attachment state.** An aura
   pointing at a creature, whose untap ability then feeds the mana system.
   Another bounded special case.
4. **Thassa's Oracle needs devotion and library size.** Its win condition is
   `devotion_to_blue >= library_size`. Both become pattern vocabulary (§5.2).
5. **State-dependent mana amounts** (Gaea's Cradle) mean a source's output is a
   function of state, evaluated at tap time, not a constant. This is why
   `DYNAMIC_MANA_SOURCE` is separate.

None of these are fatal. All of them are reasons the model is *this deck's*
model and not a general one.

### 4.4 Cards the model cannot express: refuse, with an explicit opt-out

Three states, and there is no fourth:

- **`modeled`** — has an effect definition.
- **`inert`** — explicitly declared as doing nothing in this model. Still a real
  card: it occupies a deck slot, can be drawn, costs mana if cast, and dilutes
  every draw.
- **unlisted** — **the build fails at load with the card name.**

**Refuse by default.** Silent approximation is how you get numbers you cannot
audit. Forcing an explicit `inert` declaration puts every limit of the model in
the deck file, where it is readable, greppable, and countable — and where a
reviewer can disagree with a specific line rather than with the tool.

The payoff: the run report prints **"N of 99 cards are inert"** every time
(§9.5). That is a permanent honesty metric that cannot rot, because the build
fails if it is ever out of date.

Note that most inert cards in a cEDH list are inert *correctly*. Force of Will,
Fierce Guardianship, Swan Song, Flusterstorm, Pact of Negation — a goldfish
genuinely does not use interaction. The model is not failing there. But it does
mean the simulator systematically **overstates** the deck relative to a real
four-player pod, and that bias is one-directional and should be stated wherever
the numbers are presented.

#### `reason` is categorised, so the inert set has a shape and not just a size

A count of 28 says how much the model cannot see. It does not say **what** it
cannot see, and those are different questions — 28 counterspells and 28 mana
sources would be the same number and completely different models.

So every `inert` card carries a required `reason` (free text, for the human) and
a required `reason_category` drawn from a **closed enumeration**, same discipline
as the kinds:

| Category | Meaning | ~Cards |
|---|---|---|
| `interaction` | Counters or removal with no opposing spell or permanent to answer | ~14 |
| `opponent_trigger` | Triggers on an opponent acting | ~5 |
| `opponent_permanent` | Targets, copies, or steals something an opponent controls | ~4 |
| `timing_only` | Grants flash or alters timing; meaningless with no stack and no opponents | ~2 |
| `no_object_in_model` | Needs a game object the model lacks — combat, the stack, a meaningful graveyard | ~3 |

Counts are provisional; exact assignment happens at authoring (§15 item 23) and
this table is then replaced with measured values, as §4.5 was.

Reading the shape is the point. If `interaction` dominates, the model is missing
*opposition* and the fix is opposition profiles (§2.5). If `no_object_in_model`
dominated instead, the model would be missing *mechanics*, and the fix would be
a bigger card model — a completely different project. The provisional split says
it is the first, which is evidence that the narrow model is narrow in the right
direction.

**The grouped table, not the bare count, is what the run prints** (§9.5).

### 4.5 The actual list, measured

Measured against `mtg_v1.card_any_medium` on 2026-09-01. **No estimates here** —
every number is a query result. The earlier projection has been deleted rather
than reconciled.

**Resolution:** 99/99 resolve. 97 by exact name, 2 by front-face split. Colour
identity within `{G,U}` for all 99. Five require `card_any_medium` rather than
`card` (§2.7).

**Type composition** (`all_types`, cards may count in several rows):

| Type | Count |
|---|---|
| Creature | 26 |
| Land | 25 |
| Instant | 22 |
| Artifact | 17 |
| Enchantment | 8 |
| Sorcery | 5 |
| Battle | 1 |

24 cards have empty `castable_cmcs` — the 24 true lands. The 25th "land" is
*Sink into Stupor // Soporific Springs*, an MDFC whose spell face is castable at
3.

**Multi-faced cards: 2**, and **both have empty `mana_cost` in the contract**:

| Card | Layout | `mana_cost` | `castable_cmcs` | `has_land_face` |
|---|---|---|---|---|
| Invasion of Ikoria // Zilortha, Apex of Ikoria | `transform` | **(empty)** | `{2}` | f |
| Sink into Stupor // Soporific Springs | `modal_dfc` | **(empty)** | `{3}` | t |

Smaller than §2.2's corpus-wide rates suggested, but not zero, and both are real
cards in the list. `mtg_v1.card_face` (§2.1) is still required — and *Invasion of
Ikoria* is a **Battle**, a card type the model in §4.2 has no concept of.

**Opponent-facing text: 24 of 99** — 14 mention an opponent, 10 counter a spell.
Their treatment is §2.8's and §4.4's problem, and three of them are mana sources.

**What is deliberately not stated here: the `modeled` / `inert` split.** That
number is the *output* of authoring effects (§15, Phase 7 item 23), not an input
to it. Producing it now would mean guessing, and §4.4's build-fails rule
guarantees it will be exact and current once the work is done. It gets recorded
here then, and printed on every run regardless (§9.5).

---

### 4.6 Decided: no Battle type, ever

*Invasion of Ikoria // Zilortha, Apex of Ikoria* is the only Battle in the list.
**First, a correction to the premise:** it is **mana value 2**, not 4 —
`mana_value = 2.0`, `castable_cmcs = {2}`, `color_identity = {G}`. That matters,
because a 2-drop is seen and cast in far more games than a 4-drop, so the cost of
getting it wrong is higher than you assumed, not lower.

**The architectural decision, settled now: the model does not gain a Battle
type.** The reason is stronger than "it is one card":

> **A Siege flips by being attacked. The model has no combat.** Zilortha is
> unreachable by construction — not approximated, not unlikely, *impossible*. A
> fully-implemented Battle type would still never transform, so the type would
> buy exactly nothing at any level of effort.

That collapses the question to: does the **front face's ETB** do something worth
modelling? If yes, it is authored as an ordinary spell effect and the battle
permanent is simply discarded — that is not "adding a card type," it is
modelling an ETB and throwing away a permanent the model cannot use. If the ETB
is combat- or opponent-facing, it is `inert`.

#### RESOLVED in Phase 0: it is a `TUTOR`, not inert

`mtg_v1.card_face` shipped (§2.1) and the text it withheld settles this
immediately — **against the instinct, and against inert**:

> *Invasion of Ikoria* — `{X}{G}{G}`
> "When this Siege enters, search your library and/or graveyard for a
> **non-Human creature card with mana value X or less and put it onto the
> battlefield**."

That is the same effect as *Finale of Devastation*, *Chord of Calling* and
*Nature's Rhythm* — an X-cost creature tutor straight to the battlefield, and
its non-Human clause is the same restriction Kinnan's own ability carries. It
finds *Thrasios* (Merfolk). It is a **core game-plan card**, and `TUTOR` goes
from 8 users to 9.

It is also not a 2-drop. `mana_value = 2` is the `{X}{G}{G}` cost evaluated at
X=0, which is the rules-correct reading and a meaningless one for curve
purposes — it is castable at 2 and useless there. §2.2's warning about
`mana_value` distorting a curve, applied to a card in this deck.

**The architectural decision held; the data flipped the residual.** No Battle
type — Zilortha is still unreachable, since a Siege flips by being attacked and
the model has no combat. The front face is authored as an ordinary spell effect
and the battle permanent is discarded, exactly as prescribed above. What changed
is only the one-line answer to "does the ETB do something?"

**And declaring it inert would have been a real error**, not a rounding one: it
would have removed one of nine tutors from a deck whose game plan is tutoring,
in ~15–20% of games, concentrated in the early turns the CDF is most sensitive
to. Ten seconds of reading the text beat both of our instincts, which is the
argument for §4.7's `authored_from` — every one of these decisions is only as
good as the text someone actually read.

*Sink into Stupor // Soporific Springs* resolved at the same time and needs no
new machinery: the spell face is `{1}{U}{U}` "return target spell or nonland
permanent **an opponent controls**" → `inert`, category `opponent_permanent`;
the land face taps for `{U}` and enters tapped unless you pay 3 life →
`MANA_SOURCE`. That "pay 3 life" is the **same predicate shape** as *Breeding
Pool*'s, so **RULE K1's standings are unchanged at 3 of 4.**

### 4.7 The effect-authoring format, worked on one card

Before authoring 99. *Basalt Monolith* is the instructive one — it is a mana
source, a self-untap, an engine piece, and it interacts with Kinnan.

```toml
[cards."Basalt Monolith"]
status  = "modeled"
# {3} Artifact. "This artifact doesn't untap during your untap step."
# "{T}: Add {C}{C}{C}."  "{3}: Untap this artifact."
authored_from = "sha256:4f9c1a…"   # hash of oracle_text at authoring time

[[cards."Basalt Monolith".effects]]
kind             = "MANA_SOURCE"
produces         = { C = 3 }
taps             = true
untaps_normally  = false     # the "doesn't untap" clause
is_creature      = false
is_land          = false

[[cards."Basalt Monolith".effects]]
kind = "SELF_UNTAP"
cost = { generic = 3 }
```

Four properties of the format, each deliberate:

**1. A card carries a list of effects, not one.** Basalt Monolith is two. This
is why §4.2 says "one or more of" — modelling it as a single archetype would
force a `MANA_SOURCE_WITH_UNTAP` special case, and then another for every other
combination.

**2. Cards do not know about other cards.** Nothing here mentions Kinnan.
Kinnan's doubling is a `MANA_MULTIPLIER` on *Kinnan's* entry, applied by the mana
system (§6.4 step 1) to any source it sees. The engine "Kinnan + Basalt" is
declared in the **deck** file (§5.1), not here. Three layers, three files, no
cross-references downward — this is what makes effects reusable across ablated
decks (§9.2).

**3. `untaps_normally = false` is a flag, not an archetype.** The test for
whether something is a flag or a new `kind`: a flag modifies how an existing
effect behaves; a `kind` is a thing the game loop must learn to do. Getting this
wrong is how a 13-archetype model becomes a 40-archetype model.

**4. `authored_from` pins the text the human read.** The nightly ingest updates
cards; `content_updated_at` moves. The exporter recomputes each card's oracle
text hash and **warns when it differs from `authored_from`** — that effect was
authored against text that has since changed, so a human should re-read it. This
is the seam where the hand-authored layer meets live data, and without it the
effects file rots silently. (Note that for transform cards this hash is currently
of an empty string, which is §2.2 again.)

The trivial and the absent cases, for completeness:

```toml
[cards."Sol Ring"]
status = "modeled"
authored_from = "sha256:1b77e2…"
[[cards."Sol Ring".effects]]
kind = "MANA_SOURCE"
produces = { C = 2 }
taps = true

[cards."Force of Will"]
status          = "inert"
reason_category = "interaction"    # closed enum, §4.4
reason          = "counterspell; no opponents cast spells in this model"

[cards."Exotic Orchard"]
status = "modeled"
[[cards."Exotic Orchard".effects]]
kind     = "MANA_SOURCE"
produces = { from_table = "opponent_colors" }   # §2.8; empty list => produces nothing
taps     = true
is_land  = true
```

`reason` is **required** on every `inert` card and is free text. It is the field
that makes §9.5's inert count auditable rather than a number — you can read 23
reasons and disagree with three of them.

---

## 5. Win patterns

### 5.1 Structure: two levels, not flat and not arbitrary

**Engines** are named declarations that set typed flags. **Wins** are
conjunctions that reference flags and cards. A win may reference an engine; an
engine may not reference another engine; a win may not reference another win.
Exactly one level of indirection.

**Why not flat:** with 3 engines and 4 outlets you would hand-write 12 patterns
and lose the ability to report "which engine" separately from "which outlet" —
which is exactly the diagnostic the ablation work needs.

**Why not arbitrary composition:** at 6–10 patterns it buys nothing, and it
costs you cycle detection, evaluation-order questions, and a small graph
evaluator to debug. If you ever need it, the two-level form is a strict subset
and can be widened without invalidating existing files.

**Cost of the choice:** an outlet usable by two engines is written once and
referenced twice; an outlet needing engine-specific conditions (a `{5}{G}{U}`
outlet works off a coloured engine but not off Kinnan+Basalt's colorless one)
must express that as a mana-type term rather than by nesting. That is the right
place for it anyway, per §2.6.

```toml
[[engine]]
name = "kinnan_basalt"
sets = ["INFINITE:C"]
requires = { in_play = ["Kinnan, Bonder Prodigy", "Basalt Monolith"],
             untapped = ["Kinnan, Bonder Prodigy"] }

[[win]]
name = "kinnan_basalt_into_dig"
requires = { flag = "INFINITE:C",
             in_play = ["Kinnan, Bonder Prodigy"],
             available_mana = { G = 1, U = 1 } }   # the {5} comes from the engine

[[win]]
name = "thoracle"
requires = { resolved = ["Thassa's Oracle"], devotion_blue_gte_library = true }
```

### 5.2 The vocabulary of pattern terms

A pattern term is one of exactly these. This list is the contract; anything not
on it cannot be expressed.

| Term | Meaning |
|---|---|
| `in_play = [names]` | all named cards on the battlefield |
| `in_hand = [names]` | all named cards in hand |
| `in_play_or_hand = [names]` | each named card in either zone |
| `any_of = [names]` | at least one of the named cards, in play |
| `untapped = [names]` | named cards present and untapped |
| `flag = "INFINITE:C"` | a typed flag set by an engine this turn |
| `available_mana = {C=n, U=n, …}` | payable right now from untapped sources |
| `resolved = [names]` | card resolved this game (for ETB wins) |
| `library_size_lte = n` | |
| `devotion_blue_gte_library` | Thassa's Oracle's actual condition |
| `creature_count_gte = n` | |
| `attached = [aura, creature]` | aura attachment (Freed from the Real) |
| `imprinted = [scepter, card]` | Isochron Scepter's exiled card |
| `turn_gte = n` | |

**Deliberately absent, and will not be added:** anything about opponents, life
totals, the stack, timing windows, "target player", combat, or the graveyard. If
a line needs one of those, it is not expressible and the line is not modelled —
say so in the deck file rather than approximating it.

**Terms are conjunctive only.** No `OR` inside a pattern, no negation. Two
alternatives are two patterns; that keeps "which pattern fired" meaningful.
Negation is banned because a term that fires on the *absence* of something is
how you get a pattern that silently succeeds due to a modelling gap.

### 5.3 Requirements you specified

- **Record which pattern fired.** The result is `(turn, pattern_id)`, and the
  reported distribution is over both. Output includes a pattern mix table
  ("68% kinnan_basalt, 19% thoracle, 13% other").
- **A pattern that never fires is reported, not omitted.** The summary lists
  every declared pattern with its count, including zeroes, under a heading that
  names them as either dead lines or modelling bugs. Zero-count patterns are
  printed *first*, because they are the interesting ones.
> **A never-fired pattern has FOUR causes, not two.** §5.3 originally named
> two; the report has since found the others by itself:
>
> | Cause | Signal | The fix is |
> |---|---|---|
> | Dead line | fired 0, also-satisfied 0 | a deck change, or delete the pattern |
> | Modelling bug | fired 0, also-satisfied 0 | code |
> | **Shadowed** | fired 0, also-satisfied **> 0** | reorder or narrow the patterns |
> | **Resolved intra-turn** | fired 0, also-satisfied 0 | evaluate more often, or accept it |
>
> The fourth arrived in Phase 7 and is the subtlest. Patterns are checked once
> per turn, after the main phase, which is exact for the turn *number* but blind
> to any state the policy creates and resolves within a turn.
> `infinite_C_outlet_in_hand` fired 145 times under the stub and **zero** under
> the authored policy — not because the deck changed, but because the policy
> casts a rank-88 Thrasios the moment it is affordable, so the outlet is never
> still in hand when the check runs.
>
> **A worked history: `infinite_C_into_thrasios` fired 1095, then 9, then 1258.**
> Every move was a proxy being replaced by the actual condition, and only the
> last number means anything.
>
> | Fires | What the engine required | Why it was wrong |
> |---|---|---|
> | 1095 | Kinnan + Basalt in play, Basalt untapped — under a **stub mana model** | Every permanent with a land face tapped for any colour; rocks produced nothing |
> | 9 | the same, under **real mana** | `untaps_normally = false` was now honoured, so Basalt stayed tapped forever and the *untapped* proxy almost never held |
> | 1258 | Kinnan + Basalt in play, **entry cost `{3}` payable now** | — |
>
> The instructive part is the middle. **The number looked stable at 1095 and was
> wrong twice before it was right**, and the second value was wrong in the
> opposite direction from the first. Neither move was a bug being fixed; both
> were a *proxy* — "a land face means mana", "untapped means loopable" — being
> replaced with the thing it stood in for.
>
> A reader seeing only the final figure would have no way to know it had moved
> two orders of magnitude in both directions. That is the argument for the
> pattern mix being reported at every phase rather than at the end.

> It is indistinguishable from a dead line in the output, and it is the one
> cause that a *better* policy makes *more* likely. Worth remembering whenever a
> pattern describes a transient state rather than a stable one.

- **Validated at load.** Every card named in any pattern must be in the deck;
  every deck card must resolve in the export. Both fail loudly at load with the
  offending name. Never mid-simulation — a simulation that can fail is a
  simulation whose failures correlate with the seed.

---

### 5.4 What this deck actually wins with — and why "win" must mean "assembled"

Read from oracle text, not from memory of the archetype.

**Engines:**

| Engine | Mechanism | Output |
|---|---|---|
| Kinnan + Basalt Monolith | tap for `{C}{C}{C}`, Kinnan adds `{C}` → 4; `{3}` untaps | **unbounded `{C}` only** |
| Kinnan + Enduring Vitality | Vitality grants every creature `{T}: add one mana of any color`; Kinnan doubles each | wide coloured mana, not unbounded |
| Valley Floodcaller | untaps Birds/Frogs/Otters/Rats on each noncreature spell cast | untap loop with a mana creature |

**Outlets:**

| Outlet | Cost | Works off the Basalt engine? |
|---|---|---|
| **Thrasios, Triton Hero** | `{4}` generic | **Yes** — this is the primary line |
| Kinnan's own ability | `{5}{G}{U}` | **No** — needs `{G}{U}` from elsewhere |
| Finale of Devastation / Chord of Calling / Nature's Rhythm | `{X}` + coloured | No, not alone |

*Thrasios* being in the 99 rather than as a partner commander is the whole
point: `{4}` is generic, so unbounded colorless converts directly into drawing
the library. **This is the concrete vindication of §2.6's typed flags** — a
boolean `INFINITE_MANA` would have let the same engine pay Kinnan's `{5}{G}{U}`,
which it cannot, and the two outlets would have become interchangeable when they
are not.

**Now the uncomfortable part.** This list contains **no Thassa's Oracle**, no
*Ad Nauseam*, and no other card that reads "you win the game." Every actual kill
is opponent-facing and therefore unrepresentable here:

- *Emrakul, the Promised End* — "target opponent takes an extra turn"
- *Hullbreaker Horror* — bounces permanents you *don't* control
- *Finale of Devastation* into a lethal attack — combat, no opponents

**So for this deck a win pattern cannot mean "the game is won." It means "the
deck reached a state from which a competent pilot wins."** That is a real and
defensible thing to measure — it is what a goldfish measures — but it is a
different claim, and the difference has to survive contact with a reader six
months from now.

Consequences, all enforced:

- **Patterns are named for the state, not the outcome**: `infinite_C_thrasios`,
  not `win_thrasios`. The vocabulary in §5.2 never gets a `you_win` term.
- The result field stays `pattern_id`, and the reported quantity is
  **turn-to-assembly**, never "turn won."
- §9.5's header states it, and it goes in the manifest (§8.3) so the label
  travels with the numbers rather than living in this document.

This is the sharpest form of your §2.5 conclusion: it is not merely that some
cards are invisible, it is that **for this deck the terminal state itself is
outside the model** and has to be replaced with a declared proxy.

---

## 6. The play policy

### 6.1 Decision: authored priority, behind an interface

A `Policy` abstract interface with one v1 implementation, `AuthoredPolicy`. The
interface exists because it is nearly free now and expensive to retrofit; the
search-based implementation is **not designed in v1** and gets no accommodation
beyond the boundary itself.

The rule that makes the boundary real: **all decisions live behind `Policy`, and
`GameState` contains no decision logic.** If the state machine ever "chooses,"
the boundary has leaked. Enforced by review, and visible in testing because a
policy-free state machine is deterministic given a decision sequence.

The honest framing of what this buys: the numbers assume *this piloting*, and
the piloting is a file you can read. That is a defensible claim. "Near-optimal
play within a depth-capped search" is not a claim either of us could verify.

### 6.2 Your proposed split, and a correction

You proposed a static priority order for casting and land drops, plus a
state-dependent scorer for selection. **The split is right; the framing should
be one function, not two mechanisms.**

```
score(card, state) = 1000 * authored_rank[card] + situational(card, state)
```

The "static priority list" *is* the scorer, with a large constant term. This
matters for three reasons: one thing to test, one thing to dump in the play log,
and no question about which mechanism governs a given decision.

`situational` is a small, closed set of integer adjustments:

- **Completes a pattern** — enormous bonus if adding this card to the
  battlefield would satisfy a declared win or engine given current state.
- **Castability** — heavy discount if not castable this turn and not a land.
- **Colour need** — bonus for a source producing a colour the hand requires and
  the board lacks.
- **Land count** — lands score high below the deck's land floor, near zero above
  its ceiling. Both numbers declared in the deck file.
- **Redundancy** — penalty for the Nth copy of an effect already online.

Integer, not floating point (§7.3).

> **The weight ladder is derived, not chosen — and it was wrong the first time.**
>
> `completes_engine` was set to 50,000 against a rank term spanning 0–100,000,
> so a rank gap of 70 outvoted *finishing the engine*. An "enormous bonus" a
> static ranking could overrule is the term failing at its only job.
>
> **The shape is a magnitude mismatch between two independently-chosen scales.**
> The rank scale (0–100, authored by hand in the deck file) and the bonus scale
> (round numbers, picked while writing the code) were each reasonable alone and
> had never been compared. No single line was wrong — the *relationship* between
> two lines was, and neither line's author was in a position to notice.
>
> This recurs anywhere terms are summed: a scorer, a heuristic, a weighted
> objective, a cost function. **The fix is not better numbers, it is deriving one
> scale from the other**, so the ordering is a property of the code rather than a
> coincidence of two authors' taste. Every tier is now computed a decade clear of
> the rank term's maximum, so the ladder cannot silently regress when the rank
> list grows — which it will, since ranks are authored per deck.
>
> Reading the numbers did not catch this. A test did.

### 6.3 Tutors and selection are the same problem

You suspected this; they are. Both are **"pick the best card from a candidate
set, given current state."** The only difference is the candidate set:

| Decision | Candidate set |
|---|---|
| Ponder / Brainstorm | top N of library |
| Worldly Tutor | whole library, filtered by predicate |
| Kinnan's `{5}{G}{U}` | top 5, filtered to non-Human creatures |
| What to cast | castable cards in hand |
| What to bottom | cards in hand |

One function, five call sites. **So: no static tutor-target list.** A static list
misplays the same way a static cantrip ranking does — the right tutor target
depends on what is already in hand and on board.

**The wall against general search:** the scorer never looks ahead. It scores a
card against the *current* state only, never against a hypothetical future one.
That single rule is what separates this from a 1-ply search, and it is
enforceable by inspection because `situational` takes `const GameState&` and no
move list.

### 6.4 Mana sequencing is a constraint problem

Correct, and you are right that getting it wrong looks like the deck being worse
rather than the sim being wrong — a silent, one-directional bias.

"Can I pay cost C from my untapped sources?" is **bipartite matching** between
coloured pips and sources. Sources are `(colour_mask: uint8, amount, is_land,
is_creature)`; a `{G}{U}` cost is two pips with masks `0b00010` and `0b00100`.

Algorithm:

1. Apply active `MANA_MULTIPLIER` effects (Kinnan) to each source's output.
2. Resolve `DYNAMIC_MANA_SOURCE` amounts against current state.
3. **Match coloured pips to sources, most-constrained-first**: sort pips by how
   few sources can produce them, assign greedily, backtrack on failure via
   augmenting paths.
4. **Pay generic from what remains, least-flexible-first** — spend the
   mono-coloured source before the any-colour one.

Step 3 is exact, not heuristic. With ≤ 6 coloured pips and ≤ 20 sources, an
augmenting-path matching over `uint32` bitmasks is a few hundred nanoseconds.
Most-constrained-first ordering means backtracking almost never triggers.

**Cost:** this is the hot path (§11). It is called for every candidate card,
every turn. Budget ~200–500 ns; well within §2.4's envelope.

**Which land to play** stays a scorer decision (§6.2), not a matching decision —
it is a choice about future turns, and the scorer's colour-need term handles it.

#### BUILT, Phase 7 — and step 4 was the part nobody had done

Steps 1–3 shipped in Phase 2. **Step 4 did not, and for three phases nothing
noticed, because step 3's answer was being thrown away.**

`can_pay` ran the matching, committed specific sources to specific colours, and
returned a **bool**. The turn loop then decided which sources to tap by walking
the battlefield in slot order. Two functions independently deriving "which
sources pay this cost" — the twelfth rule in the ingestion repo's `PLAN.md`
§11.0 — with the second one crude, different, and invisible.

**It was measured before it was fixed**, by swapping the turn loop's arbitrary
tie-break for an equally arbitrary one. The figures below are from *after* the
`wide_colour_into_kinnan_dig` correction; the ones taken before it are in the
retraction that follows, and they were larger.

| payment rule | turn 3 | turn 6 | turn 12 |
|---|---|---|---|
| slot order (what shipped) | 7.96% | 37.10% | 66.78% |
| lands first (a probe, equally defensible) | 8.27% | 36.94% | 66.63% |
| **the actual assignment** | **8.64%** | **38.64%** | **67.25%** |

The two crude rules differ by 0.31 points at turn 3, which is inside the ±0.32
interval — so *arbitrariness alone* does not bound the error usefully. The real
comparison is crude against correct: the shipped rule cost **0.68 points at turn
3 and 1.54 at turn 6**, both several times the interval, at the objective §4.1
selects.

**The real assignment dominates at every turn**, which is what minimising
overpayment should do: more mana left untapped compounds into more spells cast in
the same turn.

`plan_payment` now returns the assignment and the turn loop spends exactly it.
The generic remainder is filled **best fit** — the largest source that does not
exceed what is owed, then the smallest that covers it — which minimises
overpayment. That is a dominance argument, not a preference: mana left untapped
is weakly better than mana wasted.

#### RETRACTED — a claim that rested on a bug

This section briefly carried a note reading roughly: *"minimising overpayment is
not the same as maximising assembly, because when a pattern names a permanent as
untapped, which source stays untapped matters."* The evidence given was that
`lands first` beat the real assignment at turn 3, the only place it did, because
it never taps *Kinnan* and `wide_colour_into_kinnan_dig` required Kinnan
untapped.

**That requirement was wrong.** Kinnan's dig is `{5}{G}{U}:` with **no tap
symbol** — being tapped is irrelevant to it, and the requirement had been copied
across from the *static* multiplier, which is a different ability. With the
pattern corrected, the numbers are:

| payment rule | turn 3 | turn 6 | turn 12 |
|---|---|---|---|
| slot order (what shipped) | 7.96% | 37.10% | 66.78% |
| lands first (probe) | 8.27% | 36.94% | 66.63% |
| **the actual assignment** | **8.64%** | **38.64%** | **67.25%** |

**The real assignment now dominates at every turn**, and the gap between the two
crude rules at turn 3 falls from 1.33 points to 0.31 — inside the interval. The
lands-first advantage was entirely an artifact of the wrong requirement.

The general claim may still be true. **The evidence for it is gone**, and it is
retracted rather than re-worded, because no pattern in this deck currently has a
correct `untapped` term to re-ground it on — that requirement was the only one in
the file. If one is ever authored correctly, this is the section to revisit.

What remains true and is *not* retracted: the assignment does not prefer to keep
any particular source untapped, that is a policy question rather than a matching
one, and §6.2's scorer is where it would live. It is simply not known to cost
anything today.

### 6.5 Determinism under the policy

Non-negotiable, and the failure mode is nasty: a tiebreak resolved by container
order looks exactly like variance, and would be invisible until an ablation run
disagreed with itself.

Rules:

- **Every tiebreak resolves to an explicit total order.** Score descending, then
  authored rank, then **export index** — a stable integer assigned at export
  time. The final key guarantees no ties survive.
- **`std::unordered_map` and `std::unordered_set` are banned in `core/`.** Their
  iteration order is unspecified and can vary across libc++ versions. Use sorted
  `std::vector` or fixed arrays. Enforced by a grep in CI.
- **No `std::sort` on equal keys without a tiebreak** — it is unstable. Either
  `std::stable_sort` or a comparator that is a strict total order. Prefer the
  latter.
- **No floating point in `core/`** (§7.3).
- **Seeded randomness only through the injected RNG** (§7.1). No
  `std::random_device`, no clock reads, no global state.

### 6.6 Inspecting a single game

A **v1 feature, not a debugging afterthought.** You cannot find policy bugs any
other way, and the whole design rests on the policy being auditable.

`--trace <seed>` runs one game and emits a turn-by-turn log:

```
T3  untap: Basalt Monolith(no), Birds of Paradise
    draw: Mystical Tutor  [hand: 4]
    mana available: {G}{G}{U}{C}  (Birds, Forest, Island, Sol Ring)
    considering:
      Basalt Monolith      score 8420  castable  (+2000 completes engine kinnan_basalt)
      Mystical Tutor       score 6100  castable
      Sea Gate Restoration score 5200  as land
    play land: Sea Gate Restoration (back face, tapped)
    cast: Basalt Monolith  paying {3} <- Forest, Island, Birds
    engine kinnan_basalt: NOT satisfied (Kinnan not in play)
```

It prints **the scores it considered and rejected**, not just what it did. A log
that shows only the chosen line cannot tell you the ranking was wrong.

The trace writer lives in `io/`, never in `core/` — `core/` takes an optional
observer interface that is null in production and costs a predictable branch.

---

## 7. Interface, determinism, and the return type

### 7.1 Signature

```cpp
// core/sim.hpp — no I/O, no printing, no globals.
struct GameOutcome {
    std::optional<std::uint8_t> win_turn;   // nullopt == censored
    std::uint8_t  pattern_id;               // valid iff win_turn
    std::uint8_t  turns_simulated;
};

GameOutcome simulate_one(const Deck& deck,
                         const OpeningHand& hand,
                         std::uint64_t seed) noexcept;

void simulate_batch(const Deck& deck,
                    const OpeningHand& hand,
                    std::uint64_t base_seed,
                    std::span<GameOutcome> out) noexcept;
```

The solver constructs hands; the core never mulligans. For the v1 CLI, a
five-line wrapper draws a random 7 and keeps it, **labelled in the output as a
random keep** — no heuristic anywhere the solver would later have to disable.

### 7.2 Censoring is a normal outcome, not an error

`std::optional<uint8_t>`, never a sentinel. A `99` or `-1` eventually gets
averaged and becomes a plausible-looking number. `std::optional` makes every
caller handle it, checked by the compiler.

Censoring is *common* here — a mull to 4 often simply never assembles — so the
statistics layer treats it as a first-class case (§10.3), not an outlier.

`(turn, pattern, censored)` is sufficient for every objective you named:
P(win by turn N) is a count over `win_turn <= N`; percentiles are order
statistics over the uncensored set with the censored fraction reported alongside
(§10.3 explains why P90 may be undefined and what is printed instead).

### 7.3 Seeding and thread safety

- **Counter-based seeding.** Game *i* uses
  `stream_seed = splitmix64(base_seed ^ splitmix64(i))`, then a
  **xoshiro256++** stream. Game *i*'s result is a pure function of
  `(base_seed, i)` — independent of thread count, scheduling, and iteration
  order. Indexed by *game*, never by thread.
- **No shared mutable state in `core/`.** `simulate_one` takes `const Deck&` and
  a stack-allocated state; it touches no globals and allocates nothing. It is
  therefore trivially callable concurrently, and the ablation sweep parallelises
  by handing each thread a disjoint range of game indices.
- **Design constraint, stated now:** any future feature wanting a cache,
  memo table, or shared counter inside `core/` must justify itself against this.
  Retrofitting thread safety is the expensive kind of mistake.

> **INVARIANT S1 — pure-function seeding.** The outcome of game *i* is a pure
> function of `(deck, hand, base_seed, i)`. It does not depend on thread count,
> scheduling, execution order, or how many games ran before it.
>
> This is not merely tidiness: **common random numbers (§10.4) depends on it and
> fails silently without it.** If S1 breaks, CRN's variance reduction quietly
> degrades toward zero and every ablation interval gets wider — with no error, no
> warning, and no symptom except numbers that need more games than they should.
>
> S1 belongs to a failure class worth recognising on sight — **a mechanism
> quietly not running** (ingestion `PLAN.md` §11.0). This project has produced
> three instances: a sequentially-seeded RNG that silently disables CRN,
> `RelWithDebInfo` defining `NDEBUG` and compiling out every `assert` in the
> test suite, and upstream's `is_unchanged` short-circuit that never fired.
> None had a symptom. Suspect anything *configured* rather than *called*.
>
> **Test S1 directly** (`tests/unit/seeding`), not as a side effect of other
> tests: assert that game 7 of a 10-game batch equals `simulate_one(…, seed, 7)`
> run alone; that a batch's results are identical across 1, 4, and 8 threads;
> and that reversing the iteration order changes nothing. This test is the thing
> standing between you and a silent 10× cost increase.
- **No floating point in `core/`.** Integer scores throughout. This avoids
  FMA-contraction and reassociation differences between `-O0` and `-O2`, which
  would make debug and release builds disagree — the worst possible
  reproducibility bug, because it appears only when you stop looking.

### 7.4 Batching

`simulate_batch` is in the interface from day one. Per-call overhead matters at
4,851 pairs × 50,000 games, and batching is where the parallel decomposition and
any future SoA layout would land. It is a loop in v1 — but it is *the* loop, and
callers written against it now do not change later.

---

## 8. Data pipeline: database to simulator

Your instinct is right: **export to a static file.** Reasons, in order —
reproducibility (a run is pinned to a data snapshot), no libpq dependency in
C++, no database on the hot path, and simulations that run on a machine with no
Postgres at all.

**The exporter is Python**, in `export/`. You know Python, it is the right tool
for a database-to-JSON transform, and it keeps the DB dependency entirely out of
the C++ build.

### 8.1 What gets exported

Exactly the cards named by the deck file(s) — 100 rows, not the 12,003-card
Simic pool. Small, diffable, and it forces resolution failures to surface at
export time.

**Join `mtg_v1.card_any_medium`, never `mtg_v1.card`.** This is not a
preference: `card` drops 5 of this deck's 99 cards and 254 Reserved List cards
corpus-wide, silently (§2.7). The exporter asserts it resolved *every* listed
name and fails otherwise, so the failure mode that produced §2.7 — a 94-card
deck reported as fine — cannot recur here even if the upstream view changes.

### 8.2 Pip requirements are parsed once, at export

Parse `mana_cost` into a structured form in the exporter, never per simulation.
The simulator loads integers and bitmasks and does no string handling. Exported
per castable face:

```json
{ "name": "Bala Ged Recovery",
  "full_name": "Bala Ged Recovery // Bala Ged Sanctuary",
  "export_index": 42,
  "layout": "modal_dfc",
  "faces": [
    { "index": 0, "cost": {"generic": 2, "pips": ["G"]}, "mana_value": 3,
      "types": ["Sorcery"] },
    { "index": 1, "is_land": true, "enters_tapped": true, "produces": ["G"] }
  ] }
```

`export_index` is the stable tiebreak key from §6.5, assigned in sorted
`full_name` order so it does not depend on query plan.

### 8.3 The manifest

Every export writes a manifest: max `content_updated_at` across exported rows,
row counts, the ingestion schema snapshot hash, exporter version, and a hash of
the output. The simulator records it in every result set.

This is what lets you attribute a regression: a changed number is either the
code or the card data, and without the manifest you cannot tell which.

**The result manifest also carries what the numbers mean.** Not a comment — a
required field, emitted with every result set including ablation output:

```json
{ "metric": "goldfish_turns_to_assembly",
  "measures": "turns until a declared pattern is assembled, unopposed",
  "does_not_measure": "deck strength, win rate, or card quality",
  "opponents": 0,
  "table_context": { "opponents": 3, "opponent_colors": ["W","U","B","R","G"] },
  "ablation_replacement": "Forest",
  "inert_cards": 23,
  "data_manifest": "8f3a2c1e" }
```

`does_not_measure` is a stored string, not documentation. Anything rendering
these results — a CSV, a plot, a future notebook — carries it without having to
know it exists.

### 8.4 Format

**JSON for v1.** Loaded once per process, ~100 cards — parse time is
microseconds against a multi-minute run. Human-readable and diffable matters far
more. `nlohmann/json` via FetchContent, in `io/` only.

A binary format is a trivial later change behind the same loader interface. Do
not do it now; it would be optimising 0.001% of runtime at the cost of every
debugging session.

---

## 9. Deck definition, loading, validation

### 9.1 Format

**TOML** (`toml++`, header-only, via FetchContent). Deck files are hand-authored
and hand-edited constantly — patterns, ranks, scorer weights — and TOML has
comments, which JSON does not. A design document you cannot annotate is one you
will stop annotating.

### 9.2 Two files, deliberately

- **`data/effects.toml`** — card name → effect definition. A property of the
  *card*, shared across decks.
- **`data/kinnan.deck.toml`** — the 99 + commander, authored ranks, win
  patterns, engines, scorer weights, land floor/ceiling, turn cap, the
  `[ablation] replacement` declaration (§9.4), and the `[table]` opponent
  context (§2.8).

Split because ablation swaps cards in and out. If effects lived in the deck
file, every ablated variant would carry a copy and they would drift.

### 9.3 Validation, all at load, all fatal

> **RULE C1 — a bug that returns fewer rows is worse than one that errors.**
>
> A missing view errors on contact and gets fixed in minutes. A filter that
> silently drops 5 of 99 cards returns a plausible number from a broken deck, and
> nothing in the output looks wrong (§2.7). Absent data is loud; *thinned* data
> is silent, and silence is what reaches a chart six months later.
>
> **Therefore: every stage that maps a set of inputs to a set of outputs asserts
> the counts match, and names what is missing when they do not.** Never
> `len(result) > 0`, never a truthiness check, never "the query succeeded."
>
> This is cheap, it is general, and it does not depend on knowing which bug you
> are guarding against — it would have caught §2.7 in one second without anyone
> having heard of the Reserved List.

> **RULE C2 — a check with a sample size of 254 and a definition beats one with
> 38,628 and a source.**
>
> Sample size measures the *source's* size, never the check's power. What settled
> the §2.7 defect was not more rows: it was that *the Reserved List is a
> paper-only promise, so a Reserved card that is not a paper card cannot exist* —
> 254 rows, and decisive, because the invariant comes from a **definition**
> rather than from the same data being asked twice. The 38,628-row agreement that
> preceded it compared two projections of one printing and could not disagree
> with itself.
>
> This generalises well past testing, and it applies to this project's own
> output: **a small check anchored in a definition outranks a large one anchored
> in a source.** §9.3's count assertions are of the first kind — "99 names in,
> 99 rows out" is true by definition of what loading a deck means. §10.4's
> 50,000-game intervals are of the second, and no number of games makes them
> answer a question about whether the model is right.

Applied at three boundaries, each asserting a count:

- **Exporter:** rows returned == names requested; the diff is printed by name.
- **Deck loader:** cards resolved == entries in the deck file == 100.
- **Ablation driver:** decks constructed == cards ablated + 1 baseline.

Then, at load:

1. Every deck card resolves in the export (front-face names accepted, §2.3).
2. Exactly 100 cards; singleton except basic lands.
3. Every card's `color_identity ⊆` commander's identity.
4. Every card is `modeled` or explicitly `inert` (§4.4).
5. Every card named in any pattern or engine is in the deck.
6. Every card named in the priority list is in the deck, and vice versa.
7. Every engine flag referenced by a win is set by some engine.
8. Every castable card has usable cost data — **this is where a missing
   `mtg_v1.card_face` surfaces as a loud error rather than as an MDFC that
   quietly costs zero.**
9. `[table]` and `[ablation]` are present, with `replacement` resolving and
   legal in the commander's identity. **Every field is required and none has a
   default** — `opponents`, `opponent_colors`, `on_the_play`, `replacement`. An
   unstated assumption is the failure §2.8 and §9.4 exist to prevent, so omitting
   any of them fails the build rather than silently picking `opponents = 0` or
   one of two known-wrong `on_the_play` values.

Checks 1 and 8 also run in the *exporter*, which is the real boundary. Failing
there is better: it fails once at export rather than on every run.

### 9.4 Ablation baseline: replacement, declared per deck

**Settled: ablation replaces the removed card, it does not shrink the deck.**
A 98-card deck conflates "this card is good" with "a smaller deck draws its good
cards sooner," and value-above-replacement is undefined without a replacement.

**The replacement is a per-deck declaration, not a hardcoded basic:**

```toml
[ablation]
replacement = "Forest"      # the baseline every ablation is measured against
```

Hardcoding a basic would bake an unstated baseline into the tool. For a
mana-hungry deck a Forest raises the land count by one, so every nonland
ablation is measured against a slightly better-manaed deck and some will read
positive for that reason alone. That bias is not removable — it is inherent in
choosing any replacement — so the design makes it **stated and changeable**
rather than invisible.

**The bias runs both ways and the honesty header says so** (§9.5):

| Ablated card | Effect on land count | Direction of bias |
|---|---|---|
| A nonland | +1 land | Flatters the ablation — some cards will look worse than they are |
| A land | unchanged | Neutral on count; still changes which lands |

Being able to re-run the sweep with `replacement = "Island"`, a basic that is
strictly worse, or a second copy of an existing effect is the point: if a
finding survives a change of baseline it is about the card, and if it does not
it was about the baseline.

#### TECHNIQUE — declared-inert elements are a free control group

**The bias above is inherent in choosing any replacement. It is not
unmeasurable, and the thing that measures it was already in the model for a
different reason.**

This deck declares 31 cards `inert` (§4.4), each with a category and a
human-readable reason, so that the run report can say what the model cannot see.
That declaration has a second use nobody designed it for:

> **Ablating an element declared to do nothing measures the replacement and
> nothing else. A set of such elements is a control group, and its mean is the
> baseline bias in the units of the result table.**

Measured for this deck at turn 3: **−0.411%**, spread −0.627% to −0.287%. That
number *is* what a *Forest* is worth over a blank card, and subtracting it turns
"value against a Forest" into "value against nothing" — which is the quantity a
reader thinks they are looking at.

Without it the §16.5 table is close to unreadable. At turn 3 a land beats most of
this deck, so nearly every nonland reads negative, and there is no way to tell
*worse than a land* from *worse than nothing*. With it, seven of the deck's eight
clones resolve to zero.

**Three conditions, because this does not always work:**

1. **The inert set must be declared, not inferred.** A card someone *believes* is
   weak is not a control; a card the model is *architecturally unable to
   represent* is. §4.4's forced declaration — every card `modeled` or `inert`
   with a reason and a category, or the build fails — is what makes the set
   trustworthy, and it was built to make the model's limits auditable rather
   than for this.
2. **Report the SPREAD, not just the mean.** The 31 nulls span 0.34 points,
   wider than any single paired interval, because a cheap blank still gets cast
   and wastes mana where an expensive one never does. "A blank card" is not one
   number. Anything within about half that spread of zero is not distinguished
   from doing nothing, and a re-centred column printed without its spread claims
   a precision it does not have.
3. **It needs enough of them.** Five is the floor the CLI enforces; fewer is a
   coincidence rather than a control group, and the report says so and omits the
   column instead of computing a null from one card.

**The generalisation, which is the reason this is written as a technique rather
than as a note about one sweep:** any model that forces its unrepresentable
inputs to be *declared* — rather than dropping them, defaulting them, or letting
them fail quietly — has a calibration set sitting inside it for free. The
declaration was made for honesty. It pays a second time as measurement.

Requirements:

- `replacement` must resolve in the export and be legal in the deck's colour
  identity — validated at load, same as any other card (§9.3).
- It is recorded in the result manifest (§8.3). A sweep run against a different
  baseline is not comparable to one run against this baseline, and the manifest
  is what makes that detectable.
- Ablating the replacement card itself is an error, not a special case.

### 9.5 The honesty header

Every run — CLI or library summary — prints before any numbers:

```
METRIC: goldfish turns-to-assembly. NOT deck strength, win rate, or card quality.
        Measures how fast this deck assembles a declared pattern with nobody
        interacting. A faster number is not a better deck.

deck: kinnan.deck.toml   cards: 100   modelled: 71   inert: 28   patterns: 7
data: manifest 8f3a2c1e (2026-09-01)
table context: opponents=3, opponent_colors=WUBRG, on_the_play=true
ablation baseline: replacement = Forest

WHAT THE MODEL CANNOT SEE  (28 inert cards, by reason)
  interaction          14   counters/removal with nothing to answer
  opponent_trigger      5   fires when an opponent acts
  opponent_permanent    4   targets or copies an opponent's permanent
  no_object_in_model    3   needs combat, the stack, or a real graveyard
  timing_only           2   grants flash; no stack, no opponents

ASSUMPTIONS THIS RESULT DEPENDS ON
  - No opponents. 28 inert cards; 14 of them are interaction.
  - Rhystic Study, Mystic Remora, Faerie Mastermind, Consecrated Sphinx,
    Wan Shi Tong, The Cabbage Merchant draw ZERO cards here.
  - Exotic Orchard and Fellwar Stone produce mana only because
    opponent_colors is set; with opponent_colors=[] they produce nothing.
  - Rejuvenating Springs is untapped only because opponents=3.
  - on_the_play=true means Gemstone Caverns' free play NEVER applies. This
    matches a real pod 25% of the time and undervalues ~7% of opening hands
    (7/99). Re-run with on_the_play=false to bound it.
  - Ablation replaces with Forest: ablating a nonland raises land count by 1
    and flatters that result; ablating a land holds it constant.
  - Patterns are ASSEMBLY states, not wins. This deck has no "you win the
    game" card; its real kills are opponent-facing (§5.4).
```

Three rules make this hold up rather than becoming boilerplate people skip:

1. **The metric banner is first and unconditional** — before any number, on every
   run, CLI or library summary. Not a footer, not a `--verbose` flag.
2. **The label is in the column name, not only the header.** Ablation output uses
   `goldfish_turn_to_assembly_delta`, never `score`, `value`, or `rating`. A
   sorted table of cards with a column called `score` *is* a card-quality ranking
   no matter what the header said 400 rows earlier; one called
   `goldfish_turn_to_assembly_delta` resists that reading on its own.
3. **Assumptions are generated from the deck file, not typed.** Each line is
   emitted by the thing that owns the assumption — the `inert` count from the
   loader, the `opponent_colors` line from `[table]`, the replacement line from
   `[ablation]`. A hand-written caveat block goes stale; a generated one cannot,
   because changing the assumption changes the text.

---

## 10. Statistical output

### 10.1 The primary metric is a CDF, not a mean

Report **P(win by turn N)** for N = 1…cap. That is the shape of the question
("how often by turn 3"), and each point is a proportion with clean inference.

### 10.2 Confidence intervals: Wilson score

For proportions, use the **Wilson score interval**, not the normal
approximation. This is not pedantry: the normal approximation fails badly when
p is near 0 or 1, which is exactly where turn-2 and turn-3 kills live — the
numbers you care most about. It can produce intervals extending below zero.

Wilson is a closed form, costs nothing, and behaves correctly at the extremes.

### 10.3 Percentiles, and censoring

Report **P10, P25, P50, P75, P90** of win turn, with **order-statistic
(Clopper–Pearson-based) nonparametric intervals** — exact and cheap; no
bootstrap needed.

**Censoring rule:** if the censored fraction exceeds `1 - p`, the p-th
percentile does not exist. Print `">15 (censored, 22% never assembled)"` — never
a number. A percentile silently computed over only the winners is a lie that
looks like data, and it biases *optimistically*, which is the direction you
would not catch.

### 10.4 Iteration counts, variance reduction, and multiple comparisons

For a proportion at 95% confidence, half-width w: `n ≈ 3.84·p(1-p)/w²`.

| Goal | n |
|---|---|
| ±2% absolute on a p≈0.3 estimate | ~2,000 |
| ±1% | ~8,100 |
| ±0.5% | ~32,300 |

So **10,000 games gives roughly ±1%** for a single deck. That is the easy case.

**The ablation case is different and much more demanding.** Comparing baseline
against an ablated deck is a *difference* of proportions: variance roughly
doubles, so resolving a 1-point difference needs ~30,000 per arm.

And with **4,851 pairwise comparisons at α = 0.05 you expect ~240 false
positives from noise alone.** Three decisions, all settled:

**1. Effect sizes with intervals are the primary output.** The reported result
for an ablation is `+2.1% [0.4, 3.8]` — a point estimate and its interval — not
a significance verdict. A ranked list of 4,851 "significant" findings is ~240
false positives dressed as discoveries, in a format that hides which is which.
An interval carries its own uncertainty and cannot be read as a verdict by
accident. Significance testing is available but never the headline.

**2. Benjamini–Hochberg where thresholding is unavoidable.** FDR control, not
Bonferroni — at 4,851 comparisons Bonferroni's α/n ≈ 1e-5 would suppress every
real effect this deck actually has.

**3. Common random numbers.** The baseline and each ablated deck run on the
*same seed sequence*. The two runs become positively correlated and the variance
of their **difference** collapses — often 10× or more. It costs nothing at
runtime and is worth more than every optimisation in §11 combined.

CRN is available **only** because seeding is a pure function of game index
(§7.3). That is why **INVARIANT S1 is a tested invariant rather than a
convention**: CRN does not fail loudly when S1 breaks, it just quietly stops
reducing variance, and the symptom is intervals that need 10× the games with no
error anywhere.

**Caveat, stated honestly:** replacing a card changes the deck, so the coupling
is imperfect — the same seed does not produce the same shuffle of a deck with
Forest in slot 42 as of one with Mana Crypt there. Correlation stays high but is
not 1. **Measure the realised variance reduction on the first sweep** by running
one ablation both coupled and uncoupled and comparing interval widths; do not
assume the 10×. Note that §9.4's replacement design helps here: a same-size deck
keeps the draw sequence far better aligned than a 98-card deck would.

#### MEASURED, Phase 7 item 22 — and the caveat above was wrong in our favour

98 ablations × 30,000 games, coupled and uncoupled:

| | mean standard error |
|---|---|
| coupled (common random numbers) | 0.00044 |
| uncoupled (independent seeds) | 0.00232 |
| **ratio** | **5.3× on the standard error, 28× on the variance** |

Range across the 98: **1.1× to 24.6×** on the standard error. So the headline
holds — 28× on variance is past "10× or more" — but the *spread* is the part
worth carrying forward: for some cards the coupling buys almost nothing, and a
single measured number would have hidden that.

**The caveat's premise does not apply to this implementation, and that is worth
correcting rather than leaving as a hedge that sounds careful.** Ablation here is
a *slot swap*: the ablated deck has the same slots in the same order, so for a
given seed the shuffle is the **identical permutation of slot indices** and only
the identity of one slot differs. The draw sequence is not "well aligned", it is
the same sequence. The two arms diverge only where the policy makes a different
decision — which is exactly the divergence being measured, and which is why the
1.1× cases are the cards that change the most decisions.

The variance reduction is why §16.5's question could be answered at 30,000 games
per card rather than needing the ~30,000-per-arm-per-point that §10.4's unpaired
arithmetic implies.

### 10.5 Also reported

- Pattern mix, zero-count patterns first (§5.3)
- Censored fraction
- Mean and standard deviation — last, and clearly subordinate to the CDF
- The data manifest hash (§8.3)

### 10.6 Built, Phase 6 — and the two things reading the output changed

`core/stats.hpp` holds Wilson, the binomial CDF, the percentiles and
`RunSummary`. All of it is a function of **counts**, so every reported number is
testable without running a game, and there is one place a figure can be wrong.

Two decisions that only became visible once real output existed:

- **The metric banner was not first, for three phases.** §9.5's rule 1 says
  "before any number, on every run", and every run in fact opened with the card
  database summary — *100 cards, 25 with a land face, 76 castable* — and only
  then said what was being measured. The rule was written down, agreed, and
  violated by the code that printed it, because the violation was in the
  ordering of two functions in `main` and nothing reads that as a claim.
- **The generated card list had to become one card per line.** *Wan Shi Tong,
  Librarian* is one card whose name contains a comma, and in a comma-joined list
  it reads as two. A caveat block that miscounts the cards it is warning about
  is worse than no caveat block, and no assertion on that line would have
  noticed — the sixth instance of §11.0's "output correctness is not testable,
  only readable".

**The percentile intervals invert the binomial CDF** rather than approximating.
The count of games at or below the true *p*-quantile is Binomial(*n*, *p*), so
the bracketing ranks come straight off that CDF and each rank maps back to a
turn through the cumulative histogram. That construction is what makes the
censored case honest: a rank past the last uncensored game has **no turn**, so
the report prints `[8, censored]` rather than clipping the bound to the cap in
the flattering direction.

The regularized incomplete beta is the only special function in the project. It
is here rather than a normal approximation because turn-to-assembly has a
twelve-point integer support, and a normal approximation over twelve points is
not an approximation of anything.

**What Phase 6 did not do:** §10.4's realised-variance-reduction measurement,
because it needs the ablation sweep (§15 item 22). Until it exists, §16.5's
admission stands — the ablations reported in §16 are paired but the pairing is
not exploited, so a difference of a few tenths of a point is not distinguished
from zero.

---

## 11. Where the hot path is

In order, from profiling expectations:

1. **`can_pay` / mana matching (§6.4)** — every candidate card, every turn.
   Dominates. Bitmask matching over `uint32`; most-constrained-first ordering to
   avoid backtracking.
2. **Card scoring (§6.2)** — every candidate, every turn. Integer arithmetic
   over a fixed-size array; the pattern-completion term is the expensive part
   and should be evaluated lazily, only for cards named in some pattern.
3. **Pattern evaluation (§5)** — after every state change. Compile each pattern
   at load into a pair of `uint64` masks over deck slots; a check becomes
   `(in_play & mask) == mask`. Effectively free.
4. **Shuffling** — once per game. **Partial Fisher–Yates only.** A game draws
   ~15–25 cards of 99; shuffling all 99 wastes ~75% of the work.

**State representation:** index cards by *deck slot* 0–99, not by identity — this
sidesteps the basic-lands-are-not-unique problem, since each basic gets its own
slot. Zones become `std::array<uint64_t, 2>` bitsets, with a parallel bitset for
tapped status. The library needs order, so it stays a `std::vector<uint8_t>` of
slot indices with a draw pointer.

Total state: on the order of 200 bytes. It fits in L1, which is the single
biggest performance property of the design and comes free from the bitset
choice.

**Do not** do SoA layouts, custom allocators, or SIMD in v1. §2.4 says you do
not need them, and they would obstruct §6.6's trace output.

### 11.1 Measured, Phase 2 (`-O3`, base M1)

`can_pay` built for correctness, then measured before any optimisation:

| Call | ns |
|---|---|
| `{G}{U}`, early board (4 sources) | 72 |
| `{G}{U}`, late board (22 sources) | 63 |
| `{5}{G}{U}`, late board | 63 |
| `{X}{G}{G}{G}`, late board, X=0 | 66 |
| `{B}{B}`, late board — **unpayable** | 64 |
| `max_affordable_x`, `{X}{G}{G}{G}`, late | 297 |

**Verdict: no optimisation warranted.** §6.4 budgeted 200–500 ns; it comes in at
~65, three to eight times under. Two things worth noting beyond the headline:

- **The unpayable case costs the same as the payable one.** That is the
  most-constrained-first ordering doing its job — a failing search is where a
  naive ordering would thrash, and it does not.
- **Board size barely matters** (72 ns at 4 sources, 63 at 22). The work is
  dominated by fixed setup, not by the search, which means the search is
  finding its assignment almost immediately on real boards.

`max_affordable_x` is ~5 `can_pay` calls via binary search, as expected.

> **What this does NOT tell us, and what to measure next.** This is nanoseconds
> per *call*. The per-game cost is `calls × 65 ns`, and the call count is a
> property of the **policy**, which does not exist yet. At a guess of ~200 calls
> per game that is ~13 µs of mana per game — already most of §2.4's 20 µs
> budget, which confirms `can_pay` is the hot path but says nothing about
> whether the total is 20 µs or 200 µs.
>
> **So the measurement that matters is calls-per-game, and it cannot be taken
> until Phase 5.** Optimising ns-per-call now would be tuning the half already
> known to be fine.

### 11.2 Measured, Phase 3 — calls-per-game, first observation

Instrumented from the first turn loop rather than reconstructed later.
**Stub policy, real 100-card deck, 200,000 games, `-O3`:**

| | |
|---|---|
| `can_pay` calls per game | **62.5** |
| Wall clock | **~4.1 µs/game** |
| Turns / cards drawn / spells cast | 12.0 / 18.0 / 11.0 |

My §11.1 guess was ~200 calls per game. The observed floor is **62**, about a
third of it — which is the reason to instrument rather than estimate, and a
small instance of the rule about deferring rather than guessing.

At 62 calls × 65 ns, mana accounts for ~4 µs, essentially the whole 4.1 µs.
`can_pay` is confirmed as the hot path, and the full pairwise sweep extrapolates
to **~4 minutes across 4 P-cores** rather than §2.4's pessimistic 21.

> **Read this as a floor, not a forecast.** The stub casts the first affordable
> card in slot order and stops. §6.2's authored policy will evaluate every
> candidate, score them, and ask `max_affordable_x` for X spells — each of which
> multiplies the call count. A real policy at 5-10x the traffic is entirely
> plausible and would still land inside §2.4's budget.
>
> Re-measure at Phase 5 with the same command. The counter is already there:
> `cs data/cards.json --games N --seed S`.

### 11.3 Measured, Phase 5 — the authored policy

Same command, same deck, 200,000 games:

| | Stub (Phase 3) | Authored (Phase 5) | |
|---|---|---|---|
| `can_pay` calls/game | 62.5 | **94.8** | 1.5x |
| Wall clock | 4.1 µs | **9.1 µs** | 2.2x |

**The 5-10x did not happen.** The scorer does evaluate every candidate rather
than stopping at the first affordable one, which is where the extra calls come
from — but `pattern_completion` turned out cheap, because a requirement check is
a mask compare and the hypothetical state is a stack copy of ~200 bytes.

Wall clock grew faster than call count (2.2x against 1.5x), so the added cost is
mostly scoring rather than mana. `can_pay` is no longer the whole story, though
it remains the single largest item.

Extrapolated: the full pairwise sweep is **~9.5 minutes across 4 P-cores**,
against §2.4's pessimistic 21 and Phase 3's optimistic 4. Still no optimisation
warranted, and §2.4's conclusion holds — the budget was never the constraint.

> Re-measure again when effects are authored (Phase 7). Real mana sources mean
> more sources per `can_pay` call and more castable candidates per turn, and
> that is the change most likely to move this.

---

## 12. The C++ project

Explained rather than assumed, per your note.

### 12.1 Build system: CMake + Ninja

`brew install cmake ninja` (neither is currently installed).

CMake is a build *generator*: it reads `CMakeLists.txt` and writes build files
for a backend — here Ninja, which is much faster than Make. You are not choosing
CMake because it is pleasant; you are choosing it because it is the lingua franca
and because `nanobind`/`pybind11` for your later Python bindings are CMake-native.
Meson is a friendlier language, but you would fight the ecosystem.

**`CMakePresets.json`** defines named configurations so you type
`cmake --preset debug` rather than remembering flags. Three presets:

| Preset | Flags | Use |
|---|---|---|
| `debug` | `-O0 -g`, assertions on | Development |
| `asan` | `-O1 -g -fsanitize=address,undefined` | **Run tests here by default** |
| `release` | `-O3 -DNDEBUG` | Benchmarks and real runs |

The sanitizer preset matters enormously for a first C++ project. ASan and UBSan
catch use-after-free, buffer overruns, and signed overflow *at the moment they
happen* instead of as corrupted output three functions later. Make the test
suite run under `asan` in CI and locally; run `release` only for timing.

### 12.2 Standard: C++20

Apple clang 21 supports it fully. You want `std::span` (a non-owning view over a
contiguous range — the right parameter type for "here is an array I do not own"),
`<bit>`'s `std::popcount` for the bitsets, designated initializers, and
`constexpr` improvements. C++23's library support in libc++ is still patchy;
not worth it.

### 12.3 Memory ownership — the good news

For this project the answer is unusually simple: **almost nothing is dynamically
allocated, and there are no smart pointers anywhere.**

- The card database loads once into a `std::vector<CardData>` owned by `main`
  (or by the binding module later) and outlives everything.
- Everything downstream takes `const CardDb&` or `std::span<const CardData>` —
  non-owning views. The owner is unambiguous and it is the caller.
- `GameState` is fixed-size arrays, stack-allocated per game. No heap, no
  destructor, no ownership question.
- **No `new`, no `delete`, no `shared_ptr`, no `unique_ptr` in `core/`.**

This is genuinely a good first C++ project *because* ownership is trivial here.
The rule to internalise: **a function that does not own something takes a
reference or a span, never a pointer.** Raw pointers in this codebase should
appear only where "might be absent" is meaningful, and even then prefer
`std::optional` or a reference.

The one place ownership gets interesting is the trace observer (§6.6), which is
a non-owning `Observer*` that may be null — deliberately the only nullable thing
in `core/`.

### 12.4 Testing: Catch2 v3

Via CMake's `FetchContent` — CMake downloads and builds the dependency as part of
your build, so there is no package manager to install and no system state. For
three dependencies this is the right weight; vcpkg or Conan would be
over-engineering.

Catch2 over GoogleTest: better documentation for someone learning, and plain
`REQUIRE(x == y)` instead of a macro family. doctest compiles faster but has a
smaller community; at this project's size compile time will not be your problem.

Test layers:

1. **Unit** — mana matching (the interesting one: hand-built source/cost cases
   with known answers, including the ones where naive greedy fails), pattern
   evaluation, the scorer, RNG reproducibility.
2. **Golden traces** — a fixed seed and deck produce a byte-identical play log,
   committed to the repo. This is the regression net that makes §6.5's
   determinism *checkable* rather than aspirational, and it catches accidental
   policy changes, which unit tests will not.
3. **Statistical** — over a fixed seed, a known-fast hand wins by turn N in
   x%±ε. Loose bounds; these catch gross breakage, not drift.
4. **Property** — no game exceeds the turn cap; a censored result never carries a
   pattern; the same seed twice gives identical results; results are independent
   of thread count.

Test 4's last case is worth writing early — it is the one that catches the
container-ordering bug from §6.5, and it is nearly free.

### 12.5 Layout

```
commander_simulator/
  CMakeLists.txt
  CMakePresets.json
  SIM_PLAN.md
  data/
    cards.json            # exported, with manifest
    effects.toml
    kinnan.deck.toml
  export/                 # Python: DB -> cards.json
    pyproject.toml
    export_cards.py
  src/
    core/                 # links ONLY the standard library
      card.hpp  mana.hpp  state.hpp  rng.hpp
      policy.hpp/cpp  patterns.hpp/cpp  sim.hpp/cpp
    io/                   # JSON/TOML loading, trace writing
    stats/                # Wilson, percentiles, aggregation
    cli/main.cpp
  tests/
    unit/  golden/  statistical/
```

**The load-bearing rule: `core/` does no I/O, includes no `<iostream>`, and
links nothing but the standard library.** That single constraint is what makes
Python bindings additive later — a binding module calls `simulate_batch` and
never touches `io/`.

> **PATTERN B — a boundary that cannot be crossed by accident.**
>
> This is the second instance in the project, and the pattern is worth naming
> because both instances arrived by the same reasoning and both are stronger
> than the reviews they replace.
>
> | Boundary | How it is enforced | What crossing it does |
> |---|---|---|
> | `mtg_consumer` cannot read `mtg_internal` | No `USAGE` grant | Query fails: `permission denied for schema mtg_internal` |
> | `cs_core` cannot use JSON | `cs_core` links only the standard library | **Fails to compile** — the include path is not there to find |
> | A pattern cannot be named for an outcome | `reject_outcome_naming` at load | **Deck fails to load**, naming the pattern and why |
>
> The third is one tier weaker than the first two — a check that runs, not an
> impossibility — but it is the same move: `win_thrasios` is not discouraged in
> a style guide, it is unusable. **What makes it usable rather than merely
> strict is the guard-the-guard test**: `windfall_engine_online` must pass, so
> the check is word-boundary matched rather than a substring search. A guard
> that fires on correct input gets disabled within a week, and then it is worse
> than nothing, because everyone remembers there is a check.
>
> Neither is a rule someone remembers. In both cases the wrong thing is not
> discouraged, it is *unavailable*: the upstream one fails at the first query,
> and ours fails at the first build, before any test runs and before review.
>
> **The test for whether a boundary qualifies:** could a well-intentioned person
> cross it without noticing? If yes, it is a convention and will eventually be
> crossed. A convention costs a review; a build property costs a compile error
> with the file and line already in it.
>
> Where a build property is not achievable, the fallback is a check that runs
> unconditionally — `scripts/check_core_is_sealed.sh` catches what still
> compiles (`<iostream>`, `std::unordered_map`, `std::random_device`), because
> those are in the standard library and therefore always reachable. That is
> strictly weaker: it is a rule that runs, not an impossibility.

Realised as: `cs_core` declares no `target_link_libraries` naming a project
library, `cs_io` links `nlohmann_json` as `PRIVATE` so nothing inherits a parser
through it, and the CLI links `cs_io` alone since `cs_core` arrives transitively.

---

## 13. Over- and under-engineered, flagged

**Over-engineered for one deck:**

- **Confidence intervals on every value-function call.** For the mulligan solver,
  a call returns raw outcomes; CIs belong in the reporting layer only. Computing
  them per call would be waste at 4,851 × 50,000.
- **A general effect DSL.** At 100 cards, effects are a fixed enum with
  parameters. A parser and evaluator would be more code than the effects.
- **Binary export format** (§8.4).
- **SIMD, SoA, custom allocators** (§11) — §2.4 says the budget is not there.

**Under-engineered in the original brief — all three now adopted:**

- **Common random numbers** (§10.4), with INVARIANT S1 (§7.3) tested rather than
  assumed. Worth more than every optimisation in §11 combined.
- **Multiple comparisons** (§10.4): effect sizes with intervals as the primary
  output, Benjamini–Hochberg only where thresholding is unavoidable.
- **Ablation baseline** (§9.4): replacement rather than deck-shrinking, with the
  replacement card declared per deck and its bias printed in the honesty header.

**Still open, and deliberately so:**

- **The trace format is an output contract.** Golden tests (§12.4) pin it, so it
  needs to be stable enough to diff. Worth a moment's thought before writing it,
  not after — but it is not worth designing before Phase 5.
- **Opposition profiles** (§2.5) are out of scope for v1 and are *not* a
  refinement to bolt on later. See the note in §2.5: they change what the model
  measures, and the v1 numbers should be read as an upper bound until they
  exist.

### 13.1 OPEN QUESTION — what a keep/mull chart is a chart *of*

**Unresolved, and it must be resolved before the mulligan solver starts, because
it decides what the solver computes rather than how.** Written up here with the
options and their costs; no option is chosen.

§1 says the core takes a *specific opening hand* and returns a value, and §7.1's
signature is `simulate_one(deck, hand, seed)`. That is the right interface and it
says nothing about what the **output** is. There are C(99,7) ≈ **1.6 × 10¹⁰**
seven-card hands. A chart cannot be over hands, so it is over something else, and
the plan has never said what.

**The difficulty is the feature set, not the search.** A chart over the wrong
features is unreadable; over too many it is uncomputable; and the features are
what a human has to hold in their head at the table, which is a constraint the
statistics cannot supply.

Four options, with what each costs and what each gives up:

| Option | What the chart is over | Cost | What it gives up |
|---|---|---|---|
| **A. Fixed feature grid** | A hand-authored tuple: land count × mana-source count × has-Kinnan × has-a-tutor, say | Cheapest. Enumerate cells, sample hands matching each, run *n* games per cell | Everything not in the tuple. Two hands in one cell can differ by *Basalt Monolith*, and the cell reports their average as if it were a decision |
| **B. Sampled hands, reported raw** | Nothing — a list of *N* sampled hands with a value each | Trivial to compute, and it is what the core already does | Readability entirely. This is data, not a chart, and the user has to find the pattern themselves |
| **C. Learned features** | Whatever separates keeps from mulls, fitted | A model, a training set, and a validation story this project does not have | Auditability. Every other number here is traceable to a declared assumption; a fitted feature is not, and §9.5's whole discipline breaks |
| **D. Decision-list over declared terms** | A short ordered list of rules over §5.2's *existing* pattern vocabulary — `in_hand`, `creature_count`, land count | Moderate: the vocabulary exists and is already validated at load | Expressiveness. It can only say things the pattern language can say, which is the point and also the limit |

**What is actually being asked, stated so the choice can be made against it:** a
mulligan decision is *keep this 7* versus *mull to 6 and keep the better of what
comes*. So the chart's cell value is not `P(assembled by turn 3 | this hand)` —
it is that quantity compared against the **expectation over the mulligan**, which
is itself a function of the same chart one row down. **The recursion is the
computation**, and it is cheap only if the feature set is small.

Three things worth deciding at the same time, because they interact:

1. **The features must be observable before the mulligan.** "Has a tutor" is; "is
   a hand that assembles by turn 3" is not. A feature the player cannot evaluate
   while holding the cards is not a decision rule.
2. **Cell counts must be reported.** A grid cell holding four sampled hands is
   noise with a number on it, and §10.2's intervals apply per cell — most of the
   sweep's discipline transfers directly.
3. **§4.1's blind spot lands here, and this is the one place it stops being
   analytical and becomes user-facing.** Everywhere else the blind spot is a
   caveat in a document read by someone already reasoning about objectives. On a
   keep/mull chart it is a **cell**, and the cell for "has *Basalt Monolith*"
   will read near zero to every pilot of this deck, all of whom know it is half
   the primary engine.

   > **A chart whose most legible cell contradicts universal intuition is a
   > chart nobody trusts — including in the cells where it is right.** The
   > blind spot does not merely mislead about Basalt; it discredits the rest of
   > the chart by association, and it does so to exactly the readers who know the
   > deck well enough to use it.

   Two consequences for the design:

   **§4.1's three chart-reader warnings belong on the chart, not in this
   document.** Specifically: the **turn on every cell**, and *"low means does not
   act by turn N"* adjacent to the numbers rather than in a legend. A caveat one
   scroll away from a grid is a caveat nobody reads, and §9.5's whole discipline
   is that the label travels with the number.

   **It is an argument for reporting two turns per cell rather than one.** A cell
   reading `3: +0.1 | 12: +5.6` is self-explaining in a way that `+0.1` with a
   footnote is not — the reader sees the card is slow rather than weak, without
   having to be told what the objective cannot see.

   **What a second turn costs, so the decision can be made on numbers:**

   | | cost |
   |---|---|
   | Simulation | **Zero.** `RunSummary` already accumulates `assembled_on` per turn and `PairedRun` already carries a 2×2 table for *every* turn — §16.5's turn-3/6/12 tables are read off one run. Both turns come from the same games |
   | Statistics | Zero. Wilson and the percentile machinery are per-turn already |
   | The mulligan recursion | **This is where it is not free.** *Keep* versus *mull* is a comparison, and two objectives can disagree. A cell with two numbers is a chart; a *decision* with two objectives needs a rule for which one decides, and that rule is a new declared assumption |
   | Screen width | Real and not trivial at grid sizes a person can scan |

   So: two turns per cell is free as **reporting** and not free as a **decision
   rule**. The honest split is probably to compute the keep/mull decision on one
   declared objective and *display* the second turn beside it as context — which
   makes the second number a guard against misreading rather than an input.

**DECIDED: B first, then A.** B is nearly free and it answers whether anything
separates at all — if raw sampled hands show no separation, A is a grid of noise
and the feature design was wasted effort. D stays as the thing A is eventually
validated against: if a decision list over declared terms reproduces the grid,
the grid is real; if it cannot, the grid was fitting noise. C is not on the table
while §9.5's auditability discipline holds.

---

## 14. Definition of done for v1

1. `mtg_v1.card_face` exists upstream; the snapshot and migration test are
   updated (§2.1).
2. `export/export_cards.py` produces `cards.json` + manifest for the Kinnan list,
   failing loudly on any unresolved name or missing cost data. **RULE C1 count
   assertions at all three boundaries** (§9.3), each naming what is missing.
   Regression-tested against `mtg_v1.card`, which must fail with 5 named cards.
3. `kinnan.deck.toml` declares 100 cards, every one `modeled` or `inert` with a
   `reason` **and a `reason_category` from the closed enum** on each inert card
   (§4.4), authored ranks, ≥ 6 win patterns, and engines. `effects.toml` carries
   `authored_from` hashes and the exporter warns on drift. The run prints the
   inert set **grouped by category**, not as a count (§9.5).
4. **NOT DONE, and it reads as done.** `simulate_batch` is implemented and
   `core/` links only the standard library, provably. But **§7.1's signature
   does not exist**: both entry points deal their own random opening hand.
   `begin_game` shuffles and draws seven, and there is no way to hand the core a
   *specific* hand — which is the one thing §1 says this library is for. The
   mulligan solver is the caller that would have discovered it, and it has not
   been written, so nothing has.

   The work is small: a `begin_game` overload that seeds a given hand and
   shuffles the rest, and an `OpeningHand` parameter threaded through
   `run_game`/`simulate_batch`. It is listed here rather than done because it
   belongs to the solver's boundary, and because a definition-of-done item that
   quietly reads as satisfied is worse than an open one (§11.0's
   mechanism-quietly-not-running family — this is the same shape in a
   checklist).
5. **INVARIANT S1 holds and is tested** (§7.3): game *i*'s outcome is a pure
   function of `(deck, hand, base_seed, i)` — identical across 1, 4, and 8
   threads, identical whether run in a batch or alone, identical under reversed
   iteration order. This test gates common random numbers (§10.4) and is not
   optional.
6. `[ablation] replacement` and `[table]` declared in the deck file, validated
   at load, and printed in the honesty header with their biases (§9.4, §9.5).
   Exotic Orchard, Fellwar Stone and Rejuvenating Springs behave per `[table]`,
   and `opponent_colors = []` visibly changes the result (§2.8).
7. `--trace <seed>` prints a readable turn-by-turn log including rejected
   candidates and their scores.
8. CLI prints the metric banner **first and unconditionally** (§9.5), then the
   honesty header, the P(assembled by turn N) CDF with Wilson intervals,
   percentiles with censoring handled per §10.3, and the pattern mix with
   zero-count patterns listed first.
9. The metric label is in the **result manifest as a required field** (§8.3) and
   in output **column names** — `goldfish_turn_to_assembly_delta`, never
   `score`. Assumption lines are generated from the deck file, never typed.
10. Test suite passes under the `asan` preset. Golden trace committed.
11. 10,000 games complete in **under 10 seconds** on this M1 — a deliberately
   loose bar (~1 ms/game) that §2.4's budget clears by orders of magnitude.
   Correctness first; the headroom is real.
12. `SIM_PLAN.md` limits section matches what the code actually does.

Explicitly **not** in v1: mulligan logic, Python bindings, opponents, a second
deck, the ablation driver itself.

---

## 15. Task breakdown, ordered

Each task ends somewhere runnable.

**Phase 0 — upstream and scaffolding**
1. Add `mtg_v1.card_face` to the ingestion repo; update the exact-set test,
   regenerate the snapshot, and add the **inventory-drift test** that asserts
   §3.8's documented view list matches what `R__010` creates, in both directions
   (§2.1). Record `card_name_index`'s absence and the simulator's dependency on
   it in `STATE.md`.
2. `brew install cmake ninja`. CMake skeleton, three presets, Catch2 via
   FetchContent, one trivial passing test. **Goal: the build works before any
   design lands in it.**

**Phase 1 — data in**
3. Exporter: resolve the decklist against **`mtg_v1.card_any_medium`** (§2.7),
   front-face index, assert all 100 resolve, fail loudly on any that do not.
4. Exporter: parse `mana_cost` into structured pips; emit `cards.json` +
   manifest.
5. C++ card DB loader; a CLI that loads and prints a summary. **First
   end-to-end path.**

**Phase 2 — mana, the hard part first**
6. `Mana`, `Cost`, `Source` types with bitmask colours.
7. `can_pay` via most-constrained-first matching, plus the greedy-fails unit
   tests. **Do this before the game loop** — it is the hot path and the thing
   most likely to be subtly wrong.
8. `MANA_MULTIPLIER` (Kinnan) and `DYNAMIC_MANA_SOURCE` (Cradle).

**Phase 3 — state and loop**
9. `GameState`: bitset zones over deck slots, library vector, turn counter.
10. RNG (`splitmix64` + `xoshiro256++`), partial Fisher–Yates, reproducibility
    test.
11. Turn loop: untap, draw, land drop, main phase — with a stub policy that
    plays the first legal thing. **First simulated game.**

**Phase 4 — patterns**
12. Pattern and engine parsing from TOML; load-time validation (§9.3).
13. Compile patterns to bitmasks; evaluate after state changes; typed flags.
14. `simulate_one` returns `GameOutcome`. **First real number.**

**Phase 5 — policy**
15. `Policy` interface; `AuthoredPolicy` with authored ranks.
16. The situational scorer (§6.2), integer-valued.
17. Wire the scorer to selection, tutors, and casting (§6.3).
18. `--trace` and the golden test. **Read a real game's log and disagree with
    it — expect to fix the policy here, and expect this to take longer than
    step 17.**

**Phase 6 — statistics**
19. `simulate_batch`; thread-count-independence test.
20. Wilson intervals, percentiles with censoring, pattern mix.
21. CLI reporting with the honesty header.

**Phase 7 — closing**
22. Parallel driver over game index ranges. **Done**, and it carried the
    leave-one-out sweep with it: `--sweep`, `--ablate <card>`, `--threads`,
    `--turn`. §10.4's variance reduction is measured at 28× and §16.5 is the
    result. Pairwise (§10.4's 4,851) is still not implemented.
23. Effect coverage pass: every deck card `modeled` or `inert`, deliberately,
    each inert one categorised (§4.4). **Replace §4.4's and §4.2's provisional
    counts with measured ones**, as §4.5 was — and check RULE K1's standings:
    if `enters_tapped` reached a fourth predicate shape during authoring, it is
    promoted and every `MANA_SOURCE` card is re-audited before this item closes.
24. Benchmark; record actual µs/game; revisit §2.4's estimate with real data.

Step 18 is where the project's real quality is decided. Everything before it is
mechanism; that step is where you find out whether the numbers mean anything.

---

## 16. Findings — what the model has said that reading the deck did not

Every other section of this document is about *how* to get a number. This one
records the numbers that came back and said something, and it is deliberately
short: most of what a simulator produces confirms what its author already
believed, and the entries worth writing down are the ones that did not.

**All figures: 20,000 games, seed 1, `P(assembled by turn N)`, measured by
declaring the cards in question `inert` and re-running.**

### 16.0 An ablation measures your implementation, not the card

**The strongest result in this section, and it is a limitation of the method
rather than a fact about the deck.** It applies to every number in §16.5,
including the ones that look right.

> **An ablation removes an *implementation*, not a card. A near-zero delta is
> therefore ambiguous between "this mechanism does not matter" and "this
> mechanism is built badly" — and those call for opposite responses. One says
> stop working on it; the other says start.**

#### How it was found: by being wrong about the headline

This section first reported that *Chord of Calling*'s convoke — the hardest cost
in the deck, deliberately authored last, given its own comment block — was worth
**−0.02 points** against the one line of TOML declaring the card a tutor, a
ratio of roughly **300 : 1**. It was written up as evidence that implementation
complexity is uncorrelated with contribution.

Re-measured at 200,000 games after two defects were fixed, both mine:

| | turn 3 | turn 6 | turn 12 |
|---|---|---|---|
| the tutor half | +1.83 | +6.02 | +7.67 |
| **convoke alone** | **+0.67** | **+0.66** | **+0.12** |
| ratio | **2.7 : 1** | 9.1 : 1 | 64 : 1 |

At the objective §4.1 recommends, convoke is worth **27% of the simple half**,
not 0.3%. And it is worth more than 90 of the 98 cards in the sweep.

**The defect that mattered was not in the card. Convoke was paid by a separate
loop that ran *before* the mana system**, tapping convokable creatures first,
always, needed or not. Once §6.4's payment assignment landed and convoke bodies
joined the payable sources, the planner chose between convoking a body and
tapping a land under one rule — and convoke started working.

**"Convoke is worth 0.02 points" was a fact about my code.** It was measured
correctly, reported honestly, and it was an artifact.

#### What follows for reading the sweep

**A near-zero ablation result is a hypothesis about the implementation before it
is a finding about the deck.** §16.5 now splits its table on that basis rather
than presenting 98 numbers as if they were alike.

There are **three** causes of a near-zero delta, not two, and the third is cheap
to rule out:

| Cause | How to tell | Response |
|---|---|---|
| The card genuinely does little | Near zero at *every* turn, and the implementation reads correctly in a trace | Believe it |
| **The implementation bypasses a shared system** | Near zero at every turn, and nobody has read a trace of it firing | **Read the trace before believing anything** |
| §4.1's blind spot: the objective cannot see it | Near zero at turn 3, **large at turn 12** | Believe it, and report the other turn |

The third is one command. *Basalt Monolith* is the only card in this deck that
answers to it: +0.02 against the null at turn 3, **+5.50 at turn 12**.

The cheap check for the second is the one that would have caught convoke:
**read a trace of the mechanism actually firing.** Convoke tapping creatures
before the mana system had been consulted is obvious in a single turn of output
and invisible in any aggregate.

#### The audit this prompted, and what it found

If convoke bypassed the mana system, what else bypasses a shared system? Every
authored effect was checked for a reader, and every path that puts a permanent
onto the battlefield was compared against the others. **Two more, both the
twelfth rule (`PLAN.md` §11.0), both now fixed:**

1. **`CARD_COST` had zero readers.** *Chrome Mox* exiles a nonland card from
   hand and *Mox Diamond* discards a land. Both were authored, both were
   validated as required by the loader, and **neither was ever charged** — the
   moxen were free for four phases. §4.2's RULE K2 singles this kind out as
   sitting on the value function's most sensitive input, which made it the worst
   one to leave uncalled. Charging it costs the deck 0.58 points at turn 3 and
   takes **Mox Diamond from +0.87 to +0.12** — out of the sweep's top eight.
2. **Four code paths put a permanent onto the battlefield and only one was
   complete.** The land drop applied `enters_tapped` *and* paid the life for
   entering untapped; the **fetch paid no life**, so a fetched *Breeding Pool*
   entered untapped for free while a played one cost 2; and the cast and
   tutor-to-battlefield paths checked neither. Now one `enter_battlefield`
   function, called by all four.

**The ordering claim from the original write-up survives with a smaller number.**
Convoke took a new predicate, a source-augmentation path in the scorer and three
tests; the tutor took one line. That is 64 : 1 at turn 12 and 2.7 : 1 at turn 3,
and in neither case did the effort ranking predict the contribution ranking. But
the *reason* it survives is now measured rather than asserted, and the first
measurement of it was wrong by two orders of magnitude.

### 16.1 Clones matter less than their count suggests

`CLONE` is the second-largest kind in the deck at 8 cards, and §4.2 makes a point
of the fact that a taxonomy derived from the format rather than from these 99
texts would have missed it. That was right about the *modelling* and it is a poor
guide to the *deck*.

The measurement that settles it is §16.5's sweep, not the leave-one-out figures
this section originally carried — those were taken against the wrong pattern and
the wrong payment rule, and are superseded rather than corrected.

**The reason is specific and is the finding, not the number.** In a Kinnan shell
a clone's job is copying a mana source. Copying a mana source only pays when mana
is the binding constraint — and for this deck it usually is not. What binds is
*finding Thrasios*, which is a tutor's job and not a clone's.

This was the first result in the project that was about the DECK rather than
about the model. Everything before it — the seeding, the pattern proxies, the
mana payment — was the simulator being wrong and then being less wrong.

### 16.2 One tutor is worth several clones

*Chord of Calling*, made inert, costs the deck **2.50 points at turn 3 and 7.79
at turn 12** (200,000 games). Against §16.5's clones, which sit at zero.

The cross-check is *Finale of Devastation*, the same effect already authored, and
§16.1's explanation predicts both: an X-cost creature tutor straight to the
battlefield finds Thrasios, and finding Thrasios is what the deck is short of.

### 16.3 Convoke is worth a quarter of the card's simple half

| 200,000 games | turn 3 | turn 6 | turn 12 |
|---|---|---|---|
| convoke alone | **+0.67** | **+0.66** | **+0.12** |

Corrected. §16.0 first reported this as −0.02 and the difference was a defect in
how convoke was paid, not in the card — see that section, which is now about the
correction rather than about the ratio.

At turn 3 convoke is worth more than 90 of the 98 cards in §16.5's sweep, which
is not "nothing". At turn 12 it is worth almost nothing, because by then the deck
has the mana anyway. Both are true and the turn is which one you get.

### 16.4 The metric is sensitive at turn 3 and numb at turn 12

Not a deck finding — a finding about how to read the other three, and it has
since been promoted into a decision (§4.1: the default objective is an early-turn
CDF point, with the blind spot that follows it).

The clearest demonstration is §6.4's payment fix. The shipped rule cost **0.68
points at turn 3 and 1.54 at turn 6, and 0.47 at turn 12** — the tail is
insensitive because by turn 12 the deck has enough real mana anyway, so getting
payment wrong changes mostly *when*.

The `wide_colour_into_kinnan_dig` correction shows the same shape from the other
side: removing the wrong requirement alone moved turn 3 from 9.47% to **20.17%**
and turn 12 from 67.19% to 67.59%. **A defect that more than doubled the early
number moved the tail by four-tenths of a point.** Anything reported as a single
summary number should therefore be an early-turn one; §10.1's insistence on the
whole CDF over a mean is doing more work than it looked like.

### 16.5 The sweep, and the answer to what §16.1 could not establish

98 leave-one-out ablations, 30,000 games each, paired against the baseline on the
same seed sequence, **against the corrected `wide_colour_into_kinnan_dig` and the
real payment assignment**. §10.4's variance reduction is 28× on the variance,
measured, which is what makes a third of a point resolvable at all.

#### The inert cards are a measured null, and that is what makes the table readable

The technique is written up as a technique in §9.4, because it generalises past
this sweep: **any model that forces its unrepresentable inputs to be declared has
a control group sitting inside it for free.** Ablating an inert card swaps a blank
for a Forest, so its delta is *exactly* the value of that swap and nothing else.

| | value |
|---|---|
| mean delta of the 31 inert cards, turn 3 | **−0.412%** |
| range across the 31 | −0.597% to −0.287% |

That number **is** §9.4's bias, in the units of the sweep: what a Forest is worth
over a blank card by turn 3. Without it the table is unreadable — at turn 3 a
Forest beats most of this deck, so almost every nonland reads negative and
nothing distinguishes "worse than a land" from "worse than nothing".

**Read the re-centred column against the spread, not against the interval.** The
31 nulls span 0.31 points, wider than any single paired interval, because a cheap
blank gets cast and wastes mana where an expensive one never does. "A blank card"
is not one number, so anything within about ±0.155 of zero on `vs blank` is not
distinguished from doing nothing.

#### The trusted list, and the unresolved one

**Split on §16.0's ambiguity rather than presented as 98 alike numbers.** A
near-zero delta does not mean a card does little; it means one of three things,
and only one of them is a result.

**TRUSTED — modelled cards measurably outside the ±0.108 band** (54 of 63; the
top of the list):

| Card | vs blank, turn 3 |
|---|---|
| Enduring Vitality | +6.79% |
| Chord of Calling | +2.25% |
| Nature's Rhythm | +1.78% |
| Invasion of Ikoria | +1.71% |
| Finale of Devastation | +1.64% |
| Chrome Mox | +1.49% |
| Mox Amber | +0.72% |
| Birds of Paradise | +0.57% |
| Tropical Island | +0.56% |
| Mox Diamond | +0.55% |
| … 44 more, mostly lands at ≈ +0.4 | |

**UNRESOLVED — modelled cards inside the band** (9 of 63). These are **not
results**. Each is ambiguous between *the card does little* and *its
implementation is wrong*, and none has had a trace of it firing read:

| Card | vs blank, turn 3 | Near zero at turn 12 too? |
|---|---|---|
| Mockingbird | +0.100 | yes |
| Flesh Duplicate | +0.077 | yes |
| Mirage Mirror | +0.050 | yes |
| Dramatic Reversal | +0.034 | yes |
| **Basalt Monolith** | +0.017 | **NO — +5.50** |
| Copy Enchantment | +0.017 | yes |
| Mirrormade | +0.014 | yes |
| Flash Photography | −0.003 | yes |
| Clever Impersonator | −0.030 | yes |

*Basalt Monolith* is resolved by the third column: it is §4.1's blind spot, not
an implementation question, and its turn-12 value settles it. **The other eight
are seven clones and a mass-untap, and they stay near zero at every turn**, which
rules out the blind spot and leaves the two causes entangled. Convoke was in
exactly this position and turned out to be the second one.

**EXPECTED — the four unauthored cards.** *Sylvan Library* (+0.030), *The One
Ring* (−0.053) and *Valley Floodcaller* (−0.073) are near zero because they do
nothing; that is the model working. *Thrasios* is at +3.567 despite being
unauthored, because it is a **pattern term** — the value is the outlet being on
the battlefield, which the pattern detects without the ability being modelled.

#### So what the clone finding now is — the trace was read

The claim §16.1 has been circling: **seven of the eight clones are
indistinguishable from a card declared to do nothing.** That is stable across
every reading. What was *not* established is why — clones genuinely doing little,
or `CLONE` having a convoke-shaped defect nobody had looked for. §16.0 says a
near-zero result is a hypothesis about the implementation until someone reads a
trace of the mechanism firing, so that was done.

**What the trace showed, and it looked like a defect:**

```
CAST: Flash Photography            paying 4
considering (clone target):
  Birds of Paradise            score  60000
  Kinnan, Bonder Prodigy       score 100000
  Mox Diamond                  score  50000
CLONE: Flash Photography becomes a copy of Kinnan, Bonder Prodigy
```

`choose_clone` picks by **authored rank**, and rank measures *how good is this
card to draw*. The question a clone asks is *how good is this permanent to have
twice*, and for the two highest-ranked permanents in the deck those are opposite:
a second *Kinnan* adds no multiplier (the model takes one modifier, and in real
Magic the legend rule kills it anyway), and a second *Enduring Vitality* grants
an ability every creature already has. **The scorer was systematically steering
clones at the two worst targets in the deck.**

**Then it was measured rather than believed**, by making `choose_clone` copy the
largest mana source instead of the highest rank:

| | turn 3 | turn 6 | turn 12 |
|---|---|---|---|
| copy the highest rank (ships) | 5.10% | 31.34% | 66.31% |
| copy the biggest mana source | 5.19% | 31.99% | 66.61% |
| individual clone ablations | −0.257% | | (unchanged, ±0.02) |

**The defect is real and worth about a tenth of a point.** Fixing the targeting
does not move any clone out of the band — it would take ~0.4 — so the answer to
§16.0's ambiguity, for this set, is **cause 1: the clones genuinely do little in
this deck.**

Stated with its limit: one alternative policy was tested, the one the trace
suggested. That does not rule out every implementation defect, but it does rule
out the obvious one, and it is more than a hypothesis with a number attached.

**A separate small gap the trace exposed, unmeasured:** the **legend rule is not
modelled**. A clone of *Kinnan* or *Thrasios* should die immediately. Here the
value comes out approximately right by accident — a second Kinnan contributes as
a vanilla creature rather than as nothing — so it has not been fixed, but it is a
real divergence and it is recorded rather than left to be rediscovered.

**Copy Artifact has now given three different answers across three readings**,
and the history is more useful than any one of them:

| Reading | Copy Artifact `vs blank` | Band | Verdict |
|---|---|---|---|
| First | +0.154 | ±0.17 | inside — *reported as outside, incorrectly* |
| Second, after the payment fix | +0.209 | ±0.155 | outside |
| Third, after the pattern and CARD_COST fixes | **+0.167** | **±0.108** | outside |

**The band is itself a measurement** — it is the spread of the 31 inert cards,
and it moves when the model changes. A card sitting near it can cross without
itself moving. The stable claim is *seven of eight at zero*; the eighth has been
on both sides of a line that is not fixed, and it is the one clone that can copy
*Basalt Monolith*.

#### The ranking is objective-dependent, and strongly

| Card | turn 3 | turn 6 | turn 12 |
|---|---|---|---|
| Enduring Vitality | +6.77% | — | — |
| **Basalt Monolith** | **−0.33%** | **+2.04%** | **+5.56%** |
| Thrasios, Triton Hero | +3.32% | — | — |

*Basalt Monolith* is half the deck's primary engine and reads **negative** at
turn 3 — it is colourless, does not untap, and a turn-3 cast buys nothing that
turn. By turn 12 it is +5.6.

This is §4.1's choice made concrete, and it cuts both ways. An early-turn
objective is the one that separates opening hands, which is what a mulligan
solver needs — and it correctly scores a mid-game engine piece near zero. Those
are the same fact. **A sweep is a ranking of cards against an objective, and
changing the objective is not a presentational choice**; the honest form names
the turn every time, which is why the column is `goldfish_turn_to_assembly_delta
at turn N` and never `score`.

### 16.6 A declared simplification is not a bounded one

The finding that should temper every number above.

§6.4 said mana sequencing was a constraint problem and deferred step 4
explicitly, in writing, as a known crudeness. The comment beside the code said
"crude" and "a policy question and a later one". Both true. **Neither is a
magnitude.**

Measured: the shipped rule cost **0.68 points of `P(assembled by turn 3)`**
against the correct assignment. **Eight of the 98 cards in §16.5's sweep are
worth more than that. Ninety are worth less.** For three phases the largest
single term in the model at the chosen objective was an arbitrary iteration
order, and nothing in the output distinguished it from the deck.

Two things generalise:

1. **Declaring a simplification makes it honest and does nothing to make it
   small.** The declaration and the measurement are different acts, and only the
   second one tells you whether the simplification mattered. Every "known
   simplification, stated with its direction" comment in `effects.toml` is
   currently an unmeasured one.
2. **Measure a simplification by perturbing it, not by reasoning about it.**
   Swapping one arbitrary rule for another costs ten minutes. Note that here it
   would have *under*-reported: the two crude rules differ by only 0.31 points
   from each other, and it took the correct implementation to show the error was
   0.68. **Arbitrariness bounds the error from below, not from above.**

And §16.0 is the same rule applied to a mechanism rather than a rule: convoke's
"worth 0.02 points" was an undeclared simplification — a mechanism paid by the
wrong code path — measured as though it were the card.

### 16.7 OPEN — the model does not represent repeatability at all

Raised while auditing Kinnan's abilities, and it is a design gap rather than a
bug, so it is recorded rather than fixed.

**How repeatability is represented now: it is not.** Patterns *detect a state*
and the metric is the turn that state is reached (§5.4), after which the game
stops being simulated. "How many times can this be activated" is outside the
measured quantity by construction. The one place the question is asked at all is
`loop_entry_cost`, which asks *can you enter the loop once*, and it is the right
question for *Kinnan + Basalt* because that loop untaps itself.

**It is the wrong question for Kinnan's dig, and in the direction that
overstates.** `{5}{G}{U}` has no tap symbol and no untap: each activation costs
seven mana, of which two must be `{G}` and `{U}` from sources that do **not**
renew. So a *Vitality* board buys some number of activations, not unbounded ones,
and `wide_colour_into_kinnan_dig` currently fires when **one** is payable.

Per §5.4 a win pattern means "a state from which a competent pilot wins". One
Kinnan activation looks at five cards and may put one non-Human creature onto the
battlefield. **That is a good turn, not an assembled combo** — and it is the
deck's *top-firing* pattern at ~37% of games. This is the most likely
overstatement left in the model.

Note the contrast that makes the typed flags (§2.6) look right again:
`infinite_C_into_thrasios` is genuinely unbounded, because *Thrasios* costs `{4}`
**generic** and unbounded `{C}` pays it every time. Kinnan's dig needs coloured
mana per activation and unbounded `{C}` cannot pay for it. **The two outlets are
not interchangeable and the model already knows that** — what it does not know is
how many activations "not interchangeable" buys.

Three ways to represent it, none chosen:

Labelled **R1/R2/R3** rather than A/B/C, because §13.1's chart options are
already A/B/C/D and the two sets were being read across each other.

| Option | What it says | Cost | What it gets wrong |
|---|---|---|---|
| **R1. Raise the entry cost to N activations** | `loop_entry_cost = 7`, `activations = N` | One number in the deck file, today | Still generic-only, so it never checks that `{G}{U}` is available *N* times. Crude, honest, and available now |
| **R2. A coloured, repeated entry cost** | A `Cost` rather than an int, times N | Extends §5.2's vocabulary — governance, and a re-audit of every pattern | Nothing much; it is the correct version of R1 |
| **R3. Execute the dig** | `TUTOR` with a five-card look, actually resolved | The largest: the policy has to choose targets, and each dig changes the board the next one sees | Turns a detect-model into an execute-model, which §5.4 deliberately is not |

#### PROBE RUN — and it collapses

100,000 games at each entry cost, `loop_entry_cost` as N × 7:

| entry cost | activations | turn 3 | turn 6 | turn 12 | dig fires |
|---|---|---|---|---|---|
| 7 | 1 | 8.08% | 37.69% | 67.08% | 36,773 |
| 14 | 2 | **5.11%** | **31.34%** | 66.65% | 33,188 |
| 21 | 3 | 4.60% | 19.75% | **60.39%** | 20,957 |
| 28 | 4 | 4.59% | 18.09% | 46.52% | 3,951 |

**Two activations instead of one costs 2.97 points at turn 3 and 6.35 at turn
6.** The stated criterion was: *if two activations are nearly as common as one,
A is enough; if it collapses, B is required.* It collapses, and by more than the
payment error, the CARD_COST error and every card in the sweep except three.

**So the cost model cannot stay undecided, and the open question is now narrower
and sharper: what is N?** That is a Magic question, not a modelling one — how
many digs does this line need before a competent pilot has won — and it is the
single largest unresolved number in the model. `wide_colour_into_kinnan_dig`
fires in ~37% of games at N = 1 and ~11% at N = 3.

Two things the probe settled on the way:

- **R1 is adequate for the mechanism.** Generic-only asks for 7N mana rather
  than `{5}{G}{U}` × N, and under WIDE_COLOUR every creature taps for any colour
  with Kinnan doubling it, so a board that can pay 7N can essentially always find
  the coloured pips among it. **R2 buys accuracy the deck cannot currently
  exercise**, and the R1/R2 gap is far smaller than the N = 1 / N = 2 gap.
- **R3 is not needed to decide this.** Executing the dig would answer *how many
  activations the deck gets*; it would not answer *how many it needs*, which is
  the question. R3 remains the right answer for a different question.

#### DECIDED: R1 with N = 2, after one more measurement

The proposal was N = 2 provisionally, conditional on checking whether the engine
partly funds itself — **because if a hit substantially paid for the next
activation, a constant N would be the wrong shape entirely**, and that is a
different finding rather than a different number.

Measured at the moment the pattern fires, over 20,000 firings:

| | |
|---|---|
| mean undrawn library | 84.6 cards |
| non-Human creatures still in it | 19.4 |
| **P(dig hits in the top five)** | **0.736** |
| mana per hit, under Kinnan + *Enduring Vitality* | **2.00, uniformly** |
| expected mana per activation | **1.47** |
| activation cost | **7** |
| **a hit funds** | **21% of the next activation** |

The mana figure has no variance worth averaging, and that is itself the finding:
WIDE_COLOUR *requires* Enduring Vitality, which grants every creature `{T}: add
one mana of any colour`, and Kinnan doubles it. **Every non-Human creature the
dig can find is worth exactly two mana**, whether it is *Birds of Paradise* or
*Emrakul*. There is no "typical hit" to average — they are all the same.

And the real figure is lower than 21%, because **summoning sickness is not
modelled** (see §16.8): a creature that just entered cannot tap for mana until
the next turn, so within a turn a hit funds **0%** of the next dig.

**So the engine does not pay for itself, a constant N is the right
parameterisation, and N = 2 stands.**

It is now a *declared* assumption rather than an arithmetic detail:
`loop_entry_cost = 7` is a fact read off the card, `activations = 2` is the
judgement, they are separate keys, `activations` is **required wherever
`loop_entry_cost` appears with no default**, and both are printed in the honesty
header beside `opponents` and `on_the_play`. `kinnan_basalt` declares
`activations = 1` explicitly, so the contrast — that loop untaps itself, this one
does not — is visible in the file rather than inferable.

### 16.8 What is still not established

- **Summoning sickness is not modelled.** A creature that enters the battlefield
  can tap for mana on the same turn — from a tutor, from a clone, or from
  Kinnan's dig. Under *Enduring Vitality* that is 2 mana per creature, arriving a
  full turn early. It **overstates**, it is on the deck's main line, and it is
  unmeasured. Found while measuring §16.7's self-funding question, where it is
  the difference between "a hit funds 21% of the next activation" and "0% within
  the same turn".
- **The legend rule is not modelled.** A clone of *Kinnan* or *Thrasios* should
  die. §16.5's trace reading found it; the value happens to come out about right,
  which is why it is here rather than in a fix.
- **The `activations` judgement is one number, declared, and worth 3 points.**
  §16.7 fixes it at 2 with the reasoning stated, and it is the single largest
  unmeasured *judgement* in the model — as opposed to the unmeasured
  *simplifications*, which §16.6 says are all of them.

#### Previously listed, still true

- The paired interval is **Wald on the discordant pairs**, not a score interval.
  §10.2 insists on Wilson for proportions because the normal approximation fails
  near 0 and 1; a *difference* near 0 is not that regime, and the report prints
  b and c so the asymptotics can be checked rather than trusted. At the top of
  the table b is in the hundreds or thousands; at the bottom it is single
  digits, and those rows lean on the total rather than on b alone.
- **No multiplicity correction is applied**, deliberately (§10.4, decision 1).
  98 comparisons at α = 0.05 expect ~5 spurious exclusions of zero, and the
  report says so where the stars are printed. Benjamini–Hochberg is the answer
  if a threshold is ever needed; a ranked list of "significant" cards is not.
- **Pairwise ablation is not implemented.** §10.4's 4,851 figure is C(99,2), and
  nothing here measures two cards removed together. Every number above is
  leave-*one*-out, which cannot see a card whose whole value is that it makes
  another card work.
- **Four cards are still unauthored** (Sylvan Library, The One Ring, Thrasios's
  own activated ability, Valley Floodcaller). Thrasios appears in the table
  because it is a *pattern term*, not because its ability is modelled; the +3.08
  is the value of the outlet being on the battlefield, which is what the pattern
  detects.
