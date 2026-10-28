"""blinky-server's integration with the Spark Job API (#288).

my-spark owns job execution, scheduling and every image. blinky submits jobs through its job
API and records their results: today the parameterized `separate` primitive (plans/0238); training,
as blinky's own code, once `gpu_task` exists (plans/0239). This server is the client's only home,
so one process owns the ledger and receives the Spark's webhooks.
"""
