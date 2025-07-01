# Fairphone 2

The Fairphone 2 is the smartphone powering the Lemon board from Citronics.

| Feature              | Description                                                       |
| -------------------- | ----------------------------------------------------------------- |
| CPU                  | 4x2.26 Ghz Qualcomm Snapdragon 801 (ARMv7)                        |
| GPU                  | Qualcomm Adreno 330 GPU @ 578 Mhz                                 |
| Memory               | 2 GB LPDDR3 RAM                                                   |
| Storage              | eMMC ~20 GB on userdata partition                                 |
| Linux kernel         | 6.1x.y linux-msm8x74 mainline fork                                |
| IEx terminal         | Built-in screen or USB keyboard                                   |
| GPIO, I2C, SPI       | Limited (led, vibration motor, but no external capabilities)      |
| ADC                  | No                                                                |
| PWM                  | No                                                                |
| UART                 | [See UART](#uart)                                                 |
| Display              | 5" IPS LCD 1080x1920px HD 446 ppi                                 |
| Camera               | Yes but not supported                                             |
| Ethernet             | No                                                                |
| WiFi                 | Yes                                                               |
| Bluetooth            | Yes, but no Elixir support [See Bluetooth](#bluetooth)            |
| Audio                | Yes but not supported                                             |
| Modem                | Yes 2G/3G/LTE dual SIM, but no Elixir support [See Modem](#modem) |

## WiFi devices

The base image includes firmware and drivers for the wcn36xx wifi device onboard.

1. Make sure you can access your FP2 via ssh or UART
2. Use `nmcli --ask dev wifi connect <YOURSSID>`

## UART

A UART port is available (`ttyMSM0`) but requires disassembling the phone and soldering wires on the motherboard. For more information, please refer to this [discussion thread](https://forum.fairphone.com/t/information-about-the-debug-connector-on-the-fp2/23746/2)

## Bluetooth

Bluetooth is supported through the BlueZ stack. This requires to start dbus and bluetoothd in an Elixir application. More Elixir testing is required.

## Modem

The modem is a qmi compatible device and is not yet fully supported in Nerves.

It requires udevd to be launched for the remoteproc to be detected. It also requires the rmtfs daemon to be launched in order to allow communication with the modem. The device is properly detected, works in buildroot itself, but still requires more work in order to be supported by Nerves.

1. Make sure you can access your FP2 via ssh or UART
2. Use `nmcli connection add type gsm ifname '*' con-name gsm apn <YOUR APN>`
3. Then type `nmcli connection up gsm`
