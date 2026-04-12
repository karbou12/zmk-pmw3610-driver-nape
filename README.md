# PMW3610 driver implementation for ZMK

## はじめに

本リポジトリは、ZMK 0.4 (Zephyr 4.1) に対応したNapeオリジン用のPMW3610 driverリポジトリをForkして、OctaShiftとTraboShiftを搭載したものです。  
driver変更に伴い、機能・用語・Configなどを変更しています。

## 機能概要

### OctaShift

Nape ProのOctaShiftのように、キーを押すことで、Napeの向きを時計回り、反時計回りに変更できます。

### TraboShift

Napeを使用したい向きに置いてから、TraboShift用レイヤーにいる状態で、トラックボールを下から上に勢いよく転がすことで、Napeの向きを変更できます。

## Napeの設定方法

Fork した `zmk-config-nape` リポジトリで作業します。

### driverの変更

`config/west.yml` で membou0202さんの `zmk-pmw3610-driver-nape` が指定されている箇所を 本driverを指定するように修正してください。

```diff
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: menbou0202
      url-base: https://github.com/menbou0202
    - name: caksoylar
      url-base: https://github.com/caksoylar
+    - name: karbou12
+      url-base: https://github.com/karbou12
    # Additional modules containing boards/shields/custom code can be listed here as well
    # See https://docs.zephyrproject.org/3.2.0/develop/west/manifest.html#projects
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-pmw3610-driver-nape
+      remote: karbou12
+      revision: develop
-      remote: menbou0202
-      revision: main
    - name: zmk-rgbled-widget
      remote: caksoylar
      revision: main
  self:
    path: config
```

[参考](https://github.com/karbou12/zmk-config-nape/blob/develop/config/west.yml)

### Config設定

`config/boards/shields/nape/nape.conf` に、必要に応じて各設定値を追加してください。

[参考](https://github.com/karbou12/zmk-config-nape/blob/develop/config/boards/shields/nape/nape.conf)

#### 必須

```ini
# TraboShift, OctaShift両方に必要
CONFIG_PMW3610_ALT_TRABO_SHIFT=y

# OctaShiftに必要。TraboShiftだけ使用したい場合は不要。
CONFIG_ZMK_BEHAVIOR_PMW3610_ROTATION=y
```

#### オプション

```ini
# TraboShift検出時間。デフォルトは100ms。検出時間内に閾値距離以上移動したらその向きに設定します。
CONFIG_PMW3610_ALT_TRABO_SHIFT_SAMPLE_TIME_MS=100

# 検出距離の閾値。デフォルトは800pixel (単位は多分)。検出時間内に閾値距離以上移動したらその向きに設定します。
CONFIG_PMW3610_ALT_TRABO_SHIFT_DISTANCE_THRESHOLD=800

# TraboShift用レイヤー。デフォルトは11で、Napeの向き切替えレイヤーと同じにしています。
CONFIG_PMW3610_ALT_TRABO_SHIFT_LAYER=11

# TraboShift 1方向あたりの角度。デフォルトでは45度 (360/45 = 8方向) になっています。3度から45度まで設定可能です。
# 360度を割り切れる数にしてください。割り切れない数が設定された場合、内部で割り切れるまで角度を増加します。
CONFIG_PMW3610_ALT_TRABO_SHIFT_ANGLE=45
```

### OctaShiftキーの設定

`config/nape.keymap` で `dtsi`ファイルをincludeしてください。
```c
#include <behaviors/pmw3610_rotation.dtsi>
```

以下のbehaviorを準備していますので、適宜使用してください。

|behavior|パラメータ|説明|
|---|---|---|
|`&tb_rot_45`|`ROT_CW`|Napeを時計回りに回転|
|`&tb_rot_45`|`ROT_CCW`|Napeを反時計回りに回転|

[参考](https://github.com/karbou12/zmk-config-nape/blob/develop/config/nape.keymap)

ZMK Studio では以下の表記となります。Keymap Editorでは表示・設定できませんでした。

|keymap|ZMK Studio|
|---|---|
|`&tb_rot_45`|PMW3610 Trackball Rotation with 45 degree|
|`ROT_CW`|Clockwise|
|`ROT_CCW`|Counter-Clockwise|


角度を変更したbehaviorを自作したい場合は、以下を`config/nape.keymap`ファイルに記述し、`step-angle-degree`に任意の角度を設定してください。

```c
/ {
    behaviors {
        tb_rot_15: tr15 {
            compatible = "zmk,behavior-pmw3610-rotation";
            #binding-cells = <1>;
            step-angle-degree = <15>;
        };
    };
};

```

## ご参考

### Nape起動時のdefault layerの設定方法

driver変更に伴い、Nape起動時のdefault layerの設定方法が変更になっています。  

#### `nape.overlay`ファイルを直接編集する場合

`config/boards/shields/nape/nape.overlay` の `default-orientation-layer`を変更してください。  
なお、その上に`snape-layers`がありますが、defaultの`11`はTraboShift用レイヤーと同じなので、使用したい場合は他の値にしてコメントを外してください。私は`12`にしています。

`config/boards/shields/nape/nape.overlay`
```c
&spi0 {
...
    trackball: trackball@0 {
...
        // Snipe layers: uncomment and fill in layer numbers to enable snipe mode
        // snipe-layers = <11>;

        // Default orientation layer at boot (0 = 0°, 1 = 45°, ... 7 = 315°)
        default-orientation-layer = <0>;
    };
```

#### `nape.overlay`ファイルを直接編集せず、`nape.keymap`ファイルを編集したい場合

まず、`nape.overlay`の`trackball_listener` にlabelを付けてください。これをしないとコンパイルエラーになります。

`config/boards/shields/nape/nape.overlay`
```diff
/ {
...
+    trackball_listener: trackball_listener0 {
-    trackball_listener {
...
```

その後、`nape.keymap`ファイルに以下を追加してください。

`config/nape.keymap`
```c
&trackball {
    snipe-layers = <12>; // snape-layerを使わないなら削除

    // Default orientation layer at boot (0 = 0°, 1 = 45°, ... 7 = 315°)
    default-orientation-layer = <6>; // 起動時に設定したいlayerに変更する
};

&trackball_listener {
    compatible = "zmk,input-listener";
    device = <&trackball>;
};
```

### RGBLEDの設定

Napeはレイヤーが変わるとLEDがレイヤーの数だけ点滅するようになっていますが、私はレイヤー毎に色を変更するように`config/boards/shields/nape/nape.conf`を修正しています。  
色の指定については[zmk-rgbled-widget](https://github.com/caksoylar/zmk-rgbled-widget?tab=readme-ov-file#configuration-details)のMapping for color valuesを参考にしてください。

`config/boards/shields/nape/nape.conf`
```ini
# CONFIG_RGBLED_WIDGET_SHOW_LAYER_CHANGE=y
CONFIG_RGBLED_WIDGET_SHOW_LAYER_COLORS=y
CONFIG_RGBLED_WIDGET_LAYER_0_COLOR=0
CONFIG_RGBLED_WIDGET_LAYER_1_COLOR=0
CONFIG_RGBLED_WIDGET_LAYER_2_COLOR=0
CONFIG_RGBLED_WIDGET_LAYER_3_COLOR=0
CONFIG_RGBLED_WIDGET_LAYER_4_COLOR=0
CONFIG_RGBLED_WIDGET_LAYER_5_COLOR=0
CONFIG_RGBLED_WIDGET_LAYER_6_COLOR=0
CONFIG_RGBLED_WIDGET_LAYER_7_COLOR=0
 # bt sel: white
CONFIG_RGBLED_WIDGET_LAYER_8_COLOR=7
# bt clr: red
CONFIG_RGBLED_WIDGET_LAYER_9_COLOR=1
# scroll: yellow
CONFIG_RGBLED_WIDGET_LAYER_10_COLOR=3
# trabo: blue
CONFIG_RGBLED_WIDGET_LAYER_11_COLOR=4
# snipe: green
CONFIG_RGBLED_WIDGET_LAYER_12_COLOR=2
# tab: magenta
CONFIG_RGBLED_WIDGET_LAYER_13_COLOR=5
```

---

以下、オリジナルのReadMe。

This work is based on [ufan's zmk pixart sensor drivers](https://github.com/ufan/zmk/tree/support-trackpad), [inorichi's zmk-pmw3610-driver](https://github.com/inorichi/zmk-pmw3610-driver), and [Zephyr PMW3610 driver](https://github.com/zephyrproject-rtos/zephyr/blob/main/drivers/input/input_pmw3610.c).

This driver had been tested on [my PMW3610 breakout board](https://github.com/badjeff/pmw3610-pcb).

> [!IMPORTANT]
> 🚨 Breaking Change for `main`/`zmk-0.4` branch of this module:
>
> To avoid conflicts with the upstream Zephyr driver module, changes need to be made to the existing configuration in order to use this module in the current version of ZMK.
> - Compatible string changed to `pixart,pmw3610-alt`
> - All config prefix changed to `CONFIG_PMW3610_ALT_*`

#### What is different to [inorichi's driver](https://github.com/inorichi/zmk-pmw3610-driver)
- Compatible to be used on split peripheral shield.
- Replaced `CONFIG_PMW3610_ORIENTATION_*` with ~~`CONFIG_PMW3610_SWAP_XY` and `PMW3610_INVERT_*`~~ device tree node attributes `swap-xy;`, `invert-x;` and `invert-y;`. Then now, it can used on [leylabella](https://github.com/badjeff/leylabella), which has different sensor breakout pcb orientation on one device.
- Moved `CONFIG_PMW3610_CPI` to device tree node `.dts/.overlay`. It is now allowed to setup diffeent config for multi-sensor on single shield. In case of building typical mouse shield, we use one movment sensor on bottom, and another sensor for scrolling on top. Those settings could be distinguishable.
- Features for scroll-mode, snipe-mode, and auto-layer are no longer needed to be provided from sensor driver. Those settings is now configurable in keymap with layer-based `zmk,input-listener`, instead of setup static value in shield config files.
- Seperating sampling rate and reporting rate. It reports accumulated XY axes displacement between data ready interrupts. You will still feeling lag and jumpy in noisy radio hell, but the cursor traction should being lossless, and predicable in exact terms.
- Default to use power saving config. Applying shorter-than-default downshift time to PMW3610.
- Deprecated manual *chip-select*. Refactored to use Zephyr's `spi_transceive_dt()`. That allow the sensor could be attacted to a shared SPI bus, works along with others SPI peripherals, such as display module.

## Installation

Include this project on ZMK's west manifest in `config/west.yml`:

```yml
manifest:
  remotes:
    ...
    # START #####
    - name: badjeff
      url-base: https://github.com/badjeff
    # END #######
    ...
  projects:
    ...
    # START #####
    - name: zmk-pmw3610-driver
      remote: badjeff
      revision: main
    # END #######
    ...
  self:
    path: config
```

Update `board.overlay` adding the necessary bits (update the pins for your board accordingly):

```dts
&pinctrl {
    spi0_default: spi0_default {
        group1 {
            psels = <NRF_PSEL(SPIM_SCK, 0, 8)>,
                <NRF_PSEL(SPIM_MOSI, 0, 17)>,
                <NRF_PSEL(SPIM_MISO, 0, 17)>;
        };
    };

    spi0_sleep: spi0_sleep {
        group1 {
            psels = <NRF_PSEL(SPIM_SCK, 0, 8)>,
                <NRF_PSEL(SPIM_MOSI, 0, 17)>,
                <NRF_PSEL(SPIM_MISO, 0, 17)>;
            low-power-enable;
        };
    };
};

#include <zephyr/dt-bindings/input/input-event-codes.h>

&spi0 {
    status = "okay";
    compatible = "nordic,nrf-spim";
    pinctrl-0 = <&spi0_default>;
    pinctrl-1 = <&spi0_sleep>;
    pinctrl-names = "default", "sleep";
    cs-gpios = <&gpio0 20 GPIO_ACTIVE_LOW>;

    trackball: trackball@0 {
        status = "okay";
        compatible = "pixart,pmw3610-alt";
        reg = <0>;
        spi-max-frequency = <2000000>;
        irq-gpios = <&gpio0 6 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;
        cpi = <600>;
        // swap-xy; /* optional */
        // invert-x; /* optional */
        // invert-y; /* optional */
        evt-type = <INPUT_EV_REL>;
        x-input-code = <INPUT_REL_X>;
        y-input-code = <INPUT_REL_Y>;

        force-awake;
        /* keep the sensor awake while ZMK activity state is ACTIVE,
           fallback to normal downshift mode after ZMK goes into IDLE / SLEEP mode.
           thus, the sensor would be a `wakeup-source` */

        force-awake-4ms-mode;
        /* while force-awake is acitvated, enable this mode to force sampling per 
           4ms, where the default sampling rate is 8ms. */
        /* NOTE: apply this mode if you need 250Hz with direct USB connection. */
    };
};

/ {
  trackball_listener {
    compatible = "zmk,input-listener";
    device = <&trackball>;
  };
};
```

Enable the driver config in `<shield>.config` file (read the Kconfig file to find out all possible options):

```conf
CONFIG_SPI=y
CONFIG_INPUT=y
CONFIG_ZMK_POINTING=y
CONFIG_PMW3610_ALT=y
# CONFIG_PMW3610_ALT_SWAP_XY=y // <-- deprecated, use swap-xy; instead
# CONFIG_PMW3610_ALT_INVERT_X=y // <-- deprecated, use invert-x; instead
# CONFIG_PMW3610_ALT_INVERT_Y=y // <-- deprecated, use invert-y; instead
# CONFIG_PMW3610_ALT_REPORT_INTERVAL_MIN=12
# CONFIG_PMW3610_ALT_LOG_LEVEL_DBG=y
# CONFIG_PMW3610_ALT_INIT_POWER_UP_EXTRA_DELAY_MS=300 // <--see Troubleshooting
```

## Troubleshooting

If you are getting `Incorrect product id 0xFF (expecting 0x3E)!` on `nice_nano_v2` board from the log, you'd want to apply `CONFIG_PMW3610_ALT_INIT_POWER_UP_EXTRA_DELAY_MS=1000` in your shield .conf/.overlay file. Due to this driver doesn't offer module dependancy setting, that would ensure external power (to enable VCC pin on board) is ready, the `CONFIG_PMW3610_ALT_INIT_POWER_UP_EXTRA_DELAY_MS` would use to add extra one second delay of power up.
