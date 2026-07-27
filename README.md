# l2boot
Amiga 600/1200 net-boot with a 3Com Etherlink III

l2boot allows an Amiga 600 or Amiga 1200 to use and boot from an HDF image or physical disk hosted by Linux.

The remote storage appears as:

```text
scsi.device unit 0
````

No custom Kickstart ROM is required. The Amiga components are loaded at boot with `LoadModule`.

## Quick start

### Requirements

Amiga:

* Amiga 600 or Amiga 1200
* AmigaOS 3.x
* 3Com EtherLink III PCMCIA card
* `LoadModule` https://aminet.net/package/util/boot/LoadModule
* supplied `ptable.library` https://aminet.net/package/driver/media/cfd.v20260614
* supplied `scsi.device`
* supplied `l2remote.boot`

Linux:

* any Linux computer
* Ethernet interface connected to the Amiga network
* GCC
* root privileges

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

### 2. Start the server

For an existing RDB HDF:

```sh
 ./l2boot-hdf-server eth0 disk.hdf
```

Replace `eth0` with the correct interface.

### 3. Load the Amiga modules

Run:

```text
wget https://github.com/Telefonorosso/l2boot/raw/refs/heads/main/scsi.device
wget https://github.com/Telefonorosso/l2boot/raw/refs/heads/main/ptable.library
wget https://github.com/Telefonorosso/l2boot/raw/refs/heads/main/l2remote.boot

LoadModule ptable.library scsi.device l2remote.boot REVERSE
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

## Existing RDB image

```sh
 ./l2boot-hdf-server eth0 workbench.hdf
```

Native RDB images are writable by default.

## Read-only mode

```sh
 ./l2boot-hdf-server eth0 disk.hdf --protect
```

Use this for preserved, unknown or damaged images.

## Prevent automatic boot

```sh
 ./l2boot-hdf-server eth0 disk.hdf --noboot
```

The disk remains visible and writable, but its partitions are presented temporarily with:

```text
BootPri -10
```

The source disk is not modified.

## Create a blank HDF

Fully allocated 256 MiB image:

```sh
dd if=/dev/zero of=blank-256m.hdf bs=1M count=256 status=progress
```

Sparse 1 GiB image:

```sh
truncate -s 1G blank-1g.hdf
```

## Prepare a blank disk

A blank file has no RDB and is protected from writes by default.

Start the server once with:

```sh
 ./l2boot-hdf-server eth0 blank-256m.hdf --allow-raw-write --noboot
```

Then:

1. Boot the Amiga and do LoadModule
2. Start HDToolBox with `scsi.device` unit `0`.
3. "Define" the drive.
5. Create and save the partitions (set a high bootpri!)
6. Reboot the Amiga
7. Format the partitions
8. Make some use of them
9. Restart the server without any option when ready:

```sh
 ./l2boot-hdf-server eth0 blank-256m.hdf
```

## Linux block devices

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
 ./l2boot-hdf-server eth0 blank.hdf --allow-raw-write
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

## Components

### `scsi.device`

Exposes the remote storage as `scsi.device` unit `0`.

### `l2remote.boot`

Scans the RDB at boot and registers its partitions.

### `ptable.library`

Provides RDB partition scanning and boot-node support.

### `l2boot-hdf-server`

Linux raw-Ethernet storage server.

## Compatibility

Verified or targeted:

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

* supported 3Com PCMCIA adapters only;
* hot removal (disk swapping) is unsupported.
* ADF games with custom bootblock, copy protection, etc. will be UNREADABLE

## Credits

l2boot - Francesco Talarico

ptable.library - Jaroslav Pulchart

Original compactflash.device/CFD project - Torsten Jager

LoadModule - Thomas Richter and Etienne Vogt

```
```
