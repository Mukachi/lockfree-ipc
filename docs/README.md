# Documentation

Every component gets a design note before any code is written. Decisions with real alternatives
get an ADR.

- **Design notes** (`design/`): one file per component, sections Proposal, Design, Spec, Tasks,
  Review. Status moves Draft → Agreed → Built → Measured. Two review points: Draft → Agreed
  (approved before code) and Built → Measured (results checked against the Spec).
- **ADRs** (`adr/`): Context, Decision, Consequences, Status. Only for choices a reviewer might
  question.

Only Spec and Review are mandatory. Any section can be one line.

## Design notes

| # | Component | Status |
|---|---|---|
| [001](design/001-clock.md) | Clock | Measured |
| [002](design/002-histogram.md) | Histogram | Measured |
| [003](design/003-open-loop-load.md) | Open-loop load generator | Measured |
| [004](design/004-environment.md) | Environment capture | Measured |
| [005](design/005-noise-floor.md) | Noise-floor meter | Draft |

## Decision records

| # | Decision | Status |
|---|---|---|
| [0001](adr/0001-own-histogram.md) | Own histogram instead of HdrHistogram | Accepted |
