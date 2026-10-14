# W5500 MACRAW 模式与 lwIP 协议栈接入 评估报告

> 分析日期:2026-10-13
> 目标芯片:ESP32-WROOM-32
> 开发环境:ESP-IDF v6.0.2
> 分析对象:`managed_components\w5500`(手动拉取的 `espressif/w5500` 组件,v2.0.0)
> 分析结论:**支持。W5500 组件明确工作在 MACRAW 模式,可作为 ESP32 的以太网卡接入 lwIP 协议栈;但当前只拉了 `w5500` 一个组件、缺少其强依赖 `wiznet_common`,且放置方式有误,直接编译会失败,需按第五节方案修正后即可使用。**

---

## 一、结论摘要

| 项目 | 结论 |
| --- | --- |
| W5500 是否支持 MACRAW(原始帧)模式 | ✅ 支持。驱动把 Socket0 配置为 `MAC_RAW` 模式,绕过芯片内置 TCP/IP 栈,直接收发原始以太网帧 |
| 是否能接入 ESP32 的 lwIP 协议栈 | ✅ 支持。组件实现标准 `esp_eth_mac_t` / `esp_eth_phy_t` 接口,与现有 LAN8720 走同一套 `esp_eth` + `esp_netif` + lwIP 框架 |
| 与现有 NAT 网关 / DHCP 服务器复用 | ✅ 完全复用。NAPT、DHCP 服务器、静态 IP、DNS 下发都在 `esp_netif` 层,与底层 MAC/PHY 无关,只需替换 MAC/PHY 两个 factory 函数 |
| IDF 版本要求 | ✅ 满足。组件要求 `idf >= 6.0`,本项目 `dependencies.lock` 确认 IDF 为 **6.0.2** |
| 依赖完整性 | ❌ **不完整**。组件强依赖 `espressif/wiznet_common ^1.0.0`,当前 `managed_components` 下只有 `espressif__lan87xx` 和 `w5500`,**没有 `wiznet_common`**,缺头文件与符号,编译必然失败 |
| 组件放置方式 | ⚠️ 有问题。`managed_components/` 是组件管理器的私有目录,手动塞入的目录在下一次 `idf.py reconfigure` 时可能被清理;且 `main/idf_component.yml` 已改回无 w5500 声明 |

---

## 二、组件构成与关键事实

手动拉取的组件为 Espressif 官方 `esp-eth-drivers` 仓库的 `w5500` 目录:

- 版本:`w5500/idf_component.yml` 中 `version: 2.0.0`(注意:这是仓库 **master 分支**的版本号,与注册表上已发布的 `espressif/w5500` 1.0.x 可能不同)。
- 依赖声明(`w5500/idf_component.yml`):
  ```yaml
  dependencies:
    idf: '>=6.0'
    espressif/wiznet_common:
      version: ^1.0.0
      require: public
      override_path: ../wiznet_common
  ```
- 源码文件:
  - `include/esp_eth_mac_w5500.h` —— 提供 `esp_eth_mac_new_w5500()` 与 `ETH_W5500_DEFAULT_CONFIG()`。
  - `include/esp_eth_phy_w5500.h` —— 提供 `esp_eth_phy_new_w5500()`。
  - `src/esp_eth_mac_w5500.c` / `src/esp_eth_phy_w5500.c` / `src/w5500.h`。
- 关键外部依赖:源码 `#include "wiznet_mac_common.h"`、`"wiznet_phy_common.h"`,并调用 `emac_wiznet_new()`、`wiznet_read()`、`wiznet_write()`、`phy_wiznet_new()` 等 —— **这些全部来自 `espressif/wiznet_common` 组件**,`w5500` 自身不包含它们。
- `w5500/CMakeLists.txt` 只 `PRIV_REQUIRES esp_eth`,并未自带 wiznet_common 的源码,印证 wiznet_common 必须作为独立组件存在。

---

## 三、MACRAW 模式支持确认(源码证据)

W5500 的「裸协议栈」能力来自其 Socket 的 **MACRAW 模式**:芯片不解析 IP/TCP/UDP,而是把链路上收发的原始以太网帧(含 MAC 头)透传给主机。

组件在初始化时明确打开该模式,`src/esp_eth_mac_w5500.c:147` 的 `w5500_setup_default()`:

```c
/* Set SOCK0 mode: MACRAW + MAC filter + block multicast */
uint8_t smr = W5500_SMR_MAC_RAW | W5500_SMR_MAC_FILTER | W5500_SMR_MAC_BLOCK_MCAST;
```

其中 `src/w5500.h:62` 定义 `#define W5500_SMR_MAC_RAW (1<<2)`,即 Socket0 的 `Sn_MR` 寄存器写入 `P3=1`(MACRAW 模式)。

同时 `README.md` 第 5 行明确说明:

> "W5500 provides a complete and hardwired TCP/IP stack (**not used in the ESP driver**) and 10/100 Ethernet MAC and PHY."

即:芯片自带的 TCP/IP 栈在 ESP 驱动中**不使用**,由 ESP32 的 lwIP 栈接管。这正是本项目需要的接入方式。

---

## 四、能否接入 ESP32 的 lwIP 协议栈

**能,且与现有 LAN8720 完全同一条路径。** 组件实现的是标准 `esp_eth` 框架的两个接口:

- `esp_eth_mac_new_w5500(&w5500_config, &mac_config)` → `esp_eth_mac_t *`(SPI MAC,负责收发以太网帧)
- `esp_eth_phy_new_w5500(&phy_config)` → `esp_eth_phy_t *`(W5500 内置 PHY,负责链路/速率/双工)

接入 lwIP 的流程与 `CODE\src\ethernet.c` 里 LAN8720 的写法一致,只需替换**第 2、3 步的 MAC/PHY factory**:

| 现有 LAN8720 写法 | W5500 替换写法 |
| --- | --- |
| `esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg)` | `esp_eth_mac_new_w5500(&w5500_config, &mac_cfg)` |
| `esp_eth_phy_new_lan87xx(&phy_cfg)` | `esp_eth_phy_new_w5500(&phy_cfg)` |

其余代码(`esp_eth_driver_install` → `esp_eth_new_netif_glue` → `esp_netif_attach` → `esp_eth_start` → 事件回调里的 `esp_netif_napt_enable` / `esp_netif_dhcps_start/stop`)**逐字不变**。

补充说明:

- W5500 出厂烧录了 MAC 地址,`ETH_MAC_DEFAULT_CONFIG()` 里的 MAC 配置可省略(README 明确)。
- W5500 走 **SPI** 接口(MOSI/MISO/SCLK/CS/INT/RST),而非 LAN8720 的 RMII 固定引脚;SPI 引脚可经 GPIO 矩阵任意映射,需按实际接线配置 `spi_bus_config_t` + `spi_device_interface_config_t`。
- README 建议把 `mac_config.rx_task_stack_size = 4096`,避免接收任务栈溢出。

---

## 五、当前阻塞问题(必须先解决)

1. **缺少 `wiznet_common` 组件(致命)**
   `w5500` 的源码直接 `#include` wiznet_common 的头文件并调用其函数,但 `managed_components` 下没有 `wiznet_common`。不补齐则编译报「找不到 `wiznet_mac_common.h` / `wiznet_phy_common.h`」及大量未定义符号。

2. **`managed_components` 放置方式不正确**
   `managed_components/` 由 IDF Component Manager 私有管理:每次 `idf.py reconfigure` 会按 `dependencies.lock` / 各 manifest 的解析结果同步该目录,未声明的目录会被**删除或忽略**。而当前 `main/idf_component.yml` 已改回只有 `espressif/lan87xx`,没有 `w5500` 声明,所以手动塞入的 `w5500` 不会稳定生效。

3. **之前 manifest 方式失败的疑似原因**
   上一轮在 `main/idf_component.yml` 里写的是 `espressif/w5500: '^1.0.0'`。而注册表上 `espressif/w5500` 的 1.x 是面向 IDF 5.x 的旧接口,与 IDF 6.0.2 及本仓库 master 的 2.0.0 接口不兼容(2.0.0 才拆分出 `wiznet_common`、引入 `esp_eth_mac_spi.h`)。**若走组件管理器,版本约束应写 `^2.0.0`(或 `*`),而非 `^1.0.0`。**

---

## 六、解决方案建议

### 方案 A(推荐,本地组件,最稳)

把 W5500 相关组件从 `managed_components` 移出,放到项目根 `components/` 作为**本地组件**,构建系统会自动发现、不受组件管理器清理:

```
router/
├─ components/
│  ├─ w5500/            ← 现有 managed_components/w5500 整个目录移过来
│  └─ wiznet_common/    ← 从 esp-eth-drivers 仓库同一次提交中一起拉取(必须补齐)
├─ main/
└─ ...
```

优势:

- `w5500/idf_component.yml` 里的 `override_path: ../wiznet_common` 恰好指向 `components/wiznet_common`,依赖自动满足,无需改 manifest。
- 不再依赖组件管理器联网解析,规避了本次 manifest 失败的问题。
- `main/idf_component.yml` 保持现状(仅 `espressif/lan87xx`)即可,无需再声明 w5500。

操作要点:

1. 从 `esp-eth-drivers` 仓库**同一 commit** 拉取 `wiznet_common` 目录,与 `w5500` 版本对齐(master 的 w5500 依赖 master 的 wiznet_common)。
2. 删除 `managed_components/w5500`(避免残留造成歧义)。
3. 确认 `components/wiznet_common/idf_component.yml` 里的依赖(尤其 `idf` 版本)满足当前 6.0.2。

### 方案 B(继续用组件管理器)

在 `main/idf_component.yml` 中用**正确版本**声明,让管理器自动解析 w5500 及其传递依赖 wiznet_common:

```yaml
espressif/lan87xx: '*'
espressif/w5500: '^2.0.0'
```

然后 `idf.py reconfigure`。若管理器因网络/镜像解析 wiznet_common 失败,退回方案 A。

> 选择建议:既然已手动拉取组件、且 manifest 方式上次失败,直接走**方案 A** 最省事、最可控。

---

## 七、W5500 特有配置注意(接入时)

1. **SPI 总线初始化**:需先 `spi_bus_initialize()`,再 `spi_bus_add_device()`,把 `spi_device_interface_config_t`(mode=0、时钟速率、CS 引脚、queue_size=16)传入 `ETH_W5500_DEFAULT_CONFIG(spi_host, &spi_devcfg)`。
2. **中断与轮询互斥**:`ETH_W5500_DEFAULT_CONFIG` 默认 `int_gpio_num = 4`、`poll_period_ms = 0`。源码 `esp_eth_mac_new_w5500()` 校验二者必须「一个有效、另一个无效」(`(int_gpio_num >= 0) != (poll_period_ms > 0)`)。不用中断就设 `int_gpio_num = -1` 并给 `poll_period_ms` 一个轮询周期。
3. **接收任务栈**:`mac_config.rx_task_stack_size = 4096`。
4. **PHY 地址**:W5500 内置 PHY,`eth_phy_config_t` 的 `phy_addr` 由驱动内部处理,无需像 LAN8720 那样手动探测。
5. **SPI 时钟**:参考 README,SPI 时钟可达数十 MHz(`clock_speed_hz = <MHz> * 1000 * 1000`),按实际走线逐步抬高。

---

## 八、参考文件

- `managed_components\w5500\README.md` —— 官方用法与「TCP/IP stack not used」说明
- `managed_components\w5500\idf_component.yml` —— 版本 2.0.0、依赖 wiznet_common
- `managed_components\w5500\src\esp_eth_mac_w5500.c:147` —— MACRAW 模式设置
- `managed_components\w5500\src\w5500.h` —— `W5500_SMR_MAC_RAW` 等寄存器定义
- `managed_components\w5500\include\esp_eth_mac_w5500.h` / `esp_eth_phy_w5500.h` —— 组件接口
- `CODE\src\ethernet.c` —— 现有 LAN8720 接入代码(替换点参考)
- `DOC\LAN8720以太网与DHCP服务器可行性分析.md` —— 同框架的 LAN8720 分析报告
