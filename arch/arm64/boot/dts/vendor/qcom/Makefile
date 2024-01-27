ifeq ($(CONFIG_BUILD_ARM64_DT_OVERLAY),y)
        dtbo-$(CONFIG_ARCH_SDM660) += \
		sdm660-mtp-external-codec-overlay.dtbo \
		sdm660-mtp-internal-codec-overlay.dtbo \
		sdm660-cdp-external-codec-overlay.dtbo \
		sdm660-cdp-internal-codec-overlay.dtbo \
		sdm660-qrd-external-codec-overlay.dtbo \
		sdm660-rcm-external-codec-overlay.dtbo \
		sdm660-rcm-internal-codec-overlay.dtbo \
		sda660-mtp-external-codec-overlay.dtbo \
		sda660-cdp-external-codec-overlay.dtbo \
		sda660-rcm-external-codec-overlay.dtbo

sdm660-mtp-external-codec-overlay.dtbo-base := sdm660-pm660l.dtb
sdm660-mtp-internal-codec-overlay.dtbo-base := sdm660-pm660l.dtb
sdm660-cdp-external-codec-overlay.dtbo-base := sdm660-pm660l.dtb
sdm660-cdp-internal-codec-overlay.dtbo-base := sdm660-pm660l.dtb
sdm660-qrd-external-codec-overlay.dtbo-base := sdm660-pm660l.dtb
sdm660-rcm-external-codec-overlay.dtbo-base := sdm660-pm660l.dtb
sdm660-rcm-internal-codec-overlay.dtbo-base := sdm660-pm660l.dtb
sda660-mtp-external-codec-overlay.dtbo-base := sda660-pm660l.dtb
sda660-cdp-external-codec-overlay.dtbo-base := sda660-pm660l.dtb
sda660-rcm-external-codec-overlay.dtbo-base := sda660-pm660l.dtb
else
dtb-$(CONFIG_ARCH_SDM660) += sdm660-sim.dtb \
	sdm660-internal-codec-cdp.dtb \
	sdm660-internal-codec-mtp.dtb \
	sdm660-internal-codec-rcm.dtb \
	sdm660-cdp.dtb \
	sdm660-mtp.dtb \
	sdm660-qrd.dtb \
	sdm660-rcm.dtb \
	sdm660-pm660a-cdp.dtb \
	sdm660-pm660a-mtp.dtb \
	sdm660-pm660a-qrd.dtb \
	sdm660-pm660a-rcm.dtb \
	sdm660-internal-codec-pm660a-cdp.dtb \
	sdm660-internal-codec-pm660a-mtp.dtb \
	sdm660-internal-codec-pm660a-rcm.dtb \
	sdm660-pm660a-sim.dtb \
	sda660-cdp.dtb \
	sda660-mtp.dtb \
	sda660-rcm.dtb \
	sda660-pm660a-cdp.dtb \
	sda660-pm660a-mtp.dtb \
	sda660-pm660a-rcm.dtb \
	sda660-pm660a-qrd-hdk.dtb \
	sdm660-headset-jacktype-no-cdp.dtb \
	sdm660-headset-jacktype-no-rcm.dtb \
	sdm660-pm660a-headset-jacktype-no-cdp.dtb \
	sdm660-pm660a-headset-jacktype-no-rcm.dtb \
	sdm660-usbc-audio-mtp.dtb \
	sdm660-usbc-audio-rcm.dtb
endif

ifeq ($(CONFIG_BUILD_ARM64_DT_OVERLAY),y)
dtbo-$(CONFIG_MACH_XIAOMI_LAVENDER) += sdm660-mtp-lavender-overlay.dtbo
sdm660-mtp-lavender-overlay.dtbo-base := sdm660-mtp-lavender.dtb
else
dtb-$(CONFIG_MACH_XIAOMI_LAVENDER) := sdm660-mtp-lavender.dtb
dtb-$(CONFIG_MACH_XIAOMI_TULIP) := sdm636-mtp-tulip.dtb
dtb-$(CONFIG_MACH_XIAOMI_WAYNE) := sdm660-mtp-wayne.dtb sdm660-mtp-jasmine.dtb
dtb-$(CONFIG_MACH_XIAOMI_WHYRED) := sdm636-mtp-whyred.dtb
endif

always		:= $(dtb-y)
subdir-y	:= $(dts-dirs)
clean-files	:= *.dtb *.dtbo
