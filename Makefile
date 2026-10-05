# VectorTick Makefile

# Default build type
BUILD_TYPE ?= Release
BUILD_DIR ?= build

# CMake command
CMAKE ?= cmake

# Number of parallel jobs
NPROC ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Configure
.PHONY: configure
configure:
	@echo "Configuring VectorTick..."
	$(CMAKE) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Build
.PHONY: build
build: configure
	@echo "Building VectorTick..."
	$(CMAKE) --build $(BUILD_DIR) --parallel $(NPROC)

# Test
.PHONY: test
test: build
	@echo "Running tests..."
	cd $(BUILD_DIR) && ctest --output-on-failure --parallel $(NPROC)

# Sanitizers
.PHONY: sanitize
sanitize:
	@echo "Running with ASan + UBSan..."
	$(MAKE) clean
	$(CMAKE) -B $(BUILD_DIR)-sanitize -DCMAKE_BUILD_TYPE=Debug \
		-DENABLE_ASAN=ON -DENABLE_UBSAN=ON
	$(CMAKE) --build $(BUILD_DIR)-sanitize --parallel $(NPROC)
	cd $(BUILD_DIR)-sanitize && ctest --output-on-failure

.PHONY: tsan
tsan:
	@echo "Running with TSan..."
	$(MAKE) clean
	$(CMAKE) -B $(BUILD_DIR)-tsan -DCMAKE_BUILD_TYPE=Debug \
		-DENABLE_TSAN=ON
	$(CMAKE) --build $(BUILD_DIR)-tsan --parallel $(NPROC)
	cd $(BUILD_DIR)-tsan && ctest --output-on-failure

# Fuzz smoke test
.PHONY: fuzz-smoke
fuzz-smoke: build
	@echo "Running fuzz smoke tests..."
	$(BUILD_DIR)/bin/vectortick_tests --suite=fuzz_smoke_tests

# Demo
.PHONY: demo
demo: build
	@echo "Running demo..."
	@mkdir -p artifacts/demo
	$(BUILD_DIR)/bin/vectortick_demo 2>&1 | tee artifacts/demo/transcript.txt

# Dataset
.PHONY: dataset
dataset: build
	@echo "Generating dataset..."
	python3 tools/generate_events.py --count 100000 --output artifacts/dataset.bin

# Benchmark
.PHONY: benchmark
benchmark:
	python3 tools/run_verification.py --build $(BUILD_DIR) --output results/verified/local

# Profile
.PHONY: profile
profile: build
	@echo "Profiling..."
	@if command -v perf >/dev/null 2>&1; then \
		perf stat -e cycles,instructions,cache-misses,branch-misses \
			$(BUILD_DIR)/bin/vectortick_bench 2>&1 | tee results/profile.txt; \
	else \
		echo "perf not available, skipping"; \
	fi

# Stress
.PHONY: stress
stress: build
	@echo "Running stress tests..."
	$(BUILD_DIR)/bin/vectortick_tests

# Acceptance
.PHONY: acceptance
acceptance:
	python3 tools/run_verification.py --build build-final --output results/verified/local --job-name local-aarch64-release
	python3 tools/run_verification.py --build build-san-final --build-type Debug --sanitizers --output results/verified/local-san --job-name local-aarch64-sanitizers

.PHONY: verify
verify:
	python3 tools/make_reports.py --verify-only

# LOC
.PHONY: loc
loc:
	@echo "Counting substantive lines of code..."
	python3 tools/count_substantive_loc.py

# Clean
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(BUILD_DIR)-sanitize $(BUILD_DIR)-tsan

# Format
.PHONY: format
format:
	@if command -v clang-format >/dev/null 2>&1; then \
		find include src apps tests fuzz bench -name "*.cpp" -o -name "*.hpp" | \
		xargs clang-format -i; \
	fi

# Tidy
.PHONY: tidy
tidy: build
	@if command -v clang-tidy >/dev/null 2>&1; then \
		clang-tidy -p $(BUILD_DIR) compile_commands.json; \
	fi

# Help
.PHONY: help
help:
	@echo "VectorTick Build System"
	@echo ""
	@echo "Targets:"
	@echo "  configure   - Configure the build"
	@echo "  build       - Build the project"
	@echo "  test        - Run tests"
	@echo "  sanitize    - Run with ASan + UBSan"
	@echo "  tsan        - Run with ThreadSanitizer"
	@echo "  fuzz-smoke  - Run fuzz smoke tests"
	@echo "  demo        - Run the demo"
	@echo "  dataset     - Generate 100M event dataset"
	@echo "  benchmark   - Run benchmarks"
	@echo "  profile     - Profile execution"
	@echo "  stress      - Run stress tests"
	@echo "  acceptance  - Run full acceptance suite"
	@echo "  verify      - Verify evidence bundle"
	@echo "  loc         - Count lines of code"
	@echo "  clean       - Clean build artifacts"
	@echo "  format      - Format source code"
	@echo "  tidy        - Run clang-tidy"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD_TYPE  - Build type (Debug/Release/RelWithDebInfo)"
	@echo "  BUILD_DIR   - Build directory (default: build)"
	@echo "  NPROC       - Number of parallel jobs"
