# l2boot
Amiga 600/1200 net-boot with a 3Com Etherlink III

l2boot allows an Amiga 600 or Amiga 1200 to use and boot from an HDF image or physical disk hosted by Linux.

The remote storage appears as:

```text
l2scsi.device unit 0
````

No custom Kickstart ROM or dedicated hardware are required (except the 3Com network card). The Amiga components are loaded at boot with `LoadModule`.

## Quick start

### Requirements

Amiga:

* Amiga 600 or Amiga 1200
* AmigaOS 3.x
* 3Com EtherLink III PCMCIA card
* `LoadModule`
* supplied `ptable.library`
* supplied `l2scsi.device`
* supplied `l2remote.boot`

Linux:

* any Linux computer (tested OK on VirtualBox)
* Ethernet connection to the Amiga
* root privileges
* GCC (apt-get build-essential)

A Raspberry Pi is convenient but not required.

### 1. Build the Linux server

```sh
wget https://github.com/Telefonorosso/l2boot/raw/refs/heads/main/l2boot-pi-hdf-server-v50-data1024-native-rdb-write-blockdev-noboot-auto-peer.c
```

Compile:

```sh
gcc -O2 -Wall -Wextra \
    -o l2boot-hdf-server \
    l2boot-pi-hdf-server-v50-data1024-native-rdb-write-blockdev-noboot-auto-peer.c
```

Make it executable:

```sh
chmod +x l2boot-hdf-server
```

### 2. Start the server

For an existing RDB HDF:

```sh
 ./l2boot-hdf-server eth0 disk.hdf
```

Replace `eth0` with the correct interface.

### 3. Load the Amiga modules

Run:

```text
wget https://github.com/Telefonorosso/l2boot/raw/refs/heads/main/l2scsi.device
wget https://github.com/Telefonorosso/l2boot/raw/refs/heads/main/ptable.library
wget https://github.com/Telefonorosso/l2boot/raw/refs/heads/main/l2remote.boot

LoadModule ptable.library l2scsi.device l2remote.boot
```
The system will reboot and scan for the server in the broadcast domain.

## Features

* boot from remote Amiga RDB disks;
* regular HDF files or Linux block devices;
* write support;
* multiple RDB partitions;
* HDToolBox partitioning and resizing;
* quick and full formatting;
* automatic host discovery;
* optional read-only mode;
* temporary non-booting mode;
* creation of new blank disks;
* Linux operation on Raspberry Pi, desktop PCs and virtual machines.

## Read-only mode

```sh
 ./l2boot-hdf-server eth0 disk.hdf --protect
```

Use this or make a backup of your valuable image beforehand!

## Prevent automatic boot

```sh
 ./l2boot-hdf-server eth0 disk.hdf --noboot
```

The disk remains visible and writable, but its partitions are presented temporarily with BootPri -10. The source disk is not modified.

## Create a blank HDF

Fully allocated 256 MiB image:

```sh
dd if=/dev/zero of=blank.hdf bs=1M count=256 status=progress
```

Sparse 1 GiB image:

```sh
truncate -s 1G blank.hdf
```

## Prepare a blank disk

Start the server once with:

```sh
 ./l2boot-hdf-server eth0 blank.hdf --allow-raw-write --noboot
```

Then:

1. Boot the Amiga and do LoadModule
2. Start HDToolBox with `l2scsi.device` unit `0`.
3. "Define" the drive.
5. Create and save the partitions (set a high bootpri!)
6. Reboot the Amiga
7. Format the partitions
8. Make some use of them
9. Restart the server without any option when ready:

```sh
 ./l2boot-hdf-server eth0 blank.hdf
```

## Linux block devices e.g. MICRO-SD CARDS

```sh
 ./l2boot-hdf-server eth0 /dev/sdb
```

Check the device first:

```sh
fdisk -l
```

Before exposing a block device:

* verify the device name;
* confirm it is not the Linux system disk;
* BE CAREFUL

For an initial read-only test:

```sh
 ./l2boot-hdf-server eth0 /dev/sdb --protect
```

## Direct DOS filesystem images e.g. FLOPPY IMAGES

Some images begin directly with `DOS\0` to `DOS\7` and contain no RDB.

By default, the server can expose compatible images through a temporary virtual RDB.


## Typical commands

Writable RDB image:

```sh
 ./l2boot-hdf-server eth0 l2boot.hdf
```

Read-only image:

```sh
 ./l2boot-hdf-server eth0 l2boot.hdf --protect
```

Visible but not preferred for boot:

```sh
 ./l2boot-hdf-server eth0 l2boot.hdf --noboot
```

Blank disk initialization:

```sh
 ./l2boot-hdf-server eth0 blank.hdf --allow-raw-write --noboot
```

Physical disk:

```sh
 ./l2boot-hdf-server eth0 /dev/sdb
```

## Linux virtual machines

The server has been verified under Debian running in VirtualBox on Windows.

Use bridged networking, not NAT.

The guest must be able to:

* receive Ethernet broadcasts;
* transmit custom Ethernet frames;
* open Linux `AF_PACKET` raw sockets;
* pass EtherType `0x88B8`;
* access the selected HDF or block device.

## Performance

Typical sequential transfer speed is approximately:

```text
400-480 KiB/s
```

Actual performance depends on the Amiga, PCMCIA timing, Linux host, Ethernet adapter and access pattern.

## Compatibility

Verified:

* Amiga 600;
* Amiga 1200;
* AmigaOS 3.x;
* Kickstart 40.68 with LoadModule;
* 3Com EtherLink III PCMCIA;
* native Amiga RDB images;
* multiple partitions;
* bootable remote partitions;
* HDToolBox;
* Linux regular files;
* Linux block devices;
* Raspberry Pi Linux;
* desktop Linux;
* Debian in VirtualBox on Windows.

Current limitations:

* ADF games with custom bootblock, copy protection, etc. will be UNREADABLE
* LoadModule is incompatible with CPU FASTROM (it hangs forever on a purple screen)
* hot removal (disk swapping) is unsupported.
* supported 3Com PCMCIA adapters only;

## Components

### `l2scsi.device`

The Amiga-side storage driver.

It initializes and directly controls the supported 3Com EtherLink III PCMCIA adapter, communicates with the Linux server over raw Ethernet and exposes the remote storage as:

```text
l2scsi.device unit 0
```

To AmigaOS and normal disk utilities, the remote HDF or Linux block device therefore behaves like a standard disk attached to `l2scsi.device`.

The driver handles normal disk reads and writes, geometry queries, SCSI commands, formatting requests, error handling and communication with the Linux host.

### `ptable.library`

Provides the RDB partition-table support used during the Amiga boot process.

Its job is to understand the Amiga Rigid Disk Block structures stored on `scsi.device` unit 0, locate the `PART` entries and create the corresponding AmigaDOS partition information.

`ptable.library` does not communicate with the Linux server and does not implement the Ethernet disk driver itself. It works on top of an already available block device.

### `l2remote.boot`

A small cold-start bootstrap module specific to L2BootHDF.

Its purpose is to connect the two previous components during early system startup.

After the LoadModule reboot, `l2scsi.device` becomes available first. `l2remote.boot` then invokes the RDB scanning facilities provided by `ptable.library` for `l2scsi.device`. This causes the remote RDB partitions to be registered early enough to participate in the normal Amiga boot process. `l2remote.boot` contains very little disk logic itself: it is mainly the startup glue that tells `ptable.library` which device and unit should be scanned.

### `l2boot-hdf-server`

The Linux-side raw-Ethernet storage server.

It opens the selected HDF file or Linux block device, answers requests from the Amiga and provides the remote disk contents over Ethernet.

It can serve native RDB images, Linux block devices and compatible direct-DOS filesystem images.

## Credits

LoadModule can be downloaded from Aminet:
https://aminet.net/package/util/boot/LoadModule

The CFD Project is hosted at:
https://aminet.net/package/driver/media/cfd.v20260614

l2boot - Francesco Talarico

ptable.library - Jaroslav Pulchart

Original compactflash.device/CFD project - Torsten Jager

LoadModule - Thomas Richter and Etienne Vogt

