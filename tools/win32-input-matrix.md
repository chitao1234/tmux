# Windows console input matrix

Generated 2026-11-15T12:39:49.7484921+08:00 on DESKTOP-CIFL78F, Windows 25H2.

Terminals: conhost=yes, conpty=yes, wt=yes, wezterm=yes

## conhost: injected records -> ReadFile bytes

Each cell is what ReadFile on CONIN$ returned for one key,
in a fresh console set to the given input mode.

| key | `none` | `0x0` | `0x200` | `0x1f7` | `0x3f0` |
|---|---|---|---|---|---|
| `a` | (none) | 61 | 61 | (none) | 61 |
| `ctrl-c` | (none) | 03 | 03 | (none) | 03 |
| `ctrl-h` | (none) | 08 | 08 | (none) | 08 |
| `ctrl-i` | (none) | 09 | 09 | (none) | 09 |
| `ctrl-j` | (none) | (none) | 0a | (none) | 0a |
| `ctrl-m` | 0d 0a | 0d | 0d | 0d 0a | 0d |
| `ctrl-[` | (none) | 1b | 1b | (none) | 1b |
| `enter` | 0d 0a | 0d | 0d | 0d 0a | 0d |
| `tab` | (none) | 09 | 09 | (none) | 09 |
| `esc` | (none) | (none) | 1b | (none) | 1b |
| `backspace` | (none) | 08 | 7f | (none) | 7f |
| `shift-tab` | (none) | 09 | 1b 5b 5a | (none) | 1b 5b 5a |
| `up` | (none) | (none) | 1b 5b 41 | (none) | 1b 5b 41 |
| `alt-x` | (none) | 78 | 1b 78 | (none) | 1b 78 |
| `f1` | (none) | (none) | 1b 4f 50 | (none) | 1b 4f 50 |
| `pageup` | (none) | (none) | 1b 5b 35 7e | (none) | 1b 5b 35 7e |

## ConPTY: device bytes -> child ReadFile

This is the tmux pane boundary: bytes written at the device side,
read back by a child that set the given console input mode.

### mode `none`

| key | sent | child read | match |
|---|---|---|---|
| `a` | `61` | `` | **lost** |
| `ctrl-c` | `03` | `` | **lost** |
| `ctrl-h` | `08` | `` | **lost** |
| `ctrl-i` | `09` | `` | **lost** |
| `ctrl-j` | `0a` | `` | **lost** |
| `ctrl-m` | `0d` | `0d 0a` | **differs** |
| `ctrl-[` | `1b` | `` | **lost** |
| `enter` | `0d` | `0d 0a` | **differs** |
| `tab` | `09` | `` | **lost** |
| `esc` | `1b` | `` | **lost** |
| `backspace` | `7f` | `` | **lost** |
| `shift-tab` | `1b 5b 5a` | `` | **lost** |
| `up` | `1b 5b 41` | `` | **lost** |
| `alt-x` | `1b 78` | `` | **lost** |
| `f1` | `1b 4f 50` | `` | **lost** |
| `pageup` | `1b 5b 35 7e` | `` | **lost** |

### mode `0x0`

| key | sent | child read | match |
|---|---|---|---|
| `a` | `61` | `61` | yes |
| `ctrl-c` | `03` | `03` | yes |
| `ctrl-h` | `08` | `7f` | **differs** |
| `ctrl-i` | `09` | `09` | yes |
| `ctrl-j` | `0a` | `` | **lost** |
| `ctrl-m` | `0d` | `0d` | yes |
| `ctrl-[` | `1b` | `` | **lost** |
| `enter` | `0d` | `0d` | yes |
| `tab` | `09` | `09` | yes |
| `esc` | `1b` | `` | **lost** |
| `backspace` | `7f` | `08` | **differs** |
| `shift-tab` | `1b 5b 5a` | `5b | 5a` | **differs** |
| `up` | `1b 5b 41` | `5b | 41` | **differs** |
| `alt-x` | `1b 78` | `78` | **differs** |
| `f1` | `1b 4f 50` | `4f | 50` | **differs** |
| `pageup` | `1b 5b 35 7e` | `5b | 35 | 7e` | **differs** |

### mode `0x200`

| key | sent | child read | match |
|---|---|---|---|
| `a` | `61` | `61` | yes |
| `ctrl-c` | `03` | `03` | yes |
| `ctrl-h` | `08` | `08` | yes |
| `ctrl-i` | `09` | `09` | yes |
| `ctrl-j` | `0a` | `0a` | yes |
| `ctrl-m` | `0d` | `0d` | yes |
| `ctrl-[` | `1b` | `1b` | yes |
| `enter` | `0d` | `0d` | yes |
| `tab` | `09` | `09` | yes |
| `esc` | `1b` | `1b` | yes |
| `backspace` | `7f` | `7f` | yes |
| `shift-tab` | `1b 5b 5a` | `1b | 5b | 5a` | yes |
| `up` | `1b 5b 41` | `1b | 5b | 41` | yes |
| `alt-x` | `1b 78` | `1b | 78` | yes |
| `f1` | `1b 4f 50` | `1b | 4f | 50` | yes |
| `pageup` | `1b 5b 35 7e` | `1b | 5b | 35 | 7e` | yes |

### mode `0x1f7`

| key | sent | child read | match |
|---|---|---|---|
| `a` | `61` | `` | **lost** |
| `ctrl-c` | `03` | `` | **lost** |
| `ctrl-h` | `08` | `` | **lost** |
| `ctrl-i` | `09` | `` | **lost** |
| `ctrl-j` | `0a` | `` | **lost** |
| `ctrl-m` | `0d` | `0d 0a` | **differs** |
| `ctrl-[` | `1b` | `` | **lost** |
| `enter` | `0d` | `0d 0a` | **differs** |
| `tab` | `09` | `` | **lost** |
| `esc` | `1b` | `` | **lost** |
| `backspace` | `7f` | `` | **lost** |
| `shift-tab` | `1b 5b 5a` | `` | **lost** |
| `up` | `1b 5b 41` | `` | **lost** |
| `alt-x` | `1b 78` | `` | **lost** |
| `f1` | `1b 4f 50` | `` | **lost** |
| `pageup` | `1b 5b 35 7e` | `` | **lost** |

### mode `0x3f0`

| key | sent | child read | match |
|---|---|---|---|
| `a` | `61` | `61` | yes |
| `ctrl-c` | `03` | `03` | yes |
| `ctrl-h` | `08` | `08` | yes |
| `ctrl-i` | `09` | `09` | yes |
| `ctrl-j` | `0a` | `0a` | yes |
| `ctrl-m` | `0d` | `0d` | yes |
| `ctrl-[` | `1b` | `1b` | yes |
| `enter` | `0d` | `0d` | yes |
| `tab` | `09` | `09` | yes |
| `esc` | `1b` | `1b` | yes |
| `backspace` | `7f` | `7f` | yes |
| `shift-tab` | `1b 5b 5a` | `1b | 5b | 5a` | yes |
| `up` | `1b 5b 41` | `1b | 5b | 41` | yes |
| `alt-x` | `1b 78` | `1b | 78` | yes |
| `f1` | `1b 4f 50` | `1b | 4f | 50` | yes |
| `pageup` | `1b 5b 35 7e` | `1b | 5b | 35 | 7e` | yes |

## wt: records -> bytes inside the real terminal

| key | `none` | `0x0` | `0x200` | `0x1f7` | `0x3f0` |
|---|---|---|---|---|---|
| `a` | (none) | 61 | 61 | (none) | 61 |
| `ctrl-c` | (none) | 03 | 03 | (none) | 03 |
| `ctrl-h` | (none) | 08 | 08 | (none) | 08 |
| `ctrl-i` | (none) | 09 | 09 | (none) | 09 |
| `ctrl-j` | (none) | (none) | 0a | (none) | 0a |
| `ctrl-m` | 0d 0a | 0d | 0d | 0d 0a | 0d |
| `ctrl-[` | (none) | 1b | 1b | (none) | 1b |
| `enter` | 0d 0a | 0d | 0d | 0d 0a | 0d |
| `tab` | (none) | 09 | 09 | (none) | 09 |
| `esc` | (none) | (none) | 1b | (none) | 1b |
| `backspace` | (none) | 08 | 7f | (none) | 7f |
| `shift-tab` | (none) | 09 | 1b 5b 5a | (none) | 1b 5b 5a |
| `up` | (none) | (none) | 1b 5b 41 | (none) | 1b 5b 41 |
| `alt-x` | (none) | 78 | 1b 78 | (none) | 1b 78 |
| `f1` | (none) | (none) | 1b 4f 50 | (none) | 1b 4f 50 |
| `pageup` | (none) | (none) | 1b 5b 35 7e | (none) | 1b 5b 35 7e |

## wezterm: records -> bytes inside the real terminal

| key | `none` | `0x0` | `0x200` | `0x1f7` | `0x3f0` |
|---|---|---|---|---|---|
| `a` | (none) | 61 | 61 | (none) | 61 |
| `ctrl-c` | (none) | 03 | 03 | (none) | 03 |
| `ctrl-h` | (none) | 08 | 08 | (none) | 08 |
| `ctrl-i` | (none) | 09 | 09 | (none) | 09 |
| `ctrl-j` | (none) | (none) | 0a | (none) | 0a |
| `ctrl-m` | 0d 0a | 0d | 0d | 0d 0a | 0d |
| `ctrl-[` | (none) | 1b | 1b | (none) | 1b |
| `enter` | 0d 0a | 0d | 0d | 0d 0a | 0d |
| `tab` | (none) | 09 | 09 | (none) | 09 |
| `esc` | (none) | (none) | 1b | (none) | 1b |
| `backspace` | (none) | 08 | 7f | (none) | 7f |
| `shift-tab` | (none) | 09 | 1b 5b 5a | (none) | 1b 5b 5a |
| `up` | (none) | (none) | 1b 5b 41 | (none) | 1b 5b 41 |
| `alt-x` | (none) | 78 | 1b 78 | (none) | 1b 78 |
| `f1` | (none) | (none) | 1b 4f 50 | (none) | 1b 4f 50 |
| `pageup` | (none) | (none) | 1b 5b 35 7e | (none) | 1b 5b 35 7e |


