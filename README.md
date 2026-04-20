# 1. CS6983: Modern Storage System Course Project
    Tingyu Qu - 2026 Spring
    I have no other group members.

# 2. Base Environment
- OS: Ubuntu 22.04.5 LTS running under WSL2
- WSL2 Kernel
- Compiler: GCC 11.4.0

# 3. To set up the environment
   1) The Ubuntu 22.04 WSL2 instance already had the following installed:
      - gcc 11.4.0
      - fuse3 3.10.5-1build1 (FUSE 3.x runtime)
      - libfuse3-3 3.10.5-1build1 (FUSE 3.x shared library)

   2) Installing Additional Dependencies
      The FUSE runtime library was present, but the development headers required for compilation were missing. The following packages were installed:
      ```bash
      sudo apt update && sudo apt install -y libfuse3-dev pkg-config
      ```
      - `libfuse3-dev` (3.10.5-1build1): Provides the FUSE 3.x header files (notably `/usr/include/fuse3/fuse.h`) needed to compile FUSE-based file systems.
      - `pkg-config`: Used to automatically resolve compiler and linker flags for libfuse3 during the build process.

   3) Project Directory Structure
      ```text
      /mnt/d/CS6983/rdmafs/
      ├── rdmafs.c          # Main source file
      ├── rdmafs            # Compiled binary
      ├── test_rdmafs.sh    # Automated test script
      └── data/             # Object storage directory
      ```

   4) Key System APIs Used
      - POSIX Shared Memory: `shm_open`, `mmap` with `MAP_SHARED`
      - FUSE: Each "machine" runs as a separate FUSE server process, mounting a directory (e.g., `/tmp/mnt0`, `/tmp/mnt1`) that exposes the shared distributed file system.
      - GCC Atomic Built-ins: `__sync_bool_compare_and_swap`, `__sync_synchronize`
      - Standard File I/O: `open`, `write`, `pread`, `fsync`

# 4. Compiling Command
   1) The project is compiled as a single-file C program with the following command:
      ```bash
      gcc -Wall -o rdmafs rdmafs.c $(pkg-config --cflags --libs fuse3) -lpthread -lrt
      ```

# 5. Initial Manual Testing
   1) First create 2 mount directories. Note that the mount needs to be in the original Linux dir, don't put it in `/mnt/d`:
      ```bash
      mkdir -p /tmp/mnt0 /tmp/mnt1
      ```

   2) Check if there exists FUSE in WSL2:
      ```bash
      ls -la /dev/fuse
      ```

   3) If it exists, try running the first machine:
      ```bash
      cd /mnt/d/CS6983/rdmafs
      ./rdmafs -machine 0 -f /tmp/mnt0
      ```
      `f` means running frontend, making the printout and error directly visible for debugging.
      The printout should be like: 
      ```text
      Data dir: /mnt/d/CS6983/rdmafs/data 
      Machine 0: initialized shared memory 
      Machine 0: starting FUSE
      ```

   4) Don't close the terminal from last step, open a new terminal and enter WSL:
      ```bash
      ls -la /tmp/mnt0
      ```
      The printout should be like: `. ` and `..`
      Try:
      ```bash
      touch /tmp/mnt0/hello.txt
      ```
      Possible Errors:
      a) The printout: `touch: cannot touch '/tmp/mnt0/hello.txt': No space left on device`
      Stop FUSE (or Ctrl+C in the first terminal): 
      ```bash
      fusermount3 -u /tmp/mnt0
      ```
      Then clear up the old shared memory: 
      ```bash
      rm -f /dev/shm/rdmafs
      ```
      Clear up the old data: 
      ```bash
      rm -f /mnt/d/CS6983/rdmafs/data/*
      ```
      Restart: 
      ```bash
      cd /mnt/d/CS6983/rdmafs
      ./rdmafs -machine 0 -f /tmp/mnt0
      ```

   5) In terminal 2:
      Write in: 
      ```bash
      echo "hello world" > /tmp/mnt0/hello.txt
      ```
      Check if the file gets created: 
      ```bash
      ls -la /mnt/d/CS6983/rdmafs/data/
      ```
      The printout should be like: 
      ```text
      .
      ..
      m0.00001
      ```
      Read: 
      ```bash
      cat /tmp/mnt0/hello.txt
      ```
      See if writes and reads are functioning. The printout should be like: `hello world`

   6) In terminal 2:
      ```bash
      ./rdmafs -machine 1 -f /tmp/mnt1
      ```
      This starts machine 1.
      The printout should be like: 
      ```text
      Data dir: /mnt/d/CS6983/rdmafs/data 
      Machine 1: attached to existing shared memory 
      Machine 1: starting FUSE
      ```

   7) The roles of terminal when testing
      - **Terminal 1:** Machine 0. FUSE runs on it. No operations should be called on it.
      - **Terminal 2:** Machine 1. FUSE runs on it. No operations should be called on it.
      - **Terminal 3:** Linux user. All operations here (touch, echo, cat, ls, etc.). It determines which machine should be operated on by the mount dir.
        - Ops in `/tmp/mnt0/` -> calls to machine 0 on terminal 1.
        - Ops in `/tmp/mnt1/` -> calls to machine 1 on terminal 2.

# 6. Multi-step Testings
   1) Multi-file and overwrite in terminal 3
      ```bash
      echo "aaa" > /tmp/mnt0/a.txt
      echo "bbb" > /tmp/mnt0/b.txt
      ls -la /tmp/mnt0       # should at least see a.txt and b.txt in the printout
      cat /tmp/mnt0/a.txt    # should see aaa
      cat /tmp/mnt0/b.txt    # should see bbb
      echo "overwritten" > /tmp/mnt0/a.txt
      cat /tmp/mnt0/a.txt
      ```

   2) Subdirectory
      ```bash
      mkdir /tmp/mnt0/subdir
      echo "nested file" > /tmp/mnt0/subdir/deep.txt
      ls /tmp/mnt0/subdir
      cat /tmp/mnt0/subdir/deep.txt
      ```

   3) Delete
      ```bash
      rm /tmp/mnt0/b.txt
      ls /tmp/mnt0
      ```
      `b.txt` should be deleted.

   4) CORE VALIDATION: 
      Now mnt0 runs on terminal 1(machine 0), mnt1 runs on terminal 2(machine 1), in terminal 3:
      See if mnt1 can read the file that mnt0 writes in:
      ```bash
      cat /tmp/mnt1/hello.txt  # should see hello world
      cat /tmp/mnt1/a.txt      # should see overwritten
      ls /tmp/mnt1             # should see a.txt hello.txt subdir
      ```
      Still in terminal 3, see if mnt0 can read the file that mnt1 writes in:
      ```bash
      echo "from machine 1" > /tmp/mnt1/m1file.txt
      cat /tmp/mnt0/m1file.txt # should see from machine 1
      ```

# 7. Automated test scripts
If step 5. and 6. have been run, clear up before running 7.
Shut down mnt0 in terminal 1 and mnt1 in terminal 2, then run:
```bash
fusermount3 -u /tmp/mnt0 2>/dev/null
fusermount3 -u /tmp/mnt1 2>/dev/null
pkill -f rdmafs
rm -rf ./data
rm -f /dev/shm/rdmafs
```
Compile using 
```bash
gcc -Wall rdmafs.c -o rdmafs $(pkg-config --cflags --libs fuse3)
```
then start mnt0 by 
```bash
./rdmafs -machine 0 -f /tmp/mnt0
```
start mnt1 by 
```bash
./rdmafs -machine 1 -f /tmp/mnt1
```
then run test script:
```bash
bash test_rdmafs.sh
```
The testing should all pass, see test_result.txt.
