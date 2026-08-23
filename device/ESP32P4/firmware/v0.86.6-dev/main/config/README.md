# 固件配置层

本目录是源码层唯一配置边界：

- `board_config.h`：把生成的 Kconfig 值映射为驱动可依赖的稳定名称。
- `app_config.h`：应用身份与非硬件默认值。
- `storage_layout.h`：持久化文件系统路径。

不要在驱动中添加板级 GPIO 回退值。新增板级配置时，先在 `../Kconfig.projbuild` 声明 Kconfig 符号，再在 `../../configs/boards/` 的板型配置中赋值。

## Locator LED 第二 GPIO

- `CONFIG_SI_POWER_LOCATOR_GPIO` 是主 GPIO；`CONFIG_SI_POWER_LOCATOR_RETURN_GPIO` 是可选的第二 GPIO，单端 LED 保持 `-1`。
- 双 GPIO 模式下，关闭时两路都为低；正向开启时主 GPIO 为高；反转后第二 GPIO 为高。
- 反转值保存在 `si_power` NVS 命名空间的 `loc_rev` 键，由 `locator_reverse_on` / `locator_reverse_off` 动作和 Settings 设备设置共用。
- 设备设置中的 PWR/RST 交换通过 GPIO map 接口原子交换当前 `power_button` / `reset_button` 引脚并持久化；再次点击会交换回来，不改两路各自的有效电平。
- 只有经网表确认并在板型 overlay 中显式配置了第二 GPIO 的硬件才会开放反转控件。
