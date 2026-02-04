---
applyTo: '**'
---
Provide project context and coding guidelines that AI should follow when generating code, answering questions, or reviewing changes.

	•	do not work in main
	•	do not push unless told
	•	commit after every change, with a clear message describing the change
	•	no deprecated JSON
	•	always review logs and gather evidence before making a change
	•	prefer a plan before making a change
	•	AP wifi is 192.168.35.5 

	## Priority Order (highest → lowest)
All design and implementation decisions must preserve this priority stack:

1) V1 connectivity (must stay connected)
2) BLE ingest/drain (lowest-latency path; never block)
3) Display updates (responsive; never block BLE)
4) Audio alerts (best-effort; may drop, must not block)
5) Metrics collection (bounded time; degrade gracefully)
6) Wi-Fi / Web UI (off by default; maintenance mode)
7) Logging / persistence (best-effort; drops ok, corruption not; never block above)