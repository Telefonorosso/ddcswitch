# DDCSwitch for Emu68

Small AmigaOS CLI utility for switching the HDMI monitor input directly through **DDC/CI** from an Emu68/PiStorm system.

No Linux service, network connection, PC-side listener or modified Emu68 kernel is required.

The utility accesses the Raspberry Pi HDMI DDC controller directly from AmigaOS and controls **VCP 0x60 — Input Source**.

## Status

**Hardware validated: Raspberry Pi 3A+ / Emu68**

Tested successfully for:

- setting monitor input
- reading the current input
- listing advertised input sources
- toggling between two selected inputs

## Build

Using Bebbo's `m68k-amigaos-gcc`:

```sh
m68k-amigaos-gcc -Os -m68020 -s -o DDCSwitch ddcswitch.c -mcrt=nix20
```

Copy `DDCSwitch` anywhere in your AmigaDOS path, for example:

```text
C:DDCSwitch
```

## Usage

### Set an input directly

Decimal:

```text
DDCSwitch 16
DDCSwitch 17
```

Hexadecimal:

```text
DDCSwitch 0x10
DDCSwitch 0x11
```

The values are the monitor-specific DDC/CI values for VCP `0x60`.

### Show the current input

```text
DDCSwitch CURRENT
```

or:

```text
DDCSwitch GET
```

Example:

```text
Current input: 0x10 (16) DisplayPort-2
```

### Switch between two inputs

```text
DDCSwitch SWITCH 16 17
```

If the current input is `16`, the monitor is switched to `17`.

If the current input is `17`, the monitor is switched to `16`.

This performs a single VCP `0x60` read followed by a write, so it is considerably faster than retrieving the complete monitor capability list.

This is particularly useful for assigning the command to a Workbench action, Commodity, hotkey or other Amiga-side trigger.

### List monitor-advertised inputs

```text
DDCSwitch LIST
```

Example:

```text
Input sources advertised by monitor:

  0x10   16  DisplayPort-2
  0x11   17  HDMI-1
```

`LIST` reads the monitor DDC/CI capabilities string and extracts the values advertised for VCP `0x60`.

Some monitors have incomplete or inaccurate capability strings, so `LIST` should be considered informational. A value may still work even if it is not listed.

## Monitor requirements

The monitor must support:

- DDC/CI
- VCP code `0x60` — Input Source
- DDC/CI commands through the Raspberry Pi HDMI connection

DDC/CI may need to be enabled in the monitor's OSD menu.

Input values are monitor-dependent.

Common values include:

```text
0x0F   DisplayPort-1
0x10   DisplayPort-2
0x11   HDMI-1
0x12   HDMI-2
```

Do not assume these values are correct for every monitor. Use:

```text
DDCSwitch LIST
```

and:

```text
DDCSwitch CURRENT
```

to inspect your monitor.

## Raspberry Pi compatibility

Current implementation directly accesses the **BSC2 HDMI DDC controller** used by the BCM283x family.

### Hardware validated

| Raspberry Pi | Status |
|---|---|
| Raspberry Pi 3A+ | **Hardware validated** |

### Expected compatibility

These systems use the same general BCM283x peripheral/DDC architecture and are expected to be compatible, but have not yet been hardware tested with DDCSwitch:

| Raspberry Pi | Expected status |
|---|---|
| Raspberry Pi Zero | Presumed compatible |
| Raspberry Pi Zero W | Presumed compatible |
| Raspberry Pi Zero 2 W | Presumed compatible |
| Raspberry Pi 2 | Presumed compatible |
| Raspberry Pi 3B | Presumed compatible |
| Raspberry Pi 3B+ | Presumed compatible |

### Not currently supported

| Raspberry Pi | Status |
|---|---|
| Raspberry Pi 4 / CM4 | Not supported by current BSC2 implementation |
| Raspberry Pi 5 | Not supported |

Pi 4 and later systems use a different HDMI/DDC architecture and require a separate backend.

## Emu68 compatibility

DDCSwitch does **not** require a custom Emu68 build.

It has been tested with stock Emu68 using the BCM283x MMIO mapping exposed to the Amiga side.

The current implementation uses:

```text
BSC2 / HDMI DDC
Emu68 address: 0xF2805000
DDC/CI slave:  0x37
VCP feature:   0x60
```

## Example

Typical setup:

```text
PC        -> DisplayPort
PiStorm   -> HDMI
```

If the monitor reports:

```text
DisplayPort = 16
HDMI        = 17
```

then:

```text
DDCSwitch SWITCH 16 17
```

toggles the monitor between the PC and the Amiga/PiStorm.

Everything happens directly between **AmigaOS, Emu68, the Raspberry Pi HDMI controller and the monitor**.
