.PHONY: setup doctor start test repo-check ble-build ble-test firmware-setup firmware-build firmware-build-35 firmware-build-154 firmware-build-all firmware-package firmware-flash firmware-tools-test

PYTHON ?= python3
export JUFF_BOARD

setup:
	./scripts/bootstrap_macos.sh

doctor:
	./scripts/doctor_macos.sh

start:
	cd host && npm start

test: repo-check
	$(MAKE) firmware-tools-test
	cd host && npm test
	./scripts/build_macos_ble.sh
	./macos/build/JuffBLE.app/Contents/MacOS/JuffBLE --self-test

repo-check:
	./scripts/check_repository.sh

ble-build:
	./scripts/build_macos_ble.sh

ble-test:
	./macos/build/JuffBLE.app/Contents/MacOS/JuffBLE --self-test

firmware-setup:
	./scripts/setup_esp_idf.sh

firmware-build:
	./scripts/idf.sh build

firmware-build-35:
	$(MAKE) firmware-package JUFF_BOARD=waveshare-lcd-3.5

firmware-build-154:
	$(MAKE) firmware-package JUFF_BOARD=waveshare-lcd-1.54

firmware-build-all: firmware-build-35 firmware-build-154

firmware-package: firmware-build
	$(PYTHON) ./scripts/package_firmware.py "$(JUFF_BOARD)"

firmware-flash:
	@test -n "$(PORT)" || { echo 'Specify both JUFF_BOARD and PORT for the target device.' >&2; exit 1; }
	./scripts/idf.sh -p "$(PORT)" flash monitor

firmware-tools-test:
	$(PYTHON) -m unittest discover -s scripts/tests -p 'test_*.py'
