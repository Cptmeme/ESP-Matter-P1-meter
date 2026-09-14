# Matter P1 Smart-Meter Monitor

Firmware that turns the **Leotro Universal P1 Port Dongle** (ESP32-S3) into a
Matter **Electrical Sensor**. It reads the DSMR/P1 telegram from a smart meter
and publishes live power and cumulative energy over Matter (Wi-Fi or Thread).

Hardware: <https://github.com/Leotro-Engineering/Universal-P1-Port-Dongle>

## What it exposes

A single aggregate **Electrical Sensor** endpoint with:

| Matter cluster                     | Attribute / event            | Source (OBIS)                     |
|------------------------------------|------------------------------|-----------------------------------|
| Electrical Power Measurement       | `ActivePower` (net, mW)      | `1-0:1.7.0` − `1-0:2.7.0`         |
| Electrical Power Measurement       | `Voltage` (mV)               | `1-0:32.7.0` (L1)                 |
| Electrical Power Measurement       | `ActiveCurrent` (mA)         | `1-0:31.7.0` (L1)                 |
| Electrical Energy Measurement      | `CumulativeEnergyImported`   | `1-0:1.8.1` + `1-0:1.8.2`         |
| Electrical Energy Measurement      | `CumulativeEnergyExported`   | `1-0:2.8.1` + `1-0:2.8.2`         |

`ActivePower` is the net of import and export, so a house exporting solar power
reports a negative value. Both single-phase and three-phase DSMR 4/5 meters are
auto-detected; the aggregate endpoint uses the total power and the L1
voltage/current as representative values.

> Note: the Electrical Power/Energy Measurement clusters are Matter 1.3+.
> Home Assistant reads them well; Apple/Google Home support is currently limited.

## Pin configuration

Defaults match the Universal P1 Dongle and are configurable under
`idf.py menuconfig` → *P1 Dongle Configuration*:

| Function                          | GPIO   | Kconfig             |
|-----------------------------------|--------|---------------------|
| P1 data in, UART RX (from meter)  | 1      | `P1_UART_RX_GPIO`   |
| Data-request line (to meter)      | 2      | `P1_REQUEST_GPIO`   |
| P1 data out, UART TX (passthrough)| 42     | `P1_TX_GPIO`        |
| Downstream request in (TXREQ)     | 41     | `P1_TXREQ_GPIO`     |
| UART peripheral                   | 1      | `P1_UART_NUM`       |
| Baud rate                         | 115200 | `P1_UART_BAUD_RATE` |

The P1 data line is inverted/open-collector at the meter; the dongle's
transistor restores normal UART polarity, so it is read without software
inversion. The request line is held high so a DSMR 4/5 meter streams a telegram
every second. For older DSMR 2.2/3 meters set the baud rate to 9600 and disable
`P1_VERIFY_CRC`.

## P1 passthrough (second port)

Enabled by default (`P1_PASSTHROUGH`). Every telegram received from the meter is
relayed out the dongle's second P1 port (P1TX, `TX` on GPIO42), so a downstream
device — e.g. a **HomeWizard P1 meter** — plugged into that port reads the same
data the meter sent. The dongle also supplies 5 V on that port to power the
downstream device. Matter monitoring and passthrough both run off the same meter
read, so you get a Matter sensor *and* a working pass-through port at once.

By default telegrams stream continuously, which works with HomeWizard and most
readers. To forward only while the downstream device asserts its request line,
enable `P1_PASSTHROUGH_HONOR_REQUEST` (senses `TXREQ` on GPIO41). Disable
`P1_PASSTHROUGH` entirely if you don't use the second port.

Note: you connect *one meter* to the input port (P1RX) and a downstream
*consumer device* to the output port (P1TX) — not a second meter.

## Build & flash

```
idf.py set-target esp32s3
idf.py menuconfig          # optional: adjust pins / baud
idf.py build flash monitor
```

Parsed values are logged over the console (`p1_reader` tag) so you can confirm
the meter is being read before/independent of Matter commissioning. Long-press
the BOOT button to factory-reset (decommission) the device.

## Source layout

- `main/dsmr_parser.{h,cpp}` — OBIS/CRC16 telegram parser (no hardware deps).
- `main/p1_reader.{h,cpp}` — UART + data-request line + reader task.
- `main/electrical_measurement.{h,cpp}` — Matter EPM/EEM clusters and the
  mapping from a parsed reading to attributes/events.
- `main/app_main.cpp` — node + Electrical Sensor endpoint, wiring it together.
