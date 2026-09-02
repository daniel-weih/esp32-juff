.PHONY: setup doctor start test repo-check ble-build ble-test firmware-setup firmware-build firmware-flash

setup:
	./scripts/bootstrap_macos.sh

doctor:
	./scripts/doctor_macos.sh

start:
	cd host && npm start

test: repo-check
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

firmware-flash:
	./scripts/idf.sh flash monitor
