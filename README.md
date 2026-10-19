# lockfree-ipc

Lock-free SPSC/MPMC queues and shared-memory transport for Linux, with a measurement harness
that reports latency percentiles against the machine's own noise floor.

Work in progress. Written from scratch against public documentation.

## Results

<!-- results:spsc-summary -->
SPSC figures are being re-measured after two fixes to the harness: memory placement, and false sharing in the load generator (design note 008, Review).
<!-- /results -->

<!-- results:noise-floor -->
Noise floor on the reserved CPUs: at most 0.0003% of time stopped, longest stop 23.1 µs.
<!-- /results -->
