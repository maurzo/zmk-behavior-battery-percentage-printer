# ZMK Battery Percentage Printer Behavior

This is a modified version of [alan0ford's behavior_battery_printer.c](https://github.com/alan0ford/zmk-lplancks/blob/GHPilotBatt/boards/shields/lplancks/behavior_battery_printer.c). Changes has been added to make the behavior awaring of peripheral id.

## What it does

Type battery percentages by triggering behaviors on the shield's keymap.

`&bapp` prints the battery percentage for the source that triggered the behavior.

`&bapp_all` prints labeled battery percentages for the central and all peripherals whose battery level has been seen by the central, for example `C 91% P0 88% P1 84% `.

`&bapp_periph` prints labeled battery percentages for peripherals only, for example `P0 88% P1 84% `.

## Installation

Include this project on your ZMK's west manifest in `config/west.yml`:

```diff
  [...]
  remotes:
+    - name: maurzo
+      url-base: https://github.com/maurzo
  projects:
+    - name: zmk-behavior-battery-percentage-printer
+      remote: maurzo
+      revision: main
  [...]
```

And update `shield.conf` on both *central* and *peripheral* shields.
```
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y
```

Now, update your `shield.keymap` adding the behaviors.

```c
/{
#include <behaviors/battery_percentage_printer.dtsi>

        keymap {
                compatible = "zmk,keymap";
                base {
                        bindings = <
                              ...
                              ...   &bapp   ...   &bapp_all ...   &bapp_periph
                                  /* source */   /* all parts */   /* peripherals */
                              ...
                        >;
                };
       };

};
```
