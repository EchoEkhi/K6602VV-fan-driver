# Asus Vivobook Pro 16X K6602VV Fan Driver

This is a Linux kernel module used to control the two onboard fans installed on the Asus Vivobook Pro 16X laptop (model K6602VV), with a standard hwmon API.

## DKMS install

Install this driver through DKMS:

```sh
cd K6602VV-fan-driver
sudo ./install.sh
sudo modprobe K6602VV_fan
```

Uninstall:

```sh
cd K6602VV-fan-driver
sudo modprobe -r K6602VV_fan
sudo ./uninstall.sh
```

## Development

Build:

```sh
cd K6602VV-fan-driver
make
```

Load:

```sh
sudo insmod K6602VV_fan.ko
dmesg | tail
```

For development on ASUS models other than K6602VV:

```sh
sudo insmod K6602VV_fan.ko allow_unsupported=1
```

Read sensors:

```sh
sensors
grep . /sys/class/hwmon/hwmon*/name
```
Look for K6602VV_fan in the grep output, and replace `hwmonX` with the correct hwmon number for the following commands:
```
cat /sys/class/hwmon/hwmonX/fan1_input
cat /sys/class/hwmon/hwmonX/temp1_input
```

Manual fan control:

```sh
# Set fan 1 (left fan) to manual mode, then about 80% duty.
echo 1 | sudo tee /sys/class/hwmon/hwmonX/pwm1_enable
echo 204 | sudo tee /sys/class/hwmon/hwmonX/pwm1

# Restore automatic firmware control for fan 1.
echo 2 | sudo tee /sys/class/hwmon/hwmonX/pwm1_enable
```

Unload:

```sh
sudo rmmod K6602VV_fan
```