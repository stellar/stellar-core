# Before creating an issue
  * check the [documentation](https://www.stellar.org/developers/)
  * ask for help
    * There is a [Stellar Stack Exchange](https://stellar.stackexchange.com/)
    * For more general questions, discuss with [the community](https://www.stellar.org/community/) (Slack, forums, Reddit, ...)
  * check existing and closed issues (you may have to remove the `is:open` filter)
    * check that the version that you are running is newer than the one with the fix
    * re-open an old issue that is supposed to be fixed; be sure to include additional details

Issues opened here are to report actual problems with stellar-core; any other issues (general questions, issues unrelated to stellar-core) will be closed (sorry - we have only a small community of people that manage issues).

## protocol changes
Note: a special category of issues are `protocol` related issues.

Those are typically requests to modify how the core protocol works (adding a new operation, changing the way transactions work, etc).

As stellar-core just implements the protocol, changes in the specification of the protocol has to be done via the [protocol repository](https://github.com/stellar/stellar-protocol/issues).


# Issue type
  bug/feature request/documentation/...

# Version found
  * git hash of the version you're running `git rev-parse HEAD`
  * version returned by `./stellar-core --version`

# Your environment and setup
  * Operating System name and version (usually `uname -a`)
  * Are you running from the command line? From a container?
  * Did you pass in special parameters when building the app?

# Issue description
 *describe what is broken and the steps you took*

## Actual result
 *what happened*

## Expected result
 *what should have happened*

## Supporting files

 Paste here or link to a gist/s3 bucket/etc material relevant to this issue:
   * configuration files (DON'T FORGET TO REMOVE SECRETS)
   * log files - best is to have logs running at DEBUG level (`-ll DEBUG` on the command line)


