# Tasks

- [x] `BUDGET_SLACK` and `budget_reach`, reporting a budget over 6x its case's
      measurement with the class printed. Reported, never failed.
- [x] `gate_reach` judges the NORMALISED figure the gate decides on, not the raw
      one; `sdf_move` stops being reported as protected.
- [x] Re-derive `sdf_flatten`, `volume_flatten` and `volume_hpolish` — both
      `budgets` and `cases[]` — from the top of each case's band across valid
      bracketed records, not 1.5x one draw.
- [x] Leave `sdf_move`'s baseline alone and file the regression (#358).
- [x] `tools/check_doc_latency.py`, per-bundle tolerances, per-application
      figures, exemptions that must carry a reason and cannot outlive their row.
- [x] Regenerate `docs/09`'s table from the baseline; rewrite the `‡` footnote,
      which claimed every other row still carried the baseline's figure and had
      not been true since the noise-floor work.
- [x] Wire the checker into the CI `checks` job.
- [x] Tests: the guard's threshold and its baseline-internal failure mode; the
      normalised-vs-raw distinction; the doc checker's drift, batch, held-row
      and vanished-exemption cases.
