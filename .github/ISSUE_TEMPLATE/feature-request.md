---
name: Feature Request
about: Suggest an idea for the stellar-core implementation of the Stellar Protocol
title: "[Short Description] (Version: [stellar-core version])"
labels: enhancement
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

### I'd like to request new functionality in stellar-core!

First, you have to ask whether what you're trying to file is an issue related to Stellar's Protocol
OR if it's related to `stellar-core`, the C++ implementation that's in this repository.

Typically a request that changes how the core protocol works (such as adding a new operation, changing the way transactions work, etc) is best filed in the [Stellar Protocol repository][4].

However, if your change is related to the implementation (say you'd like to see a new command line
flag or HTTP command added to stellar-core), this is the place.

* Please check existing and closed issues in Github! You may have to remove the `is:open` filter.
* Check the [releases](https://github.com/stellar/stellar-core/releases) page to see if your request has already been added in a later release.

[4]: https://github.com/stellar/stellar-protocol/issues

## Description
### Explain in detail the additional functionality you would like to see in stellar-core.

*Be descriptive, including the interface you'd like to see, as well as any suggestions you may have
with regard to its implementation.*

### Explain why this feature is important
*If it's related to some problem you're having, give a clear and concise description of what the problem is. Ex. I'm always frustrated when [...]*

### Describe the solution you'd like
*A clear and concise description of what you want to happen.*

### Describe alternatives you've considered
*A clear and concise description of any alternative solutions or features you've considered.*

### Additional context
*Add any other context about the feature request here, including any gists or attachments that would make it easier to understand the enhancement you're requesting.*
