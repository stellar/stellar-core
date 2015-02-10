Contributing:
  We're striving to keep master's history with minimal merge bubbles. To achieve this, we're asking PRs to be submitted rebased on top of master.
  To keep your local repository in a "rebased" state, simply run:

#changes the default for all future branches
git config --global branch.autosetuprebase always 
# changes the setting for branch master
git config --global branch.master.rebase true

note: you may still have to run manual "rebase" commands on your branches, to rebase on top of master as you pull changes from upstream.

[![Build Status](https://magnum.travis-ci.com/stellar/hayashi.svg?token=u11W8KHX2y4hfGqbzE1E)]

run tests with:
  bin/stellard --test

run one test with:
  bin/stellard --test  testName

run one test category with:
  bin/stellard --test '[categoryName]'


