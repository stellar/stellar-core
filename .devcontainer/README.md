# Ubuntu 20.04 Remote Dev Container Instructions

## Who's interested in this and why?
* For those developers who'd like to validate their changes on Ubuntu 20.04, we provide a sandboxed development environment using the 'Remote Development Container' features of Microsoft's Visual Studio Code.
* This feature pre-configures a dev environment with all the tools you'll need to build stellar-core


## Pre-requisites: What do I need to use it?
* Install [Visual Studio Code](https://code.visualstudio.com/Download) for your platform
* Install the [Remote Development Extension Pack](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.vscode-remote-extensionpack)
* Install [Docker](https://www.docker.com) for your platform
  * If you are installing Docker on Windows, please make sure your settings have enabled Linux, rather than Windows containers.
* Special note for Windows users:
  * This feature mounts your git repository into the container so that you can use it from Linux.
  * If you are using Docker Desktop + Hyper-V (as opposed to new [WSL2](https://docs.microsoft.com/en-us/windows/wsl/wsl2-index) features built into Windows version 10.0.18945.x and forward not covered here):
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

In general, you build just like you would normally on Ubuntu.
Only difference is that tests that rely on postgresql will not run under root, so you need to run tests with a non priviledged account like `vscode`.
So before building etc, just run `su vscode` if needed.

### Build straight from the shared workspace
The shared folder is mounted in something like `/workspaces/stellar-core`.

Advantage of building from the shared folder is that you get to use all vscode features directly:
what you edit is what you build.

You may run into problems with various tools (git) that get confused by the hybrid setup.

For git when running from Windows, the following configuration helps (it tells git to use Linux line endings in the workspace):
```
[core]
        eol = lf
        autocrlf = input
```
NB: if you change this setting in an existing working folder, you need to reset it to have the proper line ending.
This can be done with the following sequence:
```
git checkout-index --force --all
git rm --cached -r .
git reset --hard
```

### Build in a different folder

On some platforms (Windows), there is a large overhead when building from the workspace folder (and if you're using WSL2 on Windows, cross OS file speed is [slower than WSL1](https://docs.microsoft.com/en-us/windows/wsl/wsl2-ux-changes#cross-os-file-speed-will-be-slower-in-initial-preview-builds)), the alternative
is to simply mirror your workspace into your home directory from within the container with a command such as
```
mkdir ~/src
(
  cd /workspaces/stellar-core/
  find .git -print
  echo .gitmodules
  git ls-files
  git submodule foreach --recursive --quiet 'git ls-files | xargs -I{} -n1 echo "$sm_path/{}"'
  git submodule foreach --recursive --quiet 'echo $sm_path/.git' | xargs -I{} -n1 find "{}" -print
) |
  rsync -a -HAX --files-from=- /workspaces/stellar-core/ ~/src/stellar-core
```

You can then use the new folder for building and testing (just `cd ~/src/stellar-core` and build like normal).

Note: `-C` cannot be used here as git files (the `.git` folder) are needed for `autogen.sh` to work properly.

Note (Windows): the `.git` folder may be invisible in the workspace, which causes mirroring to skip it. A workaround is to uncheck the "hidden" property from Windows explorer.

If you modify a few files in the host folder, a handful shortcut is to copy files as per git instead of syncing:
```
  git ls-files -m | xargs -I'{}' -n1 -- cp '{}' ~/src/stellar-core/'{}'
```

To mirror any changes done back into the workspace (note `-C` to skip git related files in this direction):
```
(
  cd /workspaces/stellar-core/
  git ls-files
  git submodule foreach --recursive --quiet 'git ls-files | xargs -I{} -n1 echo "$sm_path/{}"'
) |
  rsync --progress -v -ru -C --files-from=- ~/src/stellar-core /workspaces/stellar-core
```

## Known issues
* Test failures on Windows when building on the shared workspace:
  * bucket/test/BucketManagerTests.cpp:1310 will fail 
    * Failed to fsync directory stellar-core-test-52c416044e9cfd23/bucket :Invalid argument (FileSystemException.h:21)
    * Root cause - The host machine's git repository directory is shared with the docker container via CIFS, but CIFS does not implement directory fsync operations:
      * [CIFS operations without fsync](https://github.com/torvalds/linux/blob/69c902f597c4bec92013a526268620fb6255c24a/fs/cifs/cifsfs.c#L1168-L1176)
      * [NFS operations with fsync](https://github.com/torvalds/linux/blob/c971aa3693e1b68086e62645c54a087616217b6f/fs/nfs/dir.c#L63)
