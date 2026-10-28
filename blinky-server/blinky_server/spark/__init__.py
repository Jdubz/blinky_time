"""blinky-server's integration with the Spark Job API (#288).

my-spark owns all job execution and every image (my-spark plans/0238). blinky submits
parameterized jobs and records their results; nothing of blinky's runs on the Spark. This server
is the client's only home, so one process owns the ledger and receives the Spark's webhooks.
"""
