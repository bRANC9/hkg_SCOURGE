# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
.PHONY: payload test vm-test clean

payload:
	sh scripts/fetch_jdk.sh
	sh scripts/build_payload.sh

test: payload
	python3 scripts/check_runtime.py "$$(ls -d out/usb/payload/java/runtime-* | head -1)"
	python3 tests/test_runtime.py
	sh tests/test_installer.sh

vm-test: payload
	sh tests/build_vm_payload.sh
	sh tests/run_vm_test.sh

clean:
	rm -rf build out
