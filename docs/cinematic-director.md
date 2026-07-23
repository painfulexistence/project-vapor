# Cinematic Director — a step-sequenced orchestration layer (Atmospheric-first)

Third in the convergence family (`animation-ui-convergence.md`,
`scene-convergence.md`). Those exchanged *finished halves* between the two
engines. This one is different: it designs a layer **neither engine has yet**,
and proposes building it clean in Atmospheric first — because Atmospheric is
the green field for it.

This is a plan. Nothing below is implemented.

---

## 1. The gap: a director sits above the timeline, beside the FSM

Animation splits along one axis — **is time mapped to an interpolated value,
or to a discrete effect?**

- A **timeline** samples a *continuous property* as a pure function of a
  playhead: position, rotation, colour, alpha. `evaluate(t)` is pure, so it
  seeks, reverses and ping-pongs. It writes a value every frame. This is
  `ActionTimelineComponent` + `Tween` + the `AnimationSubsystem` (and, in
  Vapor, `TimelinePlaybackComponent`). **A timeline is a beat.**
- A **director** schedules *discrete side effects* at points in time: "close
  the letterbox, wait until it's shut, show line 1, wait 3s, hide it, show
  line 2, … open the letterbox." The steps are one-shot effects and
  *waits-for-condition*, not interpolated values. Firing an effect is
  irreversible (you cannot un-play a sound by rewinding). **A director is the
  script that orders the beats.**

A cinematic is structurally a director's script: a mostly-linear sequence of
timed steps with side effects and waits. Today neither engine has this layer,
so cinematics are built out of whatever is at hand:

- **Vapor** hand-writes an FSM + a quartet of systems *per cinematic type*.
  Subtitles = `SubtitleQueueComponent` + a subtitle `FSMDefinition` +
  `SubtitleInputSystem` / `SubtitlePageSensorSystem` / `SubtitleTimerSystem` /
  `SubtitleActionSystem`. Letterbox, chapter-title, scroll-text each repeat
  the pattern. That is a lot of bespoke machinery to express what is
  fundamentally a `for` loop over lines with waits.
- **Atmospheric** has none of it — which is exactly why it is the right place
  to build the director as a first-class layer instead of retrofitting one
  under working code.

Encoding a linear script as an FSM state graph is the awkward part: the FSM is
a graph, the cinematic is a line. The FSM is the right tool for *reactive,
cyclic, branching* state (locomotion: idle↔walk↔run↔jump driven by input;
interruptible dialogue trees). It is the wrong tool for "do these ten things in
order, waiting between each."

### The clean-slate check (why Atmospheric)

| Primitive the director composes | In Atmospheric? |
|---|---|
| Continuous beats (`ActionTimelineComponent`, `Tween`, `AnimationSubsystem`) | ✅ present |
| UI beats (`UINavigator`, `UIPageComponent` show/hide intent) | ✅ present (A1) |
| Branching / reactive state (`FSMComponent`, `FSMStep`) | ✅ present (A2) — **and used by nothing yet** |
| Audio beats (`AudioSubsystem`), events (`MessageBus`) | ✅ present |
| **A cinematic implementation to disturb** | ❌ **none** |

Atmospheric has every ingredient a director orchestrates and no legacy
cinematic in the way. The FSM kit was literally ported *for* this pattern (its
header says "the cinematic orchestration pattern ported from Vapor's
subtitle/letterbox systems") and nothing was built on it. Green field.

### Naming — NOT "Sequence"

The obvious name is taken. Atmospheric's JSON action parser already has a
`"Sequence"` node (`application.cpp` `AppendActionToTween`): it lays Tween
segments end-to-end with `.Then()` — i.e. it builds one `ActionTimeline`. That
is squarely the **timeline** side of the split above (continuous property
tweens), so reusing "Sequence" for the *director* would collide both lexically
and conceptually. This layer is the **Cinematic Director**:
`CinematicComponent`, built from **steps**. "Sequence" keeps meaning
"sequential tween segments"; a cinematic is a script of steps.

---

## 2. The three-layer model

```
  Cinematic Director   ── imperative script: ordered steps + waits + fire effects
       │  starts beats, waits for them, emits events
       ▼
  Timeline / Tween     ── continuous property animation (a beat); pure fn of t; seekable
       ▲  emits discrete markers
       │
  FSM                  ── reactive / cyclic / branching state; feeds & is fed by events
```

- **Timeline** is the beat (a camera move, a fade). Already exists.
- **Director** is the script that fires beats in order, waits on conditions,
  and triggers side effects. New. Linear cutscenes live here.
- **FSM** is for branching/reactive orchestration. A director step can *emit*
  an event into an FSM (or the message bus), and a director can be *started
  by* an FSM transition — so branching flows nest a linear director inside an
  FSM state, and reactive interrupts sit above it. Timeline events
  (`TimelineEvent → FSMEventQueue` in Vapor; the `MessageBus` in Atmos) are
  the discrete markers that let a continuous beat poke the FSM mid-play.

The three compose; none replaces another. The director is the missing middle.

---

## 3. Step vocabulary

The primitives a `CinematicComponent` runs, in order. A step reports
`IsComplete()`; the executor advances the cursor when the current step
finishes.

| Step | Meaning | Absorbs |
|---|---|---|
| `Call(fn)` | fire a side effect (arbitrary code), completes immediately | the `…ActionSystem` glue |
| `Wait(seconds)` | delay | the `…TimerSystem` |
| `WaitUntil(pred)` | block until a predicate is true | the `…PageSensorSystem` |
| `Play(beat)` + `WaitDone()` | start a Tween/`ActionTimeline` and (optionally) wait for it | Timeline ⇄ Director composition |
| `Event(name)` | push to `MessageBus` / an `FSMEventQueue` | FSM/gameplay interop |
| `Parallel(steps…)` | run branches concurrently, join when all finish | "fade letterbox **while** moving camera" |
| `Loop(n)` / `ForEach(items, fn)` | repetition | the subtitle line loop |
| `Branch(pred, thenSteps, elseSteps)` | shallow conditional | small forks (keep deep branching in the FSM) |

`WaitUntil` is the load-bearing one: cinematics wait on *animation state* ("the
letterbox finished opening"), not just wall-clock, which is precisely what
Vapor's per-cinematic sensor systems do by hand. One general `WaitUntil`
absorbs all of them.

---

## 4. Execution model & builder

`CinematicComponent : Component` owns the step list, a cursor, and per-step
state. Two open design decisions, called out rather than hidden:

- **Who ticks it.** Self-tick via `OnTick` is simplest (few instances, not
  perf-critical). A small `CinematicSubsystem` is the alternative if central
  control is wanted (list active cinematics, skip/fast-forward from the
  editor). Recommendation: start self-ticked; promote to a subsystem only if
  the editor needs the roster.
- **Time scale.** A cutscene often must play **while gameplay is paused**. So
  the director should NOT blindly inherit the gameplay animation time-scale
  group — it needs its own group (or an explicit "ignore pause" flag). Decide
  per-cinematic; default to a dedicated `"cinematic"` group so pausing the
  world does not freeze the cutscene.

**Reversibility.** A director is *not* seekable backwards — side effects do not
un-fire (same limitation the current FSM cinematics have, so not a
regression). "Jump to step N" is supported by fast-forwarding `Wait`/`Play`
steps; whether `Call`/`Event` steps re-fire on a jump is a per-step
`replaySafe` flag.

Fluent builder (OOP, mirrors `Tween`'s ergonomics):

```cpp
Cinematic(go)
    .call([&]{ nav->Show(kLetterbox); })
    .waitUntil([&]{ return letterbox->IsOpen(); })
    .forEach(lines, [&](Cinematic& c, const Line& l) {
        c.call([&, l]{ subtitle->Set(l.speaker, l.text); nav->Show(kSubtitle); })
         .wait(l.duration)
         .call([&]{ nav->Hide(kSubtitle); })
         .waitUntil([&]{ return subtitle->IsHidden(); });
    })
    .call([&]{ nav->Hide(kLetterbox); })
    .play();   // attaches a CinematicComponent, runs once, then removes itself
```

That single script is the whole subtitle cinematic — replacing
`SubtitleQueue` + a subtitle FSM + four systems.

---

## 5. Data-authoring — the bigger prize (staged)

The builder above holds `std::function` steps, so it is **behavior-tier**: code
built, not JSON-authorable. That is fine for C1. The real ambition — the
"全部 entity 進 JSON" idea from the scene-convergence plan, applied to
cutscenes — is a **data step vocabulary**: step kinds as tagged structs whose
effects are *declarative references*, not closures:

```json
"cinematic": {
  "group": "cinematic",
  "steps": [
    { "show": "Letterbox" },
    { "waitUntil": { "page": "Letterbox", "state": "open" } },
    { "forEach": "subtitles", "do": [
        { "setSubtitle": "$item" }, { "show": "Subtitle" },
        { "wait": "$item.duration" }, { "hide": "Subtitle" },
        { "waitUntil": { "page": "Subtitle", "state": "hidden" } } ] },
    { "hide": "Letterbox" }
  ]
}
```

Every effect here is already resolvable from data by an existing primitive:
pages by `UINavigator`'s opaque ids, clips by `AnimationLibrary`'s by-name
lookup, events by `MessageBus` strings, timelines by name on a named entity.
So the data tier is "wire declarative references to the primitives," and it
composes with Atmospheric's `ComponentFactory` and the future scene cook. Two
tiers, both valid: code-built `Cinematic` (behavior) now; data-built
`"cinematic"` (authorable, cookable) later.

---

## 6. Why this beats extending the FSM

A subtitle cinematic is a `for` loop over lines with waits. As an FSM it
becomes a hand-rolled state graph plus systems that push events, sense page
state, and tick timers to walk that graph — four systems for a loop. The
director expresses the loop directly (`forEach` + `wait` + `waitUntil`). The
FSM stays for what it is actually good at: reactive interrupts and branching
(pause during a cutscene, a dialogue choice) — which nest around/inside a
director, not instead of it.

---

## 7. Relationship to Vapor

Vapor already *has* cinematics — as the per-type FSM+systems pattern. So the
direction reverses from the earlier rounds: the **design originates
Atmospheric-first** (it is the clean slate), and **Vapor adopts it later**,
collapsing `subtitle` / `letterbox` / `chapterTitle` / `scrollText` onto the
same director. That migration is deliberately out of scope here (the user
deferred it); this plan just makes it the obvious next step once the layer
exists and is proven.

Shared vocabulary to keep aligned across both repos:

- **Step kinds** (`Call`/`Wait`/`WaitUntil`/`Play`+`WaitDone`/`Event`/
  `Parallel`/`Loop`/`ForEach`/`Branch`) and their semantics.
- **`WaitUntil` predicate meaning** — waits on animation/UI *state*, the sensor
  role.
- **Effect-reference conventions** for the data tier — page ids, clip names,
  event strings, named-entity timelines — so a JSON cutscene reads the same on
  both engines even though the spelling of the surrounding component differs
  (Vapor ECS component vs Atmos `CinematicComponent`).
- **Time-scale group** — a dedicated `"cinematic"` group, so "cutscene plays
  during pause" is the shared default.

---

## 8. Phasing

| Phase | Contents | Proof |
|---|---|---|
| C1 | `CinematicComponent` + step executor (`Call`/`Wait`/`WaitUntil`/`Event`) + fluent builder; run-once, self-ticked; dedicated time-scale group | unit test: a scripted 3-step cinematic advances on wait + predicate, fires effects in order |
| C2 | Timeline composition (`Play` + `WaitDone`), `Parallel`, `Loop`/`ForEach`, shallow `Branch` | test: parallel join; a step that waits on a Tween's completion |
| C3 | The **subtitle** reference cinematic as one script (the sample `fsm.hpp` promised), wired on `UINavigator` + a subtitle `UIPageComponent` | the four Vapor subtitle systems replaced by one script, running in an example |
| C4 | Data step vocabulary → JSON-authorable `"cinematic"`; resolve effects from the existing primitives; cook integration | test: load → run a JSON cutscene; round-trip through the scene cook |
| (Vapor, later) | migrate subtitle/letterbox/chapterTitle/scrollText onto the shared director | Vapor's bespoke cinematic systems retire |

---

## 9. Risks / notes

- **Predicate lifetime** (`WaitUntil`, `Call`): closures capture game objects;
  a cinematic outliving its captured targets dangles. Cinematics are usually
  scoped to a scene — tie the `CinematicComponent`'s life to its owner and
  cancel on scene unload (the scene-convergence teardown work is the hook).
- **Reversibility**: side effects do not un-fire; do not promise backward
  scrubbing. Editor "jump to step N" fast-forwards deterministic steps and
  honours each step's `replaySafe` flag.
- **Linear vs branching**: keep `Branch` shallow. When a cutscene becomes a
  graph (dialogue trees, heavy interruption), that is the FSM's / a behavior
  tree's job — the director hosts the linear runs between decision points.
- **Do not reabsorb the tween "Sequence" node**: that stays a timeline-
  authoring construct (continuous property tweens). The director fires and
  waits on timelines; it does not replace how a single timeline is built.
- **Pause semantics**: verify the `"cinematic"` time-scale group actually
  bypasses the gameplay pause group in `AnimationSubsystem`, or the first
  cutscene-during-pause will silently freeze.
