## Before Creating an Issue

First, what kind of issue are you bringing up?

### Hey, I have a question!

Fantastic! However, the stellar-core issues repository is meant for reporting bugs and feature
requests related to stellar-core's implementation — if you have a question, we would recommend that
you:

* Take a look at Stellar's [developer portal][1], where you'll find comprehensive documentation
  related to Stellar.
* If you can't find an answer to your question, please submit a question to [Stellar's Stack
  Exchange][2].
* If your question is non-developer centric, take a look at [Stellar's Community][3].

In general, we close any issues that are questions best served elsewhere — we have a small
community of people that manages issues, and we want to ensure that the issues that remain open are
high quality (so we actually get around to implementing them!)

[1]: https://www.stellar.org/developers/
[2]: https://stellar.stackexchange.com/
[3]: https://www.stellar.org/community

### I'm fairly certain it's a bug.

* Please check existing and closed issues in Github! You may have to remove the `is:open` filter.
* Check the releases page to see if your issue has been fixed in a later release.
* When possible, re-open an old issue if there's been a regression. Be sure to include additional
  details on what was supposed to be working.

### I'd like to request new functionality in stellar-core!

First, you have to ask whether what you're trying to file is an issue related to Stellar's Protocol
OR if it's related to `stellar-core`, the C++ implementation that's in this repository.

Typically a request that changes how the core protocol works (such as adding a new operation,
changing the way transactions work, etc) is best filed in the [Stellar Protocol repository][4]

However, if your change is related to the implementation (say you'd like to see a new command line
flag or HTTP command added to stellar-core), this is the place.

* Please check existing and closed issues in Github! You may have to remove the `is:open` filter.
* Check the releases page to see if your request has already been added in a later release.

[4]: https://github.com/stellar/stellar-protocol/issues

## Issue Type
* Bug
* Feature Request
* Documentation

## Version
* If using a standard release, please add the version returned by `./stellar-core --version`
* If working off of master, please add the git hash of the version you're running using `git
  rev-parse HEAD`

## Your Environment and Setup
* Operation System & Version (You can usually obtain from `uname -a`):
* Are you running from the command line? From a container?
* Did you pass in special parameters when building the app?

## Issue description
*Describe your bug/feature request in detail.*

### Bug

#### Steps to Reproduce
*List in detail the exact steps to reproduce the unexpected behavior of the software.*

#### Expected Result
*Explain in detail what behavior you expected to happen.*

#### Actual Result
*Explain in detail what behavior actually happened.*

### Feature Request

*Explain in detail the additional functionality you would like to see in stellar-core.*

Be descriptive, including the interface you'd like to see, as well as any suggestions you may have
with regard to its implementation.

## Supporting Files

If you have any supporting files, please paste them or link to a relevant Github gist or archive
link (such as an S3 bucket).

Potential items to attach:
* Configuration files (**DON'T FORGET TO REMOVE SECRETS**)
* Log files - it is best is to have logs running at DEBUG level when reproducing an issue (`-ll
  DEBUG` on the command line).
