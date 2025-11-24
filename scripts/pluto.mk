
# Target specific constants go here

HDF_URL:=http://github.com/analogdevicesinc/plutosdr-fw/releases/download/${LATEST_TAG}/system_top.hdf
# hyojun: we use our Device Tree Blob.
TARGET_DTS_FILES:= zynq-pluto-sdr-fmsdr.dtb zynq-pluto-sdr-revb-fmsdr.dtb zynq-pluto-sdr-revc-fmsdr.dtb
COMPLETE_NAME:=PlutoSDR
ZIP_ARCHIVE_PREFIX:=plutosdr
DEVICE_VID:=0x0456
DEVICE_PID:=0xb673

