# FileSyncDaemon

Linux daemon which synchronize two directories (source and destination)
Support recursive mode, so all files and subdirectories are synchronized too. All actions made by daemon are logged and accessible in `/var/log/syslog`

Example logs view
![image](https://github.com/Kaspek2480/FileSyncDaemon/assets/33702004/17d4a9f6-c5ee-4b54-82a1-32bcfee865f4)

## Usage

```shell
Usage: ./daemon sourcePath destinationPath [-d|--debug] [-R|--recursive] [-s=<sleep_time>|--sleep_time=<sleep_time>] [-B=<size_mb>|--big-file-size=<size_mb>]

Arguments:
    sourcePath        The path to the source directory.
    destinationPath   The path to the destination directory.

Options:
    -d, --debug              Enable debug mode.
    -R, --recursive          Synchronize directories recursively.
    -s, --sleep_time         The time in seconds to sleep between iterations. Default value is 10.
    -B:5, --big-file-size:5  Fize size when daemon will use mapping file

Example usage:
    ./Demon /home/user/source /home/user/backup -R -s=5
```

#### Useful commands

```shell
ps aux | grep Demon | grep -v grep | grep -v /bin/bash | awk '{print $2}' | while read pid; do kill -s SIGUSR1 $pid; done
```
command to send signal to daemon

```shell
head -c 5MB /dev/zero > ostechnix.txt
```
command to create 5MB file

```shell
sudo service rsyslog start
```
in WSL we must enable rsyslog service manually to see logs in /var/log/syslog

```shell
tail -5 /var/log/syslog
```
display last 5 system logs

## Installation

1. Clone the repo
   ```sh
   git clone https://github.com/Kaspek2480/FileSyncDemon
   cd FileSyncDemon
   ```
2. Build the application
   ```sh
   mkdir build
   cmake -DCMAKE_BUILD_TYPE=Release -S . -B build
   cmake --build build
   ```
3. Add executable permissions
   ```sh
   chmod +x Daemon
   ```
   
