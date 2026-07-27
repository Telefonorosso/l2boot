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
* `LoadModule`
* supplied `ptable.library`
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
gcc -O2 -Wall -Wextra \
    -o l2boot-hdf-server \
    l2boot-pi-hdf-server-v50-data1024-native-rdb-write-blockdev-noboot-auto-peer.c
```

### 2. Start the server

For an existing RDB HDF:

```sh
sudo ./l2boot-hdf-server eth0 disk.hdf
```

Replace `eth0` with the correct interface.

### 3. Load the Amiga modules

Run:

```text
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
sudo ./l2boot-hdf-server eth0 workbench.hdf
```

Native RDB images are writable by default.

## Read-only mode

```sh
sudo ./l2boot-hdf-server eth0 disk.hdf --protect
```

Use this for preserved, unknown or damaged images.

## Prevent automatic boot

```sh
sudo ./l2boot-hdf-server eth0 disk.hdf --noboot
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
sudo ./l2boot-hdf-server eth0 blank-256m.hdf --allow-raw-write
```

Then:

1. Boot the Amiga from another disk.
2. Start HDToolBox.
3. Select `scsi.device` unit `0`.
4. Define the drive if required.
5. Create and save the partitions.
6. Stop the Linux server.
7. Restart it without `--allow-raw-write`.
8. Reboot and format the partitions.

Normal use:

```sh
sudo ./l2boot-hdf-server eth0 blank-256m.hdf
```

Use `--allow-raw-write` only while creating the first RDB.

## Linux block devices

```sh
sudo ./l2boot-hdf-server eth0 /dev/sdb
```

Check the device first:

```sh
lsblk -o NAME,SIZE,MODEL,FSTYPE,MOUNTPOINTS
```

Before exposing a block device:

* verify the device name;
* confirm it is not the Linux system disk;
* unmount its Linux filesystems;
* ensure no other program is writing to it.

For an initial read-only test:

```sh
sudo ./l2boot-hdf-server eth0 /dev/sdb --protect
```

## Direct DOS filesystem images

Some images begin directly with `DOS\0` to `DOS\7` and contain no RDB.

By default, the server can expose compatible images through a temporary virtual RDB.

To disable this behavior:

```sh
sudo ./l2boot-hdf-server eth0 filesystem.hdf --raw
```

## Typical commands

Writable RDB image:

```sh
sudo ./l2boot-hdf-server eth0 workbench.hdf
```

Read-only image:

```sh
sudo ./l2boot-hdf-server eth0 workbench.hdf --protect
```

Visible but not preferred for boot:

```sh
sudo ./l2boot-hdf-server eth0 workbench.hdf --noboot
```

Read-only and non-booting:

```sh
sudo ./l2boot-hdf-server eth0 archive.hdf --protect --noboot
```

Blank disk initialization:

```sh
sudo ./l2boot-hdf-server eth0 blank.hdf --allow-raw-write
```

Physical disk:

```sh
sudo ./l2boot-hdf-server eth0 /dev/sdb
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

* one remote disk;
* `scsi.device` unit `0`;
* supported 3Com PCMCIA adapters only;
* hot removal during disk access is unsupported.

```
```
