# Net Reset Helper

This is a hardware solution to resetting your modem, router, and switches when your internet connection goes out.

A microcontroller monitors the connection, pinging one of several servers periodically and 

## Parts and Wiring

You need an Arduino Uno or Mega, an Arduino Ethernet shield, a normally closed relay, a DC power supply, wiring and an enclosure.

Here is a recommended arrangement:

![General Wiring Diagram](images/din.png)

## Process

Defaults to checking several sites in rotation, these are easily configurable by modifying the source.  Self assigns a random MAC address, which is not critical, but must be unique on the local network.


## Details

There is a detailed writeup on [functionality here](docs/README_MORE.md)