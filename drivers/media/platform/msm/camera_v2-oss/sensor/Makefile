# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -I$(srctree)/drivers/media/platform/msm/camera_v2-oss
ccflags-y += -I$(srctree)/drivers/media/platform/msm/camera_v2-oss/common
ccflags-y += -I$(srctree)/drivers/media/platform/msm/camera_v2-oss/msm_vb2
ccflags-y += -I$(srctree)/drivers/media/platform/msm/camera_v2-oss/camera
ccflags-y += -I$(srctree)/drivers/media/platform/msm/camera_v2-oss/sensor/io
ccflags-y += -I$(srctree)/drivers/media/platform/msm/camera_v2-oss/sensor/cci
obj-$(CONFIG_MSMB_CAMERA) += cci/ io/ csiphy/ csid/ actuator/ eeprom/ ois/ flash/ ir_led/ ir_cut/
obj-$(CONFIG_MSMB_CAMERA) += laser_led/
obj-$(CONFIG_MSM_CAMERA_SENSOR) += msm_sensor_init.o msm_sensor_driver.o msm_sensor.o
