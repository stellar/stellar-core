# How to contribute

We're striving to keep master's history with minimal merge bubbles. To achieve this, we're asking PRs to be submitted rebased on top of master.

To keep your local repository in a "rebased" state, simply run:

`git config --global branch.autosetuprebase always` _changes the default for all future branches_

`git config --global branch.master.rebase true` _changes the setting for branch master_

Note: you may still have to run manual "rebase" commands on your branches, to rebase on top of master as you pull changes from upstream.

## Finding things to work on
The first place to start is always looking over the current github issues for the project you are interested in contributing to. Issues marked with `help wanted` are usually pretty self contained and a good place to get started.
stellar.org also uses these same github issues to keep track of what we are working on. If you see any issues that are assigned to a particular person that means someone is currently working on that issue. 

Of course feel free to make your own issues if you think something needs to added or fixed.

# Basic quality checks

Please ensure that all tests pass before submitting changes. The local testsuite can be run as `make check` or `src/stellar-core --test`,
see [README](./README.md) for details on running tests.

Code formatting wise, we have a `.clang-format` config file that you should use on modified files.

Try to separate logically distinct changes into separate commits and thematically distinct commits into separate pull requests.

# Submitting Changes

Please [sign the Contributor License Agreement](https://docs.google.com/forms/d/1g7EF6PERciwn7zfmfke5Sir2n10yddGGSXyZsq98tVY/viewform).

All content, comments, and pull requests must follow the [Stellar Community Guidelines](https://www.stellar.org/community-guidelines/). 

Submit a pull request rebased on top of master

 * Include a descriptive commit message.
 * Changes contributed via pull request should focus on a single issue at a time.
 
At this point you're waiting on us. We like to at least comment on pull requests within one week (and, typically, three business days). We may suggest some changes or improvements or alternatives.
