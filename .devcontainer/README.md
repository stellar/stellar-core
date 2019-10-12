# Ubuntu 16.04 Remote Dev Container Instructions

## Who's interested in this and why?
* For those developers who'd like to validate their changes on Ubuntu 16.04, we provide a sandboxed development environment using the 'Remote Development Container' features of Microsoft's Visual Studio Code.
* This feature pre-configures a dev environment with all the tools you'll need to build stellar-core


## Pre-requisites: What do I need to use it?
* Install [Visual Studio Code](https://code.visualstudio.com/Download) for your platform
* Install the [Remote Development Extension Pack](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.vscode-remote-extensionpack)
* Install [Docker](https://www.docker.com) for your platform
  * If you are installing Docker on Windows, please make sure your settings have enabled Linux, rather than Windows containers.
* Special note for Windows users:
  * This feature mounts your git repository into the container so that you can use it from Linux.
  * If you are using Docker Desktop + Hyper-V (as opposed to new WSL2 features built into Windows version 10.0.18945.x and forward not covered here):
    * You will need to 'share your drive' with Docker. To do this in the most secure way, we recommend that you:
      * Create a dedicated local-only user for this purpose (as an admin): `net user DockerMount DockerMountUserPassword /add`
      * Grant read/write permission to that user to your git repository directory
      * When prompted (later) by Visual Studio Code, provide this user name and password.
      * In this way, your containers will only have limited rights to your machine.
    * As a matter of general security hygiene, we recommend against providing docker with pre-existing credentials to other existing users, especially admins.

## How do I use it?
- Enlist: `git clone https://github.com/stellar/stellar-core.git`
- Go to the code: `cd stellar-core`
- Open the directory in Visual Studio Code: `code .`
- Install the extensions listed in the pre-requisites if you haven't already.
- Invoke the container extension.
  - Hit F1
  - Run `Remote Containers: Reopen Folder in Container`
  - Wait for your container to be built
- Then go to Terminal => New Terminal, and it will automatically launch a new Linux shell in that container for you.

## How do I build once I'm in the container?
- `git submodule init`
- `git submodule update`
- Type `./autogen.sh`.
- Type `./configure`   
- Type `make` or `make -j<N>` (where `<N>` is the number of parallel builds, a number less than the number of CPU cores available, e.g. `make -j3`)
- Type `su vscode` to switch to a non-root user for test execution purposes.   (Tests that rely on postgresql will not run under root)
- Type `make check` to run tests.
- Type `make install` to install.

## Known issues
* Test failures on Windows:
  * bucket/test/BucketManagerTests.cpp:1310 will fail 
    * Failed to fsync directory stellar-core-test-52c416044e9cfd23/bucket :Invalid argument (FileSystemException.h:21)
    * Root cause - The host machine's git repository directory is shared with the docker container via CIFS, but CIFS does not implement directory fsync operations:
      * [CIFS operations without fsync](https://github.com/torvalds/linux/blob/69c902f597c4bec92013a526268620fb6255c24a/fs/cifs/cifsfs.c#L1168-L1176)
      * [NFS operations with fsync](https://github.com/torvalds/linux/blob/c971aa3693e1b68086e62645c54a087616217b6f/fs/nfs/dir.c#L63)
