PYTHON := ./venv/bin/python
PIP := ./venv/bin/pip

.PHONY: setup test lint typecheck format check ci clean

## Setup development environment
setup: venv
	$(PIP) install -e ".[dev,ble]"

venv:
	python3 -m venv venv

## Run all tests
test:
	$(PYTHON) -m pytest tests/ -v --tb=short

## Run tests with coverage summary
test-coverage:
	$(PYTHON) -m pytest tests/ -v --tb=short -q

## Lint with ruff
lint:
	$(PYTHON) -m ruff check blinky_server/ tests/

## Auto-fix lint issues
lint-fix:
	$(PYTHON) -m ruff check --fix blinky_server/ tests/

## Format with ruff
format:
	$(PYTHON) -m ruff format blinky_server/ tests/

## Check formatting (no changes)
format-check:
	$(PYTHON) -m ruff format --check blinky_server/ tests/

## Type check with mypy
typecheck:
	$(PYTHON) -m mypy blinky_server/

## Run all checks (CI equivalent)
check: lint format-check typecheck test

## CI verification (same as check, fails on first error)
ci: check
	@echo "All checks passed."

## Start the server
run:
	$(PYTHON) -m blinky_server

## Start with debug logging
run-debug:
	$(PYTHON) -m blinky_server --log-level debug

## Clean build artifacts
clean:
	find . -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
	find . -type d -name .mypy_cache -exec rm -rf {} + 2>/dev/null || true
	find . -type d -name .pytest_cache -exec rm -rf {} + 2>/dev/null || true
	find . -type d -name .ruff_cache -exec rm -rf {} + 2>/dev/null || true
