# zmk-getreuer

ZMK ports of three of [Pascal Getreuer's QMK community modules](https://getreuer.info/posts/keyboards/):

| Behavior | Original | What it does |
| --- | --- | --- |
| `&select_word` | [Select Word](https://getreuer.info/posts/keyboards/select-word/index.html) | One key selects the current word; press again / hold to extend. Line selection too. |
| `&cyclotab` | [Cyclotab](https://getreuer.info/posts/keyboards/cyclotab/index.html) | Alt+Tab (or ⌘+Tab, …) without holding the modifier. |
| `&om` | [Orbital Mouse](https://getreuer.info/posts/keyboards/orbital-mouse/index.html) | Tank-control mouse keys: forward/back + steer, with orbiting for fine aim. |

The C code follows the original modules closely (same key sequences, same
fixed-point math and default tuning), adapted to ZMK's behavior/event model.
Licensed Apache-2.0, like the originals.

## Install

This is a standard ZMK module (a folder with `zephyr/module.yml`). It needs a
`west`-based build, i.e. a GitHub repo that compiles the firmware. Web editors
that only inject devicetree text (MoErgo Layout Editor's "Custom Defined
Behaviors", ZMK Studio) cannot add C code, so see the Glove80 section below if
that is what you use.

**1. `config/west.yml`** — add the module as a project. Upstream ZMK:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: YOUR_GITHUB_USER            # wherever you host this module
      url-base: https://github.com/YOUR_GITHUB_USER
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-getreuer
      remote: YOUR_GITHUB_USER
      revision: main
  self:
    path: config
```

**2. `config/<keyboard>.conf`** — only needed for Orbital Mouse:

```ini
CONFIG_ZMK_POINTING=y
```

**3. `config/<keyboard>.keymap`** — include what you use:

```c
#include <behaviors.dtsi>
#include <dt-bindings/zmk/keys.h>

#include <behaviors/select_word.dtsi>    // &select_word, &select_word_mac
#include <behaviors/cyclotab.dtsi>       // &cyclotab
#include <behaviors/orbital_mouse.dtsi>  // &om  (+ its input listener)
```

The behaviors are `/omit-if-no-ref/`, so unused ones cost nothing.

## Glove80 / MoErgo Layout Editor

The Layout Editor builds on MoErgo's servers from a fixed ZMK tree, so it can
only accept devicetree snippets, not new behaviors written in C. The workflow
that does work:

1. Keep designing in the Layout Editor. For the keys that should use these
   behaviors, use a **Custom** key and type the binding text directly, e.g.
   `&select_word SW_WORD`, `&cyclotab LA(TAB)`, `&om OM_U`. (The editor will
   not know what they are and its own "Build firmware" will fail — that's
   expected; you won't use that button.)
2. Create a repo from MoErgo's west template
   <https://github.com/moergo-sc/glove80-zmk-config-west> and push this module
   to a repo of your own (e.g. `YOUR_GITHUB_USER/zmk-getreuer`).
3. In that repo, edit `config/west.yml` to add the module:

   ```yaml
   manifest:
     remotes:
       - name: moergo-sc
         url-base: https://github.com/moergo-sc
       - name: YOUR_GITHUB_USER
         url-base: https://github.com/YOUR_GITHUB_USER
     projects:
       - name: zmk
         remote: moergo-sc
         revision: main
         import: app/west.yml
       - name: zmk-getreuer
         remote: YOUR_GITHUB_USER
         revision: main
     self:
       path: config
   ```

4. In the Layout Editor open your layout's menu and download/export the
   `.keymap` file. Copy its contents into `config/glove80.keymap`, and add the
   three `#include <behaviors/...dtsi>` lines from step 3 of "Install" next to
   the other includes at the top. For Orbital Mouse also add
   `CONFIG_ZMK_POINTING=y` to `config/glove80.conf`.
5. Commit and push. GitHub Actions builds `firmware.zip` with
   `glove80_lh-zmk.uf2` / `glove80_rh-zmk.uf2`; flash each half with the
   matching file (unlike the editor's firmware, the halves differ).

Every time you change the layout in the editor, repeat step 4. The module is
written to compile against MoErgo's ZMK fork as well as upstream (the fork is
current enough to have the pointing subsystem the mouse needs).

## Select Word

```c
&select_word SW_WORD      // select word, repeat/hold to extend forward
&select_word SW_BACK      // select word to the left, repeat to extend back
&select_word SW_LINE      // select line, repeat/hold to extend down
&select_word SW_LINE_UP   // select line, repeat/hold to extend up
```

As in the QMK module, `SW_WORD` pressed while a Shift key is held does line
selection. Pressing `←`/`→` during a selection collapses it to that end.
Pressing any other (non-modifier) key forgets the selection state, and so does
5 s of idle time (`timeout-ms`, 0 disables).

`&select_word` sends Windows/Linux hotkeys (Ctrl+arrows, Home/End).
`&select_word_mac` sends macOS ones (Alt+arrows, Cmd+arrows). If you keep a
Mac base layer and a Windows base layer, just put a different one on each.
For a single key that switches with a toggled layer, wrap them in a mod-morph
or use conditional layers as you prefer. To make your own node:

```c
/ { behaviors {
    my_sw: my_sw { compatible = "zmk,behavior-select-word"; #binding-cells = <1>; mac; timeout-ms = <2000>; };
}; };
```

## Cyclotab

```c
&cyclotab LA(TAB)        // Alt+Tab, then keeps Alt held
&cyclotab LS(LA(TAB))    // Shift+Alt+Tab (reverse direction)
&cyclotab LG(TAB)        // Cmd+Tab on macOS
&cyclotab LG(GRAVE)      // Cmd+` (cycle windows of the app)
```

Tap once to open the switcher; the non-Shift modifiers stay held so you can
keep tapping. The modifiers are released when:

- you press any other key — that key press is swallowed (e.g. Space won't be
  typed into the terminal you just switched to). Set the `pass-through`
  property on the node if you'd rather the key went through;
- you press Escape — Escape is sent first, so the switcher cancels;
- the layer you pressed `&cyclotab` on is deactivated — put it on a momentary
  layer and releasing the layer key completes the switch instantly;
- 1 s passes with nothing pressed (`timeout-ms`, 0 disables).

Arrow keys, Shift (including mod-tap holds and sticky shift) and other modifier
keys pass through without ending the cycle. Holding Shift pauses the timeout.
The key of the hotkey is sent as a normal keycode event, so `&key_repeat`
works to send it again.

Different timeout per hotkey? Define two nodes:

```c
/ { behaviors {
    cyclotab_slow: cyclotab_slow { compatible = "zmk,behavior-cyclotab"; #binding-cells = <1>; timeout-ms = <2500>; };
    cyclotab_hold: cyclotab_hold { compatible = "zmk,behavior-cyclotab"; #binding-cells = <1>; timeout-ms = <0>; };
}; };
```

> Swallowing the terminating key relies on this module's event listener running
> before ZMK's HID listener, which is how modules link today (the same thing
> urob's leader-key and auto-layer modules rely on). If a future ZMK changes
> that, the key would simply be typed; nothing else breaks.

## Orbital Mouse

Needs `CONFIG_ZMK_POINTING=y`. Getreuer's suggested right-hand block:

```c
&om OM_W_U   &om OM_BTNS  &om OM_U     &om OM_DBLS  &om OM_FAST
&om OM_W_D   &om OM_L     &om OM_D     &om OM_R     &om OM_SLOW
&om OM_RELS  &om OM_HLDS  &om OM_SEL1  &om OM_SEL2  &om OM_SEL3
```

| Code | Action |
| --- | --- |
| `OM_U` / `OM_D` | Move forward / backward along the heading |
| `OM_L` / `OM_R` | Steer counter-clockwise / clockwise (cursor orbits a point one radius back) |
| `OM_CS_U/D/L/R` | Conventional up/down/left/right; combine for diagonals |
| `OM_W_U/D/L/R` | Mouse wheel |
| `OM_SLOW` / `OM_FAST` | Hold for slow ("sniping") / fast mode |
| `OM_BTN1`…`OM_BTN5` | Click mouse button n |
| `OM_SEL1`…`OM_SEL5` | Select a button for the four below (button 1 initially) |
| `OM_BTNS` / `OM_DBLS` | Click / double-click the selected button |
| `OM_HLDS` / `OM_RELS` | Hold / release the selected button (click-and-drag) |

ZMK's HID descriptor exposes 5 mouse buttons, so buttons 6–8 from the QMK
version are not available.

Tuning goes on the node (the dtsi's `&om` uses the QMK defaults). Since
devicetree has no floats, factors are percentages:

```c
&om {
    speed-curve = <24 24 24 32 58 66 66 66 66 66 66 66 66 66 66 66>; // 16 entries, 0-255
    radius = <36>;                 // px, 0-63
    slow-move-percent = <33>;      // ORBITAL_MOUSE_SLOW_MOVE_FACTOR 0.333
    slow-turn-percent = <50>;
    fast-move-percent = <300>;     // ORBITAL_MOUSE_FAST_MOVE_FACTOR 3.0
    fast-turn-percent = <200>;
    wheel-speed-percent = <20>;    // 0.2 steps / frame
    dbl-delay-ms = <50>;
    interval-ms = <16>;
};
```

Because movement goes through ZMK's input subsystem, input processors on
`om_input_listener` (e.g. `zip_xy_scaler`, per-layer overrides) work as they
would for `&mmv`.

## Layout of this module

```
zephyr/module.yml                       Zephyr module descriptor
Kconfig, CMakeLists.txt
dts/bindings/behaviors/*.yaml           devicetree bindings (properties above)
dts/behaviors/*.dtsi                    ready-made &select_word, &cyclotab, &om nodes
include/dt-bindings/zmk/*.h             SW_* and OM_* parameter constants
src/behavior_*.c, src/zmk_compat.h      the behaviors (+ upstream/MoErgo API shim)
```
