PORT ?= /dev/ttyUSB0
IDF_ACTIVATE ?= /home/danielro/.espressif/tools/activate_idf_v6.0.1.sh

.PHONY: principal-build principal-flash principal-monitor cam-build cam-flash cam-monitor status

principal-build:
	bash -c 'source "$(IDF_ACTIVATE)" && cd firmware/esp32_principal && idf.py build'

principal-flash:
	bash -c 'source "$(IDF_ACTIVATE)" && cd firmware/esp32_principal && idf.py -p $(PORT) flash'

principal-monitor:
	bash -c 'source "$(IDF_ACTIVATE)" && cd firmware/esp32_principal && idf.py -p $(PORT) monitor'

cam-build:
	bash -c 'source "$(IDF_ACTIVATE)" && cd firmware/esp32_cam && idf.py build'

cam-flash:
	bash -c 'source "$(IDF_ACTIVATE)" && cd firmware/esp32_cam && idf.py -p $(PORT) flash'

cam-monitor:
	bash -c 'source "$(IDF_ACTIVATE)" && cd firmware/esp32_cam && idf.py -p $(PORT) monitor'

status:
	git status
