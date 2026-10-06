# Readiness refusal diagnostics

The fixture compiles the production first-reset capture, standard-resource
destruction wrapper, adapter destruction and reset reconciliation with controlled
OS and transport peers. It checks every destruction stage, UNMAP-before-UNREF,
unchanged ledger on admission refusal, first-record retention, hidden partial
publication and capture before reset admission changes.

Run `python3 viogpu/tests/readiness-diagnostic/run.py --negative-controls`.
Four separately compiled changes must be detected: reset-record overwrite,
destroy-record overwrite, wrong UNMAP attribution and capture after reset.
The existing synchronous-timeout fixture covers adapter queue admission,
its exact mutex wait budget, poison/recovery and common-clock timeout capture.

These checks do not prove target readiness, interrupt delivery or rendering.
