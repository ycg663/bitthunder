GPIO_OBJECTS-$(BT_CONFIG_DRIVERS_GPIO_I2C_MAX7312) += $(BUILD_DIR)drivers/gpio/max7312.o


GPIO_OBJECTS += $(GPIO_OBJECTS-y)

$(GPIO_OBJECTS): MODULE_NAME="drivers-GPIO"
OBJECTS += $(GPIO_OBJECTS)
