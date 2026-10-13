# The driver-version dialog (`DXVK_CONFIG`, v1.6.0, 2026-10-12)

**Result:** with `DXVK_CONFIG=dxgi.customVendorId=1002` in the container's environment, AoE IV no longer shows "We were
unable to determine your video card's installed driver version". The variable changes only the vendor ID that DXVK's
`dxgi.dll` reports; the game then reads a driver version and passes its check. FPS did not change (49.5 against 50.4
in one window each), and the game's GPU class stayed "Gen7 Only".

## The problem

At "Loading step: [Graphics driver check]" the game logs its adapter and driver, and on the Thor it wrote:

```
-------- Display Adapter Driver Information --------
Vendor: Unknown (0x5143)
Installed Driver Version: Unknown Vendor ID (0)
Required Driver Version: Unknown (0)
```

`0x5143` is Qualcomm's PCI vendor ID. The game's strings around this block name only two vendors with a driver query:
NVIDIA (shader-unit count and clock) and AMD, through AMD's AGS library (`AMD AGS Library Not Initialized`,
`AMD GPU Relative Performance Rating`, `Umkown AMD GPU Asic Family`). Any other vendor gives "Unknown Vendor ID" and
version 0, and that result shows the dialog. Wine's registry already lists a driver version for the adapter
(`DriverVersion` = `35.0.99.9999` in the display class key), but the vendor check comes first.

The code that decides it is not readable in the file: the driver query (`RVA 0x370c4f0`) and the warning helper
(`RVA 0x826180`) are encrypted by the game's protection. Only their caller in `AppInit` is plain code
(`RVA 0x8bf9a0` onward): a result of 0 calls the warning helper with the file name `supported_drivers_check.log`,
a 1 (the "[ 1 ] days" of the button) and a message ID. The rest was found by testing.

## The "Don't show this message" choice

The choice lives in `Documents\My Games\Age of Empires IV\supported_drivers_check.log`, an empty file. Tested on the
Thor with [`tools/probes/settime.c`](../../tools/probes/settime.c), which sets and reads the file's times:

| The file | Result at the next start |
|---|---|
| Written by the game 7 minutes before | no dialog |
| Dated two days back | dialog |
| Dated 2100 | dialog, the date counts as expired |

The game rewrites the file with the current time whenever it shows the dialog, before any button is pressed, and
both buttons leave it so. The dialog therefore appears at most once a day, whatever the user picks. The other
start-up warnings use files of the same kind (`physical_memory_check.log`, `virtual_memory_check.log`,
`free_ram_check.log`, `compatability_mode_check.log`, `multi_core_check.log`). A symbolic link to a file in `/proc`
did not help: its time does not stay current, and the game could not rewrite it.

## The fix

The game ships AMD's own `amd_ags_x64.dll`, and the container's Wine already sends that library's ADL calls to
Proton's built-in `atiadlxx` (`AppDefaults\RelicCardinal.exe\DllOverrides`). With the adapter reported as AMD, the
game's AGS reads Proton's driver version and the check passes:

```
Vendor: AMD (0x1002)
Installed Driver Version: 99.19.02-230831a-396538C-AMD-Software-Adrenalin-Edition (415313922)
Required Driver Version: 21.20.01.14-210617a1 (88162305)
GPU Performance Class: Gen7 Only
```

`DXVK_CONFIG` reaches DXVK's `dxgi.dll` directly: GameNative splits each environment entry at its first `=`, and for
the VKD3D wrapper it sets no `DXVK_CONFIG` of its own (it does so only for the DXVK wrappers). With the variable and
the check file dated two days back, the game started without the dialog and did not touch the file. The same worked
with a `dxvk.conf` in the game folder, which users cannot easily create in GameNative.

Not used: the GPU class, which `performance_class_override` could set, and an AMD device ID. The game's GPU class
stays "Gen7 Only" with the vendor alone; a known AMD device could move it to a heavier class.

## Measurements

Skirmish benchmark, match minute 1, one window each, display at 120 Hz, the same session (2026-10-12):

| Setup | FPS | GPU busy |
|---|---|---|
| `aoe4-perf7-32` | 50.4 | 69 % |
| `aoe4-perf7-32` + `DXVK_CONFIG=dxgi.customVendorId=1002` | 49.5 | 70 % |

The difference is within the spread of single windows. Both windows ran on GameNative's default driver, not on
`turnip-main-c78a9e9-ubwc`: deleting the old `turnip-main-c78a9e9` earlier that day had reset the container
(README, Known issues), hence about 50 FPS instead of 58 to 60 and the GPU busy 70 %. A pair on the release's driver
is still to be measured.

## Not tested yet

- A full match or the late game with the variable.
- Whether the game uses AMD-only paths beyond the driver check now that AGS initializes.
