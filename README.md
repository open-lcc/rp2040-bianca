# OpenLCC RP2040 Firmware for Lelit Bianca

This is an evolution of [magnusnordlander/smart-lcc](https://github.com/magnusnordlander/smart-lcc), which in turn is based on the protocol dissection I did in [magnusnordlander/lelit-bianca-protocol](https://github.com/magnusnordlander/lelit-bianca-protocol).

## Compatibility

This firmware is compatible with Open LCC Board R1A through R2B. Furthermore, it's *known* to be compatible with the Bianca V2, but it's strongly suspected that it is compatible with Bianca V1, and Bianca V3 (with exception for the new Power LED). If you have a Bianca V3 and is interested in installing this project, let me (@magnusnordlander) know and we can work together on getting it fully compatible.

## Disclaimer

Considering this plugs in to an expensive machine it bears to mention: Anything you do with this, you do at your own risk. Components have been fried already during the course of this project. Your machine uses both line voltage power, high pressured hot water, steam and other dangerous components. There is a risk of both damaging the machine, personal injury and property damage, the liability for which you assume yourself. This is not the stage to get on board with this project if you aren't willing to deal with those risks.

## Status

Consider this project beta quality.

### A note on Bianca versions

There are (at the time of writing) three versions of the Bianca, V1, V2, and V3. To my understanding it should work on a Bianca V1, but it's untested. As for the Bianca V3, it features upgraded hardware. Lelit sells an upgrade kit to upgrade a V1 or V2. The differences are as follows:

* A new solenoid to control full/low flow from the pump (part of the upgrade kit)
  * Has the same P/N as the V2 one, but at least in my machine the actual parts were different.
* An LCC with new firmware (part of the upgrade kit)
  * Available as either P/N 9600147, or 9600124. V2 one was 9600045.
* The power light now an LED and software controlled (not part of the upgrade kit)
  * Available as P/N 3000056, but it's ridiculously expensive

It also uses a different Gicar Control Box, but since the V3 upgrade kit doesn't include it, my suspicion is that the changes in it are marginal (it could be as simple as the box having a different sticker). The new part number is 9600125, and the old one was 9600046. One known change is the new Standby mode, which should be disabled when using Open LCC. It is unknown if this is the only change. I would love photos of the V3 Control Board internals, just to confirm that there are no relevant hardware differences.

#### V3 Bianca suppport

My Bianca is a V2, but it's been upgraded with a V3 solenoid valve. I've implemented support for the Low Flow valve, and will implement support for the power LED. To my knowledge, this has not been tested in a real V3 though. 

As for the new features of the V3:

* The new V3 standby mode might never be supported, but it's kind of a useless mode. Just disable it.
* You can use routines to do a low flow start and low flow finish.
* Brew temperature offset – I might implement this, but it's somewhat emulatable with routines
   * I'm not sure how much of a difference this makes to water temperature in the cup, the E61 group holds a *lot* of heat
   * Not to be confused with the brew temperature offset setting that already exists though. That one is just an offset from the true boiler temperature to the temperature shown on the display (default in the stock LCC is -10 degrees celsius, but I use -16.5, as that is a truer representation of the temperature at the group

### Versioning
This project uses Semver. The major version number is increased whe RP2040 <-> ESP32 protocol version is increased (as that is a BC break).

## Project goals

Create a firmware for using the Open LCC in a Lelit Bianca to its fullest extent.

## Architecture

#### RP2040 Core 0
* System controller
    * Safety critical, uses the entire core for itself
    * Communicates with the Control Board
    * Performs a safety check, ensuring that temperatures in the boilers never exceed safe limits, and that both boilers are never running simultaneously.
    * Responsible for PID, keeping water in the boiler, running pumps etc.

#### RP2040 Core 1
* Communication with the ESP32-S3
* Reading from external sensors (via I2C etc)
* Handling Automations

### Extension boards
The Open LCC hardware has QWIIC interfaces to allow for extension. Currently the RP2040 firmware supports additional
MCP9600 thermocouple readers. 

## Building

The project is built using CMake. There are two relevant targets. `smart_lcc` and `smart_lcc_combined`. You need to first 
build the `smart_lcc` target, and *then* build the `smart_lcc_combined` target. I'm sure it would be possible to roll both
of these targets into one, but I haven't put enough effort into it yet. The reason for the two targets is the Serial
Bootloader.

### Defines

There are a number of define flags to be aware of. Firstly, there are `HARDWARE_REVISION_*` flags to set which revision
of the Open LCC Main Board you are using. Current options are `HARDWARE_REVISION_OPENLCC_R1A`, `HARDWARE_REVISION_OPENLCC_R2A`
and `HARDWARE_REVISION_OPENLCC_R2B`. You need to set one (and only one) of these

Secondly, there's `USB_DEBUG`. It enables debug output via USB-CDC, and should not be used inside an actual machine.
Outside of an actual machine (e.g. using a control board emulator), it can be useful, but it delays startup by 5 seconds
and if both cores try to print debug output at the same time, the RP2040 crashes, so it's very much just for debugging.

### Rebooting into BOOTSEL or Serial Boot

The RP2040 on the Open LCC board is controlled by the ESP32-S3; both the RESETn pin and the CSn for the flash. While the
debug board (especially the R2A debug board) has facilities to control RESETn and CSn, the recommended way to bootstrap
the project is to install the ESP32-S3 firmware first, and use that to reboot the RP2040 into BOOTSEL. Once you have
flashed the firmware (through the USB ROM bootloader), you can henceforth use Serial Boot to update it. That being said,
if you have the main board outside of your machine and the debug board connected, the USB ROM Bootloader is faster to use.

### A note on Serial Boot
This project includes a [serial third stage bootloader](https://github.com/usedbytes/rp2040-serial-bootloader). This is 
to be able to update the firmware of the RP2040 over Wi-fi via the ESP32-S3. You can still update firmware via USB, and 
in that case you should use the `smart_lcc_combined.uf2` file.

To update the firmware via Wi-fi, use [serial-flash](https://github.com/usedbytes/serial-flash) the following command:

```sh
serial-flash tcp:192.168.1.10:6638 smart_lcc_app.bin 0x10008000
```

Obviously, replace the IP address and port to match the IP address of the ESP32-S3, and the port of the serial bridge
you're using for the ESP32-S3.

## Licensing

The firmware is MIT licensed (excepting dependencies, which have their own, compatible licenses).
