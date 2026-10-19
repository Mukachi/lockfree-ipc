# lockfree-ipc

Lock-free SPSC/MPMC queues and shared-memory transport for Linux, with a measurement harness
that reports latency percentiles against the machine's own noise floor.

Work in progress. Written from scratch against public documentation.

## Results

<!-- results:spsc-summary -->
SPSC figures are being re-measured: the earlier runs placed the queues on the far NUMA node (design note 008, Review).
<!-- /results -->

<!-- results:noise-floor -->
Noise floor: being re-measured (design note 008, Review).
<!-- /results -->
