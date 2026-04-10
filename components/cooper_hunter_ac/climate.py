import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, uart
from esphome.const import CONF_ID

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["climate"]

cooper_hunter_ns = cg.esphome_ns.namespace("cooper_hunter")
CooperHunterAC = cooper_hunter_ns.class_(
    "CooperHunterAC", climate.Climate, cg.PollingComponent, uart.UARTDevice
)

CONFIG_SCHEMA = (
    climate.climate_schema(CooperHunterAC)
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.polling_component_schema("5s"))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    await climate.register_climate(var, config)
