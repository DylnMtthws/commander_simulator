# Resource probe v1

Additive `cs --resource-request FILE` and authenticated `/resource-simulate`.
The existing `/simulate` and its goldfish result contract are unchanged.
`GET /capabilities` describes the new route. Configure `SIM_RESOURCE_TOKEN` to
enable HTTP execution; otherwise it is disabled. It shares the existing bounded
admission semaphore (busy returns 429), caps games at 20,000 and execution at 10s.
No database access or export is needed: inputs are versioned compiled facts.

This controlled experiment plays opening seven, draws on every turn including
turn one, makes land drops, and casts the commander if its colored cost can be
paid. It executes only explicitly declared commander extra-land/draw effects.
Other spells, interaction, opponents and combat are outside the scenario. Only
unconditional one-mana lands (optionally always tapped) are supported. It is
not a full goldfish deck evaluation, commander casting prediction in real play,
or win rate. Unknown land shapes must be refused by the compiler, not blanked.

The generator compiles public card facts conservatively; the service checks
wire shape and bounds but does not certify caller-supplied facts against Oracle.
Keep this a private authenticated operator boundary. The ordered-slot identity
is independently checked; a complete input hash captures scenario version,
compiled effects, cost, games, seed and slot ordering. Slot order is retained for
paired random-number comparisons across substitutions. The 99-card order hash
is specific to this probe and is distinct from the existing deck_sha256 contract.

All statistics identify this scenario. Candidates changing nonland cards cannot
be ranked with it. Generator comparisons change only basic-land colors, keep
all spells, use conservative paired fixed-budget intervals, and confirm the
screened choice using separate seeds. Future capability expansions must revise
the scenario/compiler version and invalidate caches.
