# ExoAnchor -> Codex-like Refactor

## Target

Make ExoAnchor behave like a modern agent runtime:

- one backend execution model
- one event stream shared by CLI and UI
- durable runs with resume / confirm / audit
- domain logic moved out of ad-hoc route handlers
- headless mode that feels like Codex / OpenHands, not like a polling wrapper

## Current Pain

The current system works, but several important behaviors are still in the wrong layer:

- `test_server.py` contains too much request-specific orchestration
- the web UI has historically owned too much execution state
- CLI and dashboard did not share one runtime event contract
- workload resolution, slot-filling, and Minecraft-specific patches are mixed together
- confirmation UX depended on fragile modal timing instead of durable state + explicit controls

## Architecture Direction

### 1. Runtime Core

Create a backend runtime layer that owns:

- session state
- event emission
- pause / resume / confirm / abort
- durable snapshots
- audit trail

This layer should not know about HTML.

### 2. Event-first Execution

Every meaningful state change should be an event:

- session created
- intent parsed
- plan created
- step started
- step finished
- confirmation required
- confirmation accepted / rejected
- run completed / failed

UI and CLI should only render these events.

### 3. Intent Pipeline

Split natural-language handling into explicit stages:

1. context hydration
2. workload resolution
3. slot filling / missing-info questions
4. planner or skill selection
5. execution

The important point is that these stages should be reusable and testable without the dashboard.

### 4. Resolver / Slots / Domain Skills

Do not keep hardcoded product logic inside the chat route.

Instead:

- workload matching becomes a reusable resolver module
- missing parameter detection becomes a generic slot-filling layer
- domain behaviors like Minecraft admin become skills or policy-aware resolvers

### 5. Headless First

The command line path should be a first-class runtime:

- attach to a run
- stream JSONL events
- approve dangerous steps
- run without the browser

If headless mode is solid, the web UI becomes a view, not the orchestrator.

## Phase Plan

### Phase A: Runtime Skeleton

- unified runtime event hub
- streaming event API
- CLI consumes event stream
- confirmation state is durable and observable

### Phase B: Session API

- add a server-owned session endpoint that combines parsing + execution
- remove route-local branching from CLI and dashboard
- make `ask` create a durable agent session, not just a parsed result

### Phase C: Intent Refactor

- extract workload resolver from `test_server.py`
- extract slot-filling / clarification rules
- isolate model prompting from execution policy

### Phase D: Tooling and Memory

- enrich structured observations
- persist resolver facts and session summaries
- enable better recovery and cross-turn continuity

### Phase E: UI Simplification

- dashboard consumes runtime events only
- no frontend-owned orchestration loops
- no special-case confirmation logic beyond rendering runtime state

## Non-goals

These are tempting, but should not be the first move:

- adding more Minecraft hardcoding
- adding more UI-only fixes without backend cleanup
- packing more logic into `llm_chat`
- making the model prompt bigger instead of making the runtime cleaner

## Success Criteria

We are closer to "Codex" when:

- the same task can run from CLI and UI with the same event semantics
- confirmation is durable and visible from any client
- a request can be attached to, resumed, and audited
- adding a new domain does not require more route-level if/else patches
- the browser can refresh without losing the run
