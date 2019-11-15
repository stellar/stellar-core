---
name: Bug Report
about: Create a report to help us improve stellar-core
title: "[Short Description] (Version: [stellar-core version])"
labels: bug
assignees: ''

---

## Read before creating an issue

In general, we close any issues that are
* unactionable (fill the template below under "Description")
* questions best served elsewhere

We have a small community of people that manages issues, and we want to ensure that the issues that remain open are high quality (so we actually get around to implementing them!).

### I have a question!

The stellar-core issues repository is meant for reporting bugs and feature requests related to stellar-core's implementation.

If you have a question, we would recommend that you take a look at Stellar's [developer portal][1], where you'll find comprehensive documentation related to Stellar.

If you can't find an answer to your question you can:
* submit a question to [Stellar's Stack Exchange][2].
* or ask one of [Stellar's communities][3].

[1]: https://www.stellar.org/developers/
[2]: https://stellar.stackexchange.com/
[3]: https://www.stellar.org/community/#communities

### I'm fairly certain it's a bug.

* Please check existing and closed issues in Github! You may have to remove the `is:open` filter.
* Check the [releases](https://github.com/stellar/stellar-core/releases) page to see if your issue has been fixed in a later release.
* When possible, re-open an old issue if there's been a regression. Be sure to include additional
  details on what was supposed to be working.

## Issue Description

*Describe your bug/feature request in detail.*

## Steps to Reproduce
*List in detail the exact steps to reproduce the unexpected behavior of the software.*

## Expected Result
*Explain in detail what behavior you expected to happen.*

## Actual Result
*Explain in detail what behavior actually happened.*

## Your Environment and Setup

### stellar-core Version
* If using a standard release, please add the version returned by `./stellar-core --version`
* If working off of master, please add the git hash of the version you're running using `git rev-parse HEAD`

### Environment
* Operation System & Version (You can usually obtain from `uname -a`)
* Are you running from the command line? From a container?
* Did you pass in special parameters when building the app?
* Please add any additional relevant configuration related to this bug report.

## Supporting Files

If you have any supporting files, please paste them or link to a relevant Github gist or archive link (such as an S3 bucket).

Potential items to attach:
* Configuration files (**DON'T FORGET TO REMOVE SECRETS**)
* Log files - it is best is to have logs running at DEBUG level when reproducing an issue (`-ll
  DEBUG` on the command line).

## Additional context
Add any other context about the problem here.
